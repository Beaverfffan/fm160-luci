# fm160-luci

为 **广和通 FM160（单模块）** 做的 OpenWrt/ImmortalWrt 管理套件：模块管理、信号显示、QMI/MBIM/ECM 三模式拨号、短信收发、基站锁频、GNSS。

针对的是一台路由器上只有一个 FM160 的场景，因此**不做多厂商通用框架**——所有取舍都服务于「稳定」这一个目标。

> 事实依据：用户提供的三份广和通官方文档，已抽取为可检索文本并整理成 `docs/AT-FACTS.md`。
> 设计取舍与实施路线：`docs/DESIGN.md`、`docs/PLAN.md`。

---

## 当前状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **M1** | AT 内核：端口发现、单属主队列、URC 事件、分级调度、静默窗、熔断、状态缓存、ubus 接口；身份/注网/信号/小区解析；LuCI 概览·信号·AT 调试三页 | ✅ **已完成** |
| M2 | 拨号（QMI/MBIM 原生 proto + ECM 自定义 proto）、重连阶梯、USB 模式白名单与切换回滚状态机 | ⚠️ **代码完成 + 主机侧两轮自测通过，真机不可验**：`25-usbmode-dial-test.sh` **341 checks / 0 fail**（`usbmode.c` + `net.c` 由真实编译器驱动，期望值有一半来自 node 真跑 `api.js` 的**独立实现**，不是抄副本）；`26-m2-structural-check.sh` **67 checks / 0 fail**（两个状态机、netifd proto、装机前检、ACL/菜单可达性——grep 级，只答「接线被拆掉会不会有人发现」）。**本机无 SIM ⇒ 拨号成功路径与模式切换都产生不出成功形态**，按设计模式切换永不自动执行。**.ipk 尚未编出、未装机** |
| M3 | 短信（PDU 编解码、收发、存储、长短信）。**原计划的 `sendat` prompt 补丁已判定不需要**，改为在 `atq.c` 内做两阶段事务 | ⚠️ **代码完成 + 主机侧自测通过 + 已装机验收，但真机收发不可验**：自测 104 checks / 0 fail，设备侧 `DEVICE VERIFY OK`；本机无 SIM，`AT+CMGS`/`AT+CMGL` 整条收发路径产生不出成功形态，只验到「无卡时优雅降级」。见 `docs/AT-FACTS.md` §12 |
| **M4** | 频段锁定 / 小区锁定 / 邻区 | ✅ **已完成并在真机验收**（band lock 写路径已验通；cell lock 只写不生效，因为没有传统复位模式，UI 如实说明） |
| **M5** | GNSS | ✅ **已完成并在真机验收**（`DEVICE VERIFY OK`；解析器主机侧自测 239 checks / 0 fail）。开引擎对连接性影响实测为 0；NMEA 走 AT 口，读路径已在真机跑过真实帧（8 句 / 184 B / 5 talker / 0 校验错）。本机有天线但室内恒 0 颗星，故「有定位」形态仍由独立校验和帧代偿。见 `docs/AT-FACTS.md` §11（§11.11 = 真机联调） |
| M6 | i18n、日志导出、CI | ⚠️ **代码完成 + 主机侧验收通过，未装机**：**i18n** `po/zh_Hans/fm160.po` **506 条**，覆盖 **482/482 (100%)** —— 全部 8 个页面/脚本 + 菜单标题 + fm160d 发布的两组标签，门禁**判失败**于任何未载入 po 的前端消息。其中 **16 条**带 `msgctxt`（与 luci-base 共享且译文不同的 msgid 会因 `lmo_load_catalog` 的链序让服务端与浏览器读到**相反**的 archive）；撞键扫描后共享键 29 个、译文不同 **0** 个。门禁 **78 项 0 失败**（真实 `po2lmo` 往返、真跑 `cbi.js` 哈希比对、提取基逐文件下限 canary、**`FM160_TRANSLATIONS` 与 `po/` 目录双向一致**、**`$(call)` 未被折行**）。**日志导出** 进程内 128 行环形日志 + 只读 `fm160 diagnostics` 返回纯文本支持包，`diag.c` 主机侧 **92 checks / 0 fail**（`tools/hosttest/diag-export-test.sh`），回复键、只读性与页面读取互检 **8/8**。**CI** `.github/workflows/check.yml` 在 ubuntu-latest 上跑 `tools/check.sh`（`STRICT=1`）——**未在真 GitHub runner 上实际跑过** |

**M1 已在真机上验证过一轮**（2026-09-18，H69K + USB 外接 FM160）：代码编译干净，模块身份、USB 模式、注网、信号、小区命令全部实测，凡与真机不符的解析逻辑都已修正，见 `docs/HARDWARE-PROBE.md`。

**M1 也已作为真实 OpenWrt 包编出 `.ipk`**（2026-09-18，iStoreOS 24.10 树 @ `b1bb87394452`，恰好是 H69K 在跑的那个版本）：三个包全部 `rc=0`，产物见下方「已实测的构建结果」。此前只有宿主机的 musl 编译检查，那次检查**没能发现**一个只在链接期暴露的缺失符号定义——所以这一环是必需的，不是可选的。

M2 之后的功能仍按约定「先出代码」，依赖设备实际能力处一律运行期探测 + 保守默认 + 显式告警，见 `docs/PLAN.md` §4。

---

## 目录结构

```
fm160-luci/
├── at-daemon/          传输层：vendor 自 QModem 的 ubus_at_daemon，src/ 逐字节未改
│                       —— 串口独占 / 请求队列 / 行事件分流 / URC 订阅 / 端口租约
│                       （出处与 blob 哈希见 NOTICE.md、at-daemon/NOTICE.md）
├── fm160d/             策略层：本项目原创（C / uloop / libubus）
│   ├── src/
│   │   ├── fm160d.h        类型与接口
│   │   ├── main.c          ubus 连接、事件订阅、配置、生命周期 —— 含进程内日志环（M6）
│   │   ├── atq.c           AT 优先级队列 + sendat 异步客户端 + 两阶段事务（prompt → payload）
│   │   ├── sched.c         分级轮询 / 抖动 / 退避 / 静默窗 / 熔断 / 端口发现
│   │   ├── state.c         状态快照 + sysfs 流量计数 + ubus 推送
│   │   ├── cmds.c          FM160 命令与解析（身份 / 注网 / 信号 / 小区 / GNSS+NMEA）
│   │   ├── pdu.c / pdu.h   SMS PDU 编解码（GSM7 / UCS2 / UDH 拼接）— 只依赖 libc，可单独主机侧测试
│   │   ├── sms.c           SMS（M3）：存储、去重、持久化、setup 状态机、收发、解析器
│   │   ├── usbmode.c/.h    USB 模式表 + 风险判定（M2）— 只依赖 libc
│   │   ├── net.c / net.h   拨号阶梯、PDP/地址解析（M2）— 只依赖 libc（+ usbmode.h）
│   │   ├── dialer.c        拨号状态机（M2）
│   │   ├── modesw.c        USB 模式切换状态机 + 回滚（M2）
│   │   ├── diag.c / diag.h 诊断支持包：状态 + 日志环 → 纯文本（M6）— 无 I/O、无全局、时钟由调用方传入
│   │   └── ubus_methods.c  ubus 对象 "fm160"（22 个方法）
│   └── files/etc/          init.d / config / hotplug.d / uci-defaults
├── luci-app-fm160/     表现层：本项目原创（LuCI JavaScript）
│   ├── htdocs/luci-static/resources/fm160/api.js
│   ├── htdocs/luci-static/resources/view/fm160/{overview,signal,cells,dial,gnss,sms,debug}.js
│   ├── po/zh_Hans/fm160.po     简体中文（140 条，编译成 fm160.zh-cn.lmo）
│   └── root/usr/share/{luci/menu.d,rpcd/acl.d}/luci-app-fm160.json
├── tools/              门禁（每个都能单独跑；`sh tools/check.sh` 是总入口）
│   ├── check.sh            总编排；STRICT=1 时 SKIP 记为失败（CI 用这个）
│   ├── cccheck/            真 libubox/libubus 头文件下的编译 + 符号表 + OBJS/版本一致性 + CRLF
│   ├── jscheck/            前端语法 + api.js 导出 + 「daemon/声明/授权」三清单契约
│   ├── i18n/               .po 形状、哈希一致、po2lmo 往返、跨目录撞键、覆盖率
│   ├── hosttest/           diag.c 的主机侧编译并运行测试（需要 Linux 上的 cc）
│   └── lib/tooling.sh      共用助手（路径转换、删除走 python 避开沙箱）
├── .github/workflows/  CI：ubuntu-latest 上跑 tools/check.sh（STRICT=1）
└── docs/{AT-FACTS.md, DESIGN.md, PLAN.md}
```

## 分层

```
LuCI JS ──ubus──▶ fm160d ──ubus──▶ at-daemon ──▶ /dev/ttyUSBx ──▶ FM160
```

**fm160d 是全机唯一允许发 AT 的东西。** 前端只读 `fm160d` 的缓存快照；rpcd 的 ACL 里**故意不授予 LuCI 访问 `at-daemon` 的权限**，从权限层面堵住绕过。

---

## 构建

### 0. 选哪棵树（先读这段）

本套件不依赖任何 target 专属设施，纯用户态 C + LuCI JS，**任何 OpenWrt 24.10 系衍生树都能编**。但「编出来能装到你手上那台机器」需要树与目标设备对齐：

| 载体 | target / 子目标 | profile | 说明 |
|---|---|---|---|
| **H69K（RK3568，iStoreOS）** | `rockchip/armv8` | `hinlink_opc-h6xk` | **推荐先走这条**。iStoreOS 有该 target，ImmortalWrt 没有 |
| GL.iNet 等 MT798x | `mediatek/filogic` | 视机型 | 仅作交叉验证，产物不能装到 H69K |

选 iStoreOS 的理由不只是「有 h69k target」：`h6xk` 在 iStoreOS 里是 **H66K/H68K/H69K 三机合一的 profile**（`target/linux/rockchip/image/legacy.mk`，被 `armv8.mk` 在末尾 `include`），所以一个 profile 覆盖整族。

**树要选到 commit 一级，不要用分支尖端。** iStoreOS 24.10 线一直在动（`istoreos-24.10` 分支尖端比你机器上跑的版本领先一百多个提交），而 `libubox` / `libubus` 的 ABI 一旦升级，编出来的 `.ipk` 就装不进旧镜像。用设备自报的版本反查源码 commit：

```sh
# 在设备上
cat /etc/openwrt_release     # DISTRIB_REVISION=r29631-b1bb873944 → 取 b1bb873944
```

```sh
# 在构建机上
git clone --branch istoreos-24.10 https://github.com/istoreos/istoreos.git src
cd src && git checkout b1bb87394452
```

参考点：H69K 官方镜像的 profile、feeds 与源码 commit 都能在
`https://fw.koolcenter.com/iStoreOS/h6xk/` 里查到（`24.10-config.seed`、
`24.10-feeds.conf`、`commit.buildinfo`），对拍时非常有用。

### 1. 放进源码树

```sh
cp -r fm160-luci/at-daemon   <tree>/package/ubus-at-daemon
cp -r fm160-luci/fm160d      <tree>/package/fm160d
cp -r fm160-luci/luci-app-fm160 <tree>/package/luci-app-fm160
```

（也可以做成一个 feed：`src-link fm160 /path/to/fm160-luci`，再 `./scripts/feeds update fm160 && ./scripts/feeds install -a -p fm160`。注意 feed 方式下 `fm160-luci` 根目录需要有一个 `Makefile` 或直接指向子目录。）

包目录名要与 `PKG_NAME` 一致：`at-daemon/` 的 `PKG_NAME` 是 **`ubus-at-daemon`**。

> **装到装了 QModem 的机器上会发生什么。** QModem 自己就带 `ubus-at-daemon`（H69K 上是 `3.0.2-r2`，只有 4 个 ubus 方法），我们把同一个上游项目 vendor 成了 `2026.09.18-vendored-r1`（11 个方法，多了 `lease_*` / `urc_*` 和行事件推送）。**包名相同是必然的，不是疏忽**：`fm160d` 连的是 ubus 对象 `at-daemon`，一个对象只能有一个提供者，两份不可能共存。
>
> 后果：`opkg install ubus-at-daemon` 会把它当作**同一软件的升级**替换掉 QModem 那份（我们的版本号更大）。配置文件 `/etc/config/ubus-at-daemon` 两边内容实测逐字相同，且已声明为 `conffiles`，所以不会丢配置；新版本同时保留了 QModem 用的脚本回调模型（`event_callback.c` 仍在），并向 `fm160d` 提供 `urc_register` 等新方法。
>
> 想回退就重装 QModem 那份：`opkg install --force-downgrade <qmodem的ubus-at-daemon>.ipk`。

### 2. 选包与 target

先定 target/profile，再选包。H69K 的最小 `.config`（写成文件再 `make defconfig`，比交互式 menuconfig 可复现）：

```
CONFIG_TARGET_rockchip=y
CONFIG_TARGET_rockchip_armv8=y
CONFIG_TARGET_rockchip_armv8_DEVICE_hinlink_opc-h6xk=y
CONFIG_PACKAGE_ubus-at-daemon=y
CONFIG_PACKAGE_fm160d=y
CONFIG_PACKAGE_luci-app-fm160=y
```

```sh
make defconfig
grep -E '^CONFIG_TARGET_.*(BOARD|SUBTARGET|DEVICE_)' .config   # 必须断言 target 没被静默降级
```

交互式的话：`Utilities → fm160d`、`LuCI → 3. Applications → luci-app-fm160`。

⚠️ `make defconfig` 在依赖缺失时会**静默掉回默认 profile**，所以上面那行 `grep` 不是可选的。

### 3. 内核/用户态依赖（必须同时进镜像）

| 依赖 | 用途 |
|---|---|
| `kmod-usb-serial-option` | AT 串口（**FM160 的 VID:PID 必须在 option 的 id 表里**） |
| `kmod-usb-net-qmi-wwan` + `uqmi` | QMI 拨号（M2） |
| `kmod-usb-net-cdc-mbim` + `umbim` | MBIM 拨号（M2） |
| `kmod-usb-net-cdc-ether` | ECM 拨号（M2） |
| `libubus` `libubox` `libblobmsg-json` `libjson-c` | fm160d / at-daemon |
| `ubus` `rpcd` `luci-base` | 前端 |

**不要安装**：`kmod-usb-net-rndis`、`kmod-usb-net-cdc-ncm`、`ModemManager`（会抢 AT 口）。`fm160d` 启动时会检测 ModemManager 并打警告。

### 4. 编译

只编这三个包时不需要编内核，但**需要先有交叉工具链和 staging 里的 libubox/libubus**，否则 `Build/Compile` 找不到头文件和库：

```sh
make tools/install     -j$(nproc)      # host 工具
make toolchain/install -j$(nproc)      # 交叉工具链（最慢的一步）
make package/ubus-at-daemon/compile   V=s
make package/fm160d/compile           V=s
make package/luci-app-fm160/compile   V=s
```

产物在 `bin/packages/<arch>/base/`（24.10 线是 `.ipk`，25.12 线起是 `.apk`）。

内存提示：OpenWrt 的 gcc 引导阶段每个 job 可能吃掉几百 MB。H69K 这轮实测机是 16 线程 / 7 GB 内存，`-j8` 稳妥，`-j$(nproc)` 会开始换页。

### 5. 已实测的构建结果（2026-09-18）

载体：iStoreOS `istoreos-24.10` @ `b1bb87394452`，target `rockchip/armv8`，profile `hinlink_opc-h6xk`，`-j8`。

| 阶段 | 耗时 | 结果 |
|---|---|---|
| `make tools/install` | 8 min | rc=0 |
| `make toolchain/install` | 11 min | rc=0 |
| `make package/ubus-at-daemon/compile` | 14 s | rc=0 |
| `make package/fm160d/compile` | 13 s | rc=0 |
| `make package/luci-app-fm160/compile` | 25 s | rc=0 |

产物（`bin/packages/aarch64_generic/base/`）：

```
fm160d_0.1.0-r1_aarch64_generic.ipk              17856 B
ubus-at-daemon_2026.09.18-vendored-r1_...ipk     16965 B
luci-app-fm160_0.1.0-r1_all.ipk                  10641 B
```

核对过的事实：

- `fm160d` 是 **ELF64 / AArch64 / EXEC**，`DT_NEEDED` = `libubus.so.20250102`、`libubox.so.20240329`、`libblobmsg_json.so.20240329`、`libgcc_s.so.1`、`libc.so`——与设备上已装的库**版本号完全一致**，所以依赖可满足。
- `control` 的 `Depends` 解析正确：`fm160d` → `libc, libubus20250102, libubox20240329, libblobmsg-json20240329, ubus-at-daemon`。
- `fm160d` / `ubus-at-daemon` 的 `control.tar.gz` 里都有 `conffiles`（`/etc/config/fm160`、`/etc/config/ubus-at-daemon`），即配置在 `opkg upgrade` 时受保护。

> ⚠️ `conffiles` **不是** `control` 里的一个字段。`scripts/ipkg-build` 把它作为单独文件放进 `control.tar.gz`，opkg 安装时再抽成 `/usr/lib/opkg/info/<pkg>.conffiles`。去 `control` 里 grep `Conffiles:` 永远是空的，别据此以为声明没生效。

> ⚠️ 唯一剩余警告来自 vendor 的 QModem 代码（`ubus-at-daemon` 的 `main.c:3` 无条件 `#define ARRAY_SIZE`，而 `libubox/utils.h` 用 `#ifndef` 守卫）。两个定义逐字相同，属噪声；不修是有意的——改了会破坏 `at-daemon/NOTICE.md` 里的逐字节溯源。**fm160d 本身零警告。**

复现与重编的脚本在 `_tools/istoreos-h69k/`（`01-clone` → `07-verify-artifacts`）。改完 C 源码不必重跑 20 分钟的树构建：`04-sync-packages.sh` 把三个包目录推过去，`06-rebuild-packages.sh` 只重编包，一轮约 1 分钟。

---

## 上机后的取证清单

因为代码没在真机上跑过，第一次上机请按顺序抓这些，很多判断要靠它们：

```sh
# 1. 模块有没有被 option 驱动认出来
ls /sys/bus/usb/drivers/option/ | grep -i usb
cat /sys/bus/usb/devices/*/idVendor | sort -u          # 期望能看到 2cb7

# 2. 当前 USB 模式和设备自报的支持集（决定白名单实际取值）
ubus call fm160 at '{"cmd":"AT+GTUSBMODE?"}'
ubus call fm160 at '{"cmd":"AT+GTUSBMODE=?"}'

# 3. fm160d 自己的状态
ubus call fm160 status | jsonfilter -e '@.port' -e '@.at_state' -e '@.usbmode'
logread -e fm160d | tail -40

# 3b. 支持包（M6）。★ 这是**排障入口**：状态 + fm160d 自己记住的日志，
#     一次调用全拿到，且**不碰模组**（不会改动静默窗或打乱轮询时序），
#     所以它可以在一台已经在出问题的机器上安全地执行。
#     文本里含 IMEI / SN / ICCID，公开贴出前要删。
ubus call fm160 diagnostics | jsonfilter -e '@.bytes' -e '@.log_lines' -e '@.log_total'
ubus call fm160 diagnostics | jsonfilter -e '@.text' | sed 's/\\n/\n/g'

# 4. 短信：先看 setup 有没有被模组接受（无卡时这一步就会失败，属正常）
ubus call fm160 status | jsonfilter -e '@.sms'

#    列举一次模组存了什么。★ 这是**按钮语义**，不要放进任何循环：
#    无卡时 AT+CPMS? 要 10.3 秒才回 ERROR，会把串口上的其它轮询全部饿死。
ubus call fm160 sms_sync
ubus call fm160 sms_list

# 5. 长 PDU 是否稳定（判断要不要 ZLP 内核补丁）—— 看这两个数，不是看总失败率。
#    long_timeout 非零才是「多段发送在这颗内核上真的会挂」的证据。
ubus call fm160 status | jsonfilter -e '@.sms.counters'

# 6. 拨号激活到底用哪条命令（M2 需要）
ubus call fm160 at '{"cmd":"AT+GTWWAN=?"}'
ubus call fm160 at '{"cmd":"AT+GTRNDIS=?"}'
```

---

## 设计要点速查

| 主题 | 做法 |
|---|---|
| AT 并发 | 全机单飞：任意时刻 ≤ 1 条在飞；优先级「交互 > 状态机 > 轮询」；轮询等待 > 30 s 自动提升防饿死 |
| 轮询分级 | 注网 5 s（前台）/15 s（空闲）；小区 10 s 仅前台；流量读 `/sys` **0 条 AT**；身份类命令**不进轮询** |
| 抗干扰 | 每级 ±20% 抖动；熔断：连续 3 次超时→降速+静默 60 s，10 次→停自动轮询 |
| 静默窗 | 拨号 / CFUN / 模式切换 / COPS 扫描 / 手动 AT 期间挂起全部轮询 |
| URC | 事件驱动（`qmodem.at.urc` / `qmodem.at.line`），按 `correlation` 区分命令响应与真 URC；`drop_count` 监测丢行 |
| 模式切换 | 真机 `AT+GTUSBMODE=?` = **{17,18,20,21,24,29,30,31,32,33}**（真机实测，非手册推演）；硬黑名单 20/24/31——无 AT 口、切进去回不来，且真机**确实**支持这三个，属真·单向门；候选 QMI{32,17} MBIM{30,29} ECM{33,18}；写入前后回滚状态机（M2） |
| 信号显示 | 主数据源 `AT+GTCCINFO?`——一条命令拿到服务小区 + 最多 10 邻区。⚠️ 它是**多行带文本标签**、且 `tac/cellid/earfcn/pci` 是**十六进制**，按 CSV 十进制解析会在真机上静默返回空 |
| 短信（M3） | **绝不进轮询表**：无卡时 `AT+CPMS?` 10 345 ms、`AT+CMGF?` 5 373 ms，而 `AT+CSQ` 只要 24 ms；串口是串行的 ⇒ 一条慢命令饿死后面全部。只由**按钮 + `+CMTI` 事件**驱动，LuCI 短信页因此是本项目唯一不 `poll.add()` 的页面。发送是**两阶段**（`end_flag:">"` → `raw_at_content: PDU+0x1A`），两阶段在**同一个队列项内**完成以防轮询插入。删除**同时**动模组（`AT+CMGD`）与本地两份副本，且墓碑要落盘，否则重启复活 |

---

## 许可证

两部分，许可不同，**注意区分**（详见 `NOTICE.md`）：

| 范围 | 许可 | 说明 |
|---|---|---|
| `at-daemon/` | **MPL-2.0 + 禁止商用** | vendor 自 [FUjr/QModem](https://github.com/FUjr/QModem) `application/ubus_at_daemon` @ `86102c2a6f62`，`src/` **逐字节未改**（哈希见 `at-daemon/NOTICE.md`）。禁商用条款是**上游加的**，随代码一起传递 |
| `fm160d/`、`luci-app-fm160/`、`docs/`、以及 `at-daemon/` 的打包部分 | **MPL-2.0** | 本项目原创，无附加条款 |

`fm160d` 与 `ubus-at-daemon` 之间是 **ubus 进程边界通信、不链接**，所以许可不会从 vendored 部分传染到原创部分。

上游 `LICENSE` 原文保留在 `at-daemon/LICENSE`；仓库根 `LICENSE` 是同一份文本去掉上游附加条款后的 MPL-2.0。

本仓库**不包含**：`_probe/`（含真机 IMEI/SN）、广和通三份官方文档（版权属广和通）、QModem 的 Lua/LuCI 应用代码。
