# fm160-luci — 项目决策与实施路线

## 0. 已确认的前提（用户答复）

| 项 | 决定 |
|---|---|
| 目标固件 | **ImmortalWrt 25.x** |
| 硬件连接 | **USB 转接板外接 FM160** |
| 联调方式 | **先只出代码，暂不真机联调** ⇒ 一切设备相关取值必须运行期探测，不得硬编码 |
| 许可证 | 个人使用，不在意 GPL/商用限制 ⇒ **允许复用 QModem 组件** |

「暂不联调」是本次设计最强的一条约束：**所有只能靠真机确定的量，代码里必须写成探测 + 保守默认 + 显式告警**，不允许假定。见 §4 的「未知量处理表」。

## 1. 关键架构决策

### D1 — 复用 `ubus-at-daemon` 作为 AT 传输层（不改它）

调研结论：QModem 的 `application/ubus_at_daemon`（2485 行 C）已经是我们要的东西，而且做得比预期好：

| 已有能力 | 对本项目的价值 |
|---|---|
| 每端口独立 reader 线程 + 队列 mutex/cond + write mutex | 串口读写不互相踩 |
| `lease_acquire/renew/release`（带 TTL 的端口租约） | 显式声明「本机只有 fm160d 用这个口」 |
| `urc_register(port, owner, urc_id, prefix)` | 按前缀订阅 URC，不影响其它消费者 |
| **`ubus_send_event("qmodem.at.line", …)` 逐行全量推送** | 被动拿到所有行，不需要轮询 |
| **`ubus_send_event("qmodem.at.urc", …)` 按前缀推送** | 直接拿 `+CMTI` 等 |
| `correlation` ∈ IDLE/RESPONSE/TERMINAL/AMBIGUOUS | **区分「命令响应」与「真 URC」**——这正是轮询最容易搞错的地方 |
| `restart_epoch` / `sequence` / `drop_count` | 能检测队列溢出丢事件 |
| 端口 monitor 线程 + 自动重连 | 拔插模块自愈 |
| `tests/test_port_concurrency.c`、`test_reader_flood.c` | 有并发/洪泛测试背书 |

⇒ **零改动复用**。`fm160d` 只做策略层。

### D2 — 分层：传输层 / 策略层 / 表现层

```
LuCI JS ──ubus──▶ fm160d（策略层：状态机·调度·语义·解析）
                    │  ubus call at-daemon sendat / lease_acquire / urc_register
                    │  ubus subscribe qmodem.at.urc / qmodem.at.line
                    ▼
              at-daemon（传输层：串口独占 + 队列 + 行事件）
                    │ /dev/ttyUSBx
                    ▼
                  FM160
```

### D3 — 唯一的 AT 发起者

`fm160d` 内部维护一个 **优先级队列 + 单飞（one in flight）** 的 AT 客户端：
- 优先级：`0 交互（用户点击）` > `1 状态机（拨号/切换/短信）` > `2 轮询`
- 同命令去重（合并回调）
- 优先级 2 等待 > 30 s 自动提升为 1（防用户连点把轮询饿死）
- **任何组件都不允许绕过 `fm160d` 直接调 `at-daemon`**（ACL 里把 `at-daemon` 只授予 `fm160d` 的 ubus 用户）

### D4 — 短信发送需要一个**传输层补丁**（唯一需要动 at-daemon 的地方）

`AT+CMGS=<len>` / `AT+CMGW=<len>` 是两阶段事务：先等 `> ` 提示符，再写 PDU + `0x1A`。
而 `sendat` 是「发命令 → 等终态」的一次性模型，且**同端口串行** ⇒ 第二条 `sendat` 会排在第一条后面，形成死锁（第一条在等一个永远不来的终态）。

方案：给 `sendat` **追加式**加一个可选字段（不改现有语义）：

```
sendat {
  at_port, at_cmd, timeout,
  prompt: "> ",        // 新增：等到该字面量后，再写 payload
  payload_hex: "…",    // 新增：提示符到达后写出的裸字节（十六进制）
  end_flag: "+CMGS"    // 写出后期待的终态
}
```

约 30 行改动，落在 `at_handler.c`。**在补丁就位前**，SMS 走降级路径（见 §4）。

### D5 — 不引入 QModem 的 UI 与脚本层

它的 LuCI 页面、`cmds/*.sh`、`modem_ctrl.sh` 是通用多厂商框架，与「只适配 FM160、追求稳定」相冲突。**只复用 C 传输层，其余全部原创。**

## 2. 模块划分

```
fm160-luci/
├── at-daemon/            ← 复用 QModem application/ubus_at_daemon（保留其 MPL 头与出处说明）
│   └── patches/0001-sendat-prompt.patch    ← D4 的加法补丁
├── fm160d/               ← 原创策略层（C / uloop / libubus）
│   ├── src/
│   │   ├── fm160d.h        类型与接口
│   │   ├── main.c          ubus 连接、事件订阅、端口发现、启动自检
│   │   ├── atq.c           AT 优先级队列 + sendat 异步客户端
│   │   ├── sched.c         分级轮询 + 抖动 + 退避 + 静默窗 + 熔断
│   │   ├── state.c         状态快照 + 变更推送
│   │   ├── cmds.c          FM160 命令与解析（身份/注网/信号/小区）
│   │   └── ubus_methods.c  ubus 对象 fm160 的方法
│   └── files/etc/{init.d,config,uci-defaults,hotplug.d}
├── luci-app-fm160/       ← 原创 LuCI JS
│   ├── htdocs/luci-static/resources/{view/fm160/*.js, fm160/*.js}
│   └── root/usr/share/{luci/menu.d,rpcd/acl.d}/*.json
└── docs/{AT-FACTS.md, DESIGN.md, PLAN.md}
```

## 3. 里程碑

| 期 | 内容 | 本期状态 |
|---|---|---|
| **M1** | `fm160d` 核心：端口发现、AT 队列、事件订阅、分级调度、静默窗、熔断、状态缓存、ubus 接口、身份/注网/信号/小区解析；LuCI 概览页 + 信号页 + AT 调试页 | **本次交付** |
| **M2** | 拨号：QMI/MBIM 原生 proto 接入 + ECM 自定义 proto + 重连阶梯 + **模式切换白名单/回滚状态机** | 待做 |
| **M3** | 短信：PDU 编解码（GSM7/UCS2/UDH）、收发、存储、长短信、`sendat` prompt 补丁 | 待做 |
| **M4** | 锁频 / 锁小区 / 基站：动态能力枚举、一键锁当前小区、邻区表、CA | **后端 + UI 已交付并真机只读验收**（`DEVICE VERIFY OK`）；写路径已实现且**写前可干跑**（`_tools/istoreos-h69k/20-m4-dryrun.sh`），但两个持久写**均未在真机写入** —— 待用户决定，见 `AT-FACTS.md` §9 |
| **M5** | GNSS：开关、卫星组合、NMEA 解析、卫星图、可选 TCP 转发 | 待做 |
| **M6** | 打磨：i18n、日志导出、CI | 待做 |

## 4. 未知量处理表（**因为没有真机，这节最重要**）

| 未知量 | 代码怎么处理 | 失败时的表现 |
|---|---|---|
| 哪个 `ttyUSB*` 是 AT 口 | 枚举宿主 USB 里 `idVendor == 2cb7` 的串口 → 逐个 `sendat "AT"` 探测 | 找不到 → `port_found=0`，UI 黄条提示，5 s 后重试（低频） |
| 当前 USB mode | `AT+GTUSBMODE?` 回读；**绝不用 VID:PID 推断** | 查询失败 → 显示 unknown + 提示 |
| 支持哪些 USB mode | `AT+GTUSBMODE=?` 取设备列表，与我们的白名单取交集 | 取不到 → **禁止一切模式切换**（只读） |
| 拨号激活命令是 `+GTWWAN` 还是 `+GTRNDIS` | M2 实现：先 `AT+GTWWAN=?`，ERROR 再试 `AT+GTRNDIS=?`，结果缓存到运行时 | 都失败 → 拨号报错并给出原始响应 |
| 是否需要 ZLP 内核补丁 | 代码侧不做假设；M3 起在 SMS 发送路径**统计长 PDU 超时率**并上报，作为是否打补丁的判据 | 超时率高 → UI 明确提示「疑似缺 ZLP 补丁」 |
| CESQ/GTCCINFO 的字段差异 | 解析器按 `+GTCCINFO:` 行内 `<rat>` 字段分支；未知字段容错跳过 | 解析失败 → 保留上一次有效值并标记 stale |
| 短信存储位置 | `AT+CPMS=?` 探测；`ME` 优先，回退 `SM` | 都不支持 → 短信功能置灰 |
| GNSS 是否可用 | 先 `AT+GTGPSPOWER?`，ERROR 则功能置灰 | — |

## 5. 「AT 脆弱」的落实清单（对照设计原则逐条可查）

| 编号 | 措施 | 落点 |
|---|---|---|
| A1 | 单飞：任意时刻 ≤ 1 条 AT | `atq.c` |
| A2 | 轮询分 6 级，间隔 5–300 s，带 ±20% 抖动 | `sched.c` |
| A3 | 失败退避 ×2，上限分级封顶 | `sched.c` |
| A4 | 熔断：连续 3 次超时 → 停轮询 60 s；10 次 → 停自动轮询 | `sched.c` |
| A5 | 静默窗：拨号 / CFUN / 模式切换 / COPS 扫描 / 手动 AT | `sched.c` |
| A6 | **流量统计读 `/sys/class/net/*/statistics`，0 条 AT** | `cmds.c` |
| A7 | URC 优先：`+CMTI`/`+CEREG`/`+C5GREG`/`+CGEREP` 事件驱动 | `main.c` |
| A8 | 慢变命令（CGMI/CGMM/CGSN/CCID/GTUSBMODE?）**不进轮询** | `cmds.c` |
| A9 | 优先级 2 饥饿保护（> 30 s 提升） | `atq.c` |
| A10 | 队列积压超过水位时**丢弃本轮轮询**并计数 | `sched.c` |
