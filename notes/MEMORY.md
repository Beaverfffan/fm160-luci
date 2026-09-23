# GL.iNet / x-wrt 固件开发 — 速查索引

> 细节 → `BE14000-fixes/MEMORY-FULL-ARCHIVE.md`（先读 §0）。★ 本文件**不设体积上限**（原「≤6 KB」约定已作废，用户 2026-09-21 明确）⇒ 速查条目直接写全，完整推导仍进存档

进行中：MT5000 PPE；MLO 在线。★★★★★ **v7：保活 + 统一日志 + 无线交叉验证**〔`2026-09-23.md` §25〕——fm160-keepalive（30s 巡检模块 DNS、5 轮失败 CFUN 重载、24h/10 轮自动放弃，双 drill 过）+ fm160-oplog 语义化环形日志（ctl/keepalive/proto/UI 写操作全接入，logs.js 页面 3s 轮询）+ 无线交叉验证（**GTCCINFO/GTCAINFO 的 earfcn/pci/tac/cellid 是 hex**；QCI 本固件不上报；COPS 补运营商）+ 修 v6 的 proto_block_restart 裸调用与 mbim 看门狗未入构建树两回归；单测 43 断言全过 + 路由器 overlay 部署实测；镜像 `fm160-luci/istoreos-h69k-fm160-v7.img.gz`。教训：**overlay 手工部署会被刷机遮蔽，构建树同步必须逐一核验**。★★★★★ **三协议已重构为独立 luci-proto 包**（`luci-proto-{mbim,qmi,ecm}-fm160`，CLI 实现非原生 QMI/MBIM）〔`2026-09-23.md` §24〕：netifd 铁律 init_proto 三连 / raw-ip no_device=1 / proto 脚本重启才扫描；**v4 固件已刷机验证**（MBIM 自启 + v4v6 + LAN /62 PD + wan zone），main tip `a7190a3`，镜像 `fm160-luci/istoreos-h69k-fm160-v4.img.gz`。遗留：ECM v6 内核 SLAAC 不上报（接口页不显示 v6）；QMAP 固件单流彩票未解。★★★ FM160-QMI 已收口〔§33–38〕。★★★★★ ECM v6 已打通并过冷启动〔§39.20〕；`uci_bool()` 判断反了已修〔§39.21〕。★★★★★ **LAN 全球 v6 下发已落地为插件**〔§40〕：NPTv6 出局（`.table` 硬编码 mangle -150，晚于 conntrack -200；legacy ip6tables 无 raw 表 ⇒ 无配置可解）⇒ 正解 = 运营商 /64 挂 br-lan（metric 100）+ **odhcpd ndp relay**。★★★★★ **默认已翻转**（`dial_pdp 'IPV4V6'` + `lan_ipv6 '1'`，`dial_autostart` 刻意留 0）+ **四入口**（新增 `withdraw [device]`）+ **`ndproxy_routing='0'`**〔§41〕。★★★★★ **CI 首次全绿**：`Beaverfffan/fm160-luci` main tip `5658af0`。链条：①i18n 判据池子错（`46fb681`）②让 cccheck 失败时自证（`51a9632`）⇒ 自证一次说清三条：`modesw.c` 死代码（`6be59b5`）、**zig 缓存热时重放不出诊断**（`2d320ca`）、**`$((` 是算术展开 ⇒ `jscheck` 在 dash 下从未跑过**（`5658af0`）〔§43–44〕。★★★★★ **短信：格式不是变量，制式才是**（`2026-09-22.md` §13）——2×2 全测：NR SA 下 PDU/text 都在 **~100 ms 本地秒拒**（103/93 ms）；LTE 下都在 **40 s T1_RP 超时**（40013/40014 ms）⇒ **换格式零产额**；**要发只能在 LTE 发**（NR SA 下模组根本不提交）。★★★★★ **「重启整机」≠「重启模组」**：M.2 槽供电不随 SoC 断 ⇒ 必须 `AT+CFUN=1,1` 才算清场（重枚举 ≈21 s，**ttyUSB 编号会变**：AT 口 `ttyUSB2`→`ttyUSB4`）。★★★★★ **真缺口：模组复位后插件 SMS 布防陈旧**〔§13.2〕——NV 把 `CNMI` 打回 `0,0,0,0,0`、`CPMS` 打回 `SM`，而 `probe_cb()` 不清 `sms.setup_done` ⇒ **永久收不到 `+CMTI`**（建议在 `fm160_cmd_ident_start()` 里作废；本次已手工补回 CNMI/CPMS）。现场：**LTE 已锁**（`GTACT=2`，原值已记，可写回 5G）；发送额度 **4/10（余 6）**。★★★★★ **外部判据「没收到」之后已把范围收到最后两层**〔`2026-09-22.md` §14〕：**卡侧能力已排除**——EF_SMSP(41B) 里存着电信短信中心 `+8613344181200`，与 `AT+CSCA?` 的**原始应答**逐字一致（跨来源自证）⇒「这张卡没有短信业务」**不成立**；**矛头指向 IMS 承载**——`+CGACT?` 显示 ims(cid2) **未激活**、`+CGPADDR: 2,0.0.0.0`、`+GTMPDN: 0`、`+GTWWAN?` 只有 cid1 一路，而厂商自己的 ECM 样例里 ims 那条**带着真实 IPv6 地址**。待办：①**E1**（待授权）`AT+CGACT=1,2` 探 ims PDP，可回滚、不花额度；②**E2**（外部，唯一一锤定音、免费）**把卡插进手机试一次短信+电话**；GTACT 写回；修 SMS 布防缺口；多路 PDN；CM/supervisor；★ `dial_stop` 会拆链。★★★★★ **根因已由用户裁定（2026-09-22 16:54）：原厂固件缺陷，后期固件更新修复**〔§15〕⇒ 上面「矛头指向 IMS 承载」**归因推翻**（**观测保留、解释作废**）：同一线路/同一张卡/同一条命令在新版固件下正常；**E1/E2 作废不必再跑**。仍成立的是**现象签名**：NR SA ~95 ms 本地秒拒 / LTE 满 40 s `T1_RP`（缺陷在两制式下暴露面不同，**≠ 承载不同**）；卡侧能力已排除（跨来源自证）依旧成立。★★ 教训：**「AT 命令全对、`AT+CSMS?` 自报支持、却发不出去」= 全绿却失败 ⇒ 先记模组固件版本（现场 `fw=89614.1000.00.04.01.02`）并查厂商新版 changelog/已知问题**，别先怀疑线路。修插件 SMS 布防缺口仍待实施

★★★★ **全量记忆已发布**：`Beaverfffan/fm160-luci` 的 **`notes/`**（**公开仓**）commit `f867478` —— 13 份逐日日志 + `MEMORY.md` + `archive/MEMORY-FULL-ARCHIVE.md`（460 KB，所有「存档 §NN」引用指向它）+ `SMS-VERDICT` + `agent/{SOUL,IDENTITY,USER,MEMORY}.md`；**脱敏脚本** `_tools/istoreos-h69k/290-publish-memory.py`（白名单复制 + 写后保守复查 + **PAT 必抹**）；技能 `memory-publish-sanitized`。★ 验证用 `git ls-remote` + `api.github.com` 树查询 —— **`raw.githubusercontent.com` 在本机会挂死**（无 `--max-time` 时 3 分钟不返回），`repo.size` 是异步刷新的**不能**当判据〔`2026-09-22.md` §16〕

## 0. 最容易再犯（展开见存档 §0）
1. 面板：ucode 在声明点绑顶层名 ⇒ 改 `panel.uc` 必跑 `_tools/ucode_scope_check.py`（背光亮+全白+crash loop）
2. 多补丁共享插入点 ⇒ 新补丁编号必须最大排最后（否则 FAILED）
3. `make defconfig` 会静默降级（`.packageinfo` 陈旧 ⇒ `DEVICE_PACKAGES` 全 `=m`）⇒ config 放树外+断言 profile
4. 多行 shell 绝不内联穿过 ssh 链（曾搞废 LuCI）⇒ 落盘 `sh /tmp/x.sh`；★ `tee|head` ⇒ SIGPIPE
5. 别 `rm -rf tmp` / `make package/<x>/clean` ⇒ 内核整核重编（`CONFIG_ALL_KMODS=y`）
6. 改 `/usr/share/hostap/*.uc` 后必须 `/etc/init.d/wpad restart`
7. 刷机成功 ≠ 跑新代码（overlay 优先）⇒ 判据 = 设备摘要 vs 镜像摘要；★ 同版本号装机必须 `--force-reinstall`；★ **构建指纹只覆盖 `build_dir/*.c/*.h`** ⇒ 改 `files/` 时指纹不变 ⇒ 判据必须落到 ipk 内容（解包比 md5）〔§41.8〕
8. 判据本身会烂 ⇒ derive+三态+下限；fixture 只建稳态 ⇒ 初始化态断言恒真空过；★ 期望值先用新旧两份源码量一遍
9. 门禁：`-32002` 时 CLI `ubus call` 无 session ⇒ 永远通过；「本机绿」≠「门禁绿」
10. 同类脚本要「全部」查一遍；别被 shell 骗：`$?` 穿管道 = 尾部命令状态、`git push` 无 TTY 挂死
11. ★ 探测 ≠ 接受：`=?` 被答 ≠ 操作被接受（冻死或剖面拒）；照抄厂商 fork 的 flag 会静默丢帧〔§24–27〕
12. ★ 改内核 USB 设备表可只换 `.ko` 真机验；BTF ⇒ `pahole` 必在 PATH〔§26〕
13. ★ 写状态实验：①排除混淆项②样本可疑换新重测③判据取对手无法伪造的④副作用明写⑤可回滚⑥每条只发一次
14. ★ vendor 三方 kmod：`KCONFIG` 裸符号 = 条件非赋值；`package/<x>/compile` 未选 `CONFIG_PACKAGE` = 空操作 ⇒ 用 `make -C $LINUX_DIR M=… modules`；`AUTOLOAD 82` 无前缀 ⇒ 先占接口 = 顶掉〔§29〕
15. ★★ 状态码与字段都不可尽信〔§39〕：`AT+GTWWAN=1,1` 被拒/超时而链路一直通 ⇒ 读回才是判决；★ v4/v6 挤在一个 CSV 引号字段 ⇒ 先切后校验
16. ★★★ 「停止」往往才是破坏性的那一步：副作用须覆盖进入与退出；★ `dial_stop` 拆 ECM 上下文 ⇒ 安全的「停」= 留空闲态〔§39.13〕
17. ★★ 只读探针自己产假信号〔§39.15〕：路径别写死；进程计数走 `/proc`；**死字段**先数写点（`pin_status` 写点 0 ⇒ 恒空）。★★ 本机**根本没有 `timeout`** ⇒ `timeout N tcpdump &` 秒死、抓空文件 ⇒ 记 pid 显式 kill；**「抓包为空」≠「网络没包」**；tcpdump 也**不分方向**（会把我们自己的 NUD probe 读成对端的 NS）〔§41.7〕
18. ★★★ `0` 要先证明探测器活着才可判读〔§39.18〕：喂已知包 + 收工具收尾账 + 前后计数器；分清「计数器在涨我没抓到」与「计数器没动也没抓到」；★ 窗口须跨协议最大周期（RA 720 s）
19. ★★ 「收到但未投递」先查 netfilter/zone〔§39.19〕；★ 审计抓包必须按包配对（`grep -B1` 会错配）
20. ★★★ 「**写成功但读不回**」读写两侧都要看：`uci_bool()` 把「0=成功」写成 `if (!…)` ⇒ 恒返回 fallback；**四个 bool 三个因 fallback==出厂默认而「撞对」**，只有唯一常改的那项露馅〔§39.21〕
21. ★★★ 二进制**同尺寸 ≠ 同代码**（两版都 132193 B）⇒ 用 1 字节 diff + 反汇编窗口证明改的就是那条分支（`cbz`↔`cbnz`）；★ 尾调用用 `b` 不占 `bl`〔§39.21〕
22. ★★ 判据取「对手无法伪造」的：**`ping -6` 在此链路不可采信**；`cereg 99` = UNKNOWN 哨兵〔§39.20–21〕
23. ★★★★ 「换样本」纪律：同一客户端地址重复测 ⇒ 只测到**对端缓存**（模组学到 MAC 就不再发 NS）；`tcpdump 'host <x>'` 也抓不到 NDP 的 NS（目的地址是**组播**）⇒ 抓整协议再 grep〔§40.4/40.6〕
24. ★★★★ **别接受别人给的「只支持 X」结论**〔`2026-09-22.md` §14〕：①**先证明该状态在本固件读得到**——FM160-CN 上 `+CIREG/+CIREP/+CIMS/+GTIMS/+CPAS` **全 ERROR**、`AT+CLAC` 只回裸 `OK`（不提供命令表），130 条厂商手册里唯一 IMS token 是 `+CAVIMS`，而它读的是 **MT 里存的**标志（非实时）⇒ 任何"等 IMS 注册 URC（`+CIREGU`/`+VOICE REG`）"的流程在本机**不可执行**；②**用制式不对称反证**：若"只走 IMS"，NR SA 与 LTE 在同一 IMS 状态下必须同表现，实测却 **90–103 ms 本地秒拒 vs 40 s T1_RP** ⇒ LTE 那次走的是**非 IMS 承载**；③**卡侧能力必须跨来源自证**（EF_SMSP 结构扫描解出的短信中心 **==** `AT+CSCA?` 的**原始应答**）；④**语音是免费对照**：`ATD10000;` 后 `+CLCC` 停在 `stat=2(dialing)` 28 s 无 alerting ⇒ **数据通+语音死+短信死 = 一个根因面**，别在命令层打转；⑤**判据取「对手无法伪造」的**：最终一锤定音是外部的"**把卡插手机试短信+电话**"
24. ★★★ odhcpd：`interface` 是 **NETWORK 段名**不是设备名（`wan`→eth0，真链路是 `ecm6`→usb0）；master 与非 master **必须同时** `ndp=relay`，否则静默 DISABLED。★ `curl %{http_code}=000` ≠ 连接失败（IP 直连时证书 SAN 不含 IP 也是 000，TLS 其实握手完了）〔§40.3/40.6〕
25. ★★ 别拿「自己会丢包的环境」否定机制：veth 不在任何 firewall zone ⇒ `input policy drop` 先丢包，测什么都白测〔§40.5〕
26. ★★★★★ odhcpd 的 `learn_routes`（uci `ndproxy_routing`，**默认开**）按**邻居条目所在接口**给该地址装 `/128 metric 1024`：LAN 客户端的 /128 因此落到 **usb0**，而 `/128` 长于 `/64` ⇒ **最长前缀优先于 metric** ⇒ 压过插件的 `/64 dev br-lan metric 100`，包被送去模组、模组邻居又指回我们 = 环形丢包（双向 100%）。⇒ **wan 侧**段必须 `ndproxy_routing='0'`，并清已在内核里的残留。★ 判据取「邻居在、路由不在」的配对〔§41.5〕
27. ★★ `ip -6 route show` 打印**主机路由不带 `/128`**（`…::391 dev br-lan proto static metric 1024` 就是一条 /128）⇒ `grep '/128'` **恒空 = 假阴性**，曾据此误判「LAN 上没客户端」。判据用 `ip -6 route get <addr>`；分族用 `grep -F "$PFX" | grep -F '/64'`〔§42.10〕
28. ★★★★ 门禁**绿不绿取决于它拿到的数据池**，本机 ≠ CI：同一份 po，池子 = 只有 luci-base ⇒ 红 2 条；= openwrt-24.10 全量 93 个 ⇒ 全绿；= master 全量 105 个 ⇒ 又红 ⇒ **判据必须固定在与设备同支的池子**（改 CI 的 sparse 到 `'**/po/zh_Hans/*.po'` + `-b openwrt-24.10`）。★ 门禁**失败零输出** = 它的输出全在「有没有失败」的守卫里、错误只进 `out/*.log` ⇒ 先让它会说话，再动代码〔§43.3/43.4〕
29. ★★★★★ 门禁的判决**不能依赖缓存，也不能依赖 shell 方言**——这两处 CI 红都不是源码 bug〔§44〕：
    ★ **zig 缓存命中时不重放诊断** ⇒ 同一份树「缓存热 = 绿、缓存冷 = 红」，而热时给的是**错**答案 ⇒ 编译类门禁跑前清 `ZIG_LOCAL_CACHE_DIR`（global 留着，省 musl/compiler-rt；冷跑约 45 s）。⇒ 说「本机绿」之前先问：**这次的答案是算出来的，还是从缓存读出来的**
    ★ `$((` 在 POSIX shell 里是**算术展开**；bash 会退化成子 shell，**dash 直接拒绝解析整个文件**（`Syntax error: Missing '))'`，且行号会冲过 EOF）⇒ 本机 `sh`=bash / runner=dash ⇒ **平台差异第一个查 shell 方言**；★ 而且**构建机的 `/bin/sh` 也是 bash** ⇒ 必须显式 `dash -n`，别拿构建机的 `sh` 冒充 runner 的
    ★ **解析不过的 gate = 静默批准的 gate**（`jscheck` 因此在 CI 上从未跑过，而它正是抓「LuCI 视图少一个花括号 = 白屏且无任何构建报错」的那道）
    ★ 修完做**全仓同类扫描**（`for f in $(find . -name '*.sh'); do dash -n "$f"; done`，本仓 19 个全过）**并给扫描器留负对照**：我的第一版负对照 `sed` 根本没匹配上 ⇒ 白白「证明」了 0 失败；改成从 `git show HEAD:<file>` 取旧版喂进去才有效
    ★ 附带收获：`hosttest` 只有 Linux 能跑（本机永远 SKIPPED）⇒ **CI 绿的含义现在重于本机绿**

30. ★★★★★ 短信发送：**「能存」≠「能发」，必须分开报**〔§19.2〕。`AT+CMGW`（写模组存储）与 `AT+CMGS`（交网络）
    走**完全相同**的两阶段（提示符 `>` → 原始载荷 → Ctrl-Z），所以 `CMGW` 成功只证明**传输与 PDU 解析**，
    **不证明**这条路能交给网络。★ 发送路径首次上机前先做 `CMGW` 彩排：零费用、同路径、模组同样会解析 PDU
    （PDU 畸形必回 `+CMS ERROR`），能把「我们的两阶段传输坏了」与「网络拒绝」提前分开。
    ★ 彩排的一个副作用要防：**两阶段中途断掉会让模组一直等消息数据，下一条 AT 命令被当成消息正文** ⇒
    任何"排空"只能在阶段 1 确实没拿到提示符时做，否则那个 Ctrl-Z 会被当**消息终止符**吃掉，随后裸 PDU
    因无 CR 而堆在命令行缓冲里，把之后每条命令都染成 ERROR（实测 `AT degraded (3 timeouts)`）〔§19.4〕
31. ★★★★ 判读「模组拒绝」的三层对照（缺一层就会误判成自己的 bug）〔§19.3〕：
    ★ **手册的应是**：`AT+CMGS` 失败按 Fibocom §7.1.16 应回 **`+CMS ERROR: <err>`**；实测是**裸 `ERROR`**
      ⇒ 拒绝发生在短信业务层**之下**，是**命令层**拒绝 —— 拿手册的"应是"当对照，比拿自己的期望当对照可靠
    ★ **时长**：本地拒绝 ≈ **85–87 ms** 且**恒定**；网络往返是**秒级** ⇒ 时长本身就是一个判据
    ★ **同一串字节分投两条路**：`CMGW` 收下 vs `CMGS` 拒绝 ⇒ 分歧只在"要不要交给网络"
32. ★★ `sms.last_pdu` 存的是**双倍 hex**（126 字符 PDU → 252 字符 + `"1A"` = **254**）⇒ `pdu_chars` 的期望值
    是 254 而非 126。别拿"PDU 长度"直接去比 `pdu_chars`〔§19.2〕
33. ★★ 探针要**先算后发**：发短信这种"发出去就收不回"的动作，先用**独立实现**（不抄被测代码）把结果预测出来
    （`_tools/istoreos-h69k/sms_pdu_ref.py`，按 GSM 03.38 重写并用 3GPP 公开向量自证）；预测与实测逐项吻合
    才能断言"编码器无罪、问题在网络侧"。没有这一步就只能猜〔§19.6〕
34. ★★ `AT+CMEE=0`（**很多模组的出厂默认**）会把一切错误压成单词 `ERROR` ⇒ 拿到裸 ERROR 先开
    `AT+CMEE=2` 再复现；★ 但**若开了详细错误仍是裸 ERROR，那本身就是结论**（说明不是被压制的错误码）。
    用完后记得还原（本插件按数字码 `atoi()` 解析 `+CME ERROR`，文本码会解析成 0）〔§19.3〕
    ⚠️ **2026-09-22 订正**：上面「命令层拒绝」**只在 NR SA 成立**。同一命令在 LTE 下变成"提交下去 + 40 s T1_RP
    无应答"（见 #35）⇒ 「本地 85 ms 秒拒」这个判据必须**同时报制式**，否则不成立
35. ★★★★★ 同一 AT 命令的**语义会随制式变**，而**观察窗口必须长于协议定时器**（`2026-09-22.md` §2–3）：
    ★ `AT+CMGS` 在 **NR SA (act=11)** = **~90 ms 本地秒拒**；在 **LTE (act=7)** = **`ERROR` @ 40010 ms**，
      即满 3GPP TS 24.011 的 **T1_RP（40 s）** 都没等到 RP 应答 ⇒ **制式是自变量**，两个制式下"同一个失败"
      其实是**两种完全不同的故障**（本地拒 vs 网络不答）。判据必须**每臂前后各断言一次制式**，模组会自己漂
      （本次：09:40 还在 LTE，09:41 已回 NR SA，277 三臂因此全被守卫 SKIPPED——**这个跳过是正收益**）
    ★ **`ubus call` 客户端自带 ~30 s 上限**，它会在 at-daemon 的 `timeout` 之前先把请求掐掉，且**不报错**
      （275 日志 `start 09:37:09 / end 09:37:39`，我请求的是 240 s）⇒ `ubus -t <seconds>` 才能抬掉。
      **之前所有「沉默 ≥60 s」的读数其实都只有 30 s 窗口** ⇒ 结论全错 ⇒ 报"超时"前先问：**这个窗口是谁给的**
    ★ 怀疑"是不是我们自己的超时"时查三处：①调用方常量（`at-daemon/const.h` `DEFAULT_TIMEOUT 5`+**不做 clamp**）
      ②CLI 上限（上面那条）③有没有可配的**厂商定时器**（FM160 手册 GT 族 27 条命令**无一条与短信有关**
      ⇒ 40 s 只能是模组的）
    ★ **呼叫被中断会留下迟到应答，把 at-daemon 的匹配器错位一格**：279 的 `AT+CEER` 打印了上一条 `AT+CLCC`
      的内容，`AT+COPS?` 返回 `"response":"AT+COPS?"`（赤裸回显）⇒ **失步期间读数全废**；恢复办法 = 连发
      裸 `AT` 直到全部 `OK`（280，6 次即回正）。★ 这条与 #30 的"漂空"是同一族：**串口上的应答与命令会错配**
36. ★★★ 拨号（`ATD10000;`）**只作为"网络是否完成业务"的对照，不能当作"语音可用"的证据**：NR SA 下
    `ATD` 回 `OK` 且 `+CLCC` 出现 MO 呼叫，但**停在 `stat=2`（dialing）28 秒，从未进入 `alerting(3)`**
    ⇒ 只证明"网络接受了建立请求"，**不证明**能接通。昨天据 `ATD`→`OK` 就写"这张卡有语音业务"是**说过头了**
    （已订正）〔`2026-09-22.md` §6〕
37. ★★ `AT+CLCC` 里那条"清不掉的残留呼叫"**就是 ECM 数据呼叫本身，不是幽灵语音呼叫**〔§13.1〕：
    `+CLCC: 1,0,0,1,0,"",128` 逐字段读：`<mode>=1` 是 **data**、`dir=0` 是 **MO**、`stat=0` 是 active
    —— 正是 ECM 承载。它跨**主机重启**也活着（M.2 槽供电不随 SoC 断），`AT+CFUN=1,1` 后随承载重建
    ⇒ 昨天记成"幽灵语音呼叫"是错的（`dir` 当时读作 1，是因为那条是在 `ATD` 拨号尝试之后被读到的）
    ★ 教训：**多字段状态要逐字段对着 TS 27.007 的字段表读**，别抓一个字段就下结论
38. ★ 探针自己的顺序 bug：`AT+CMGL=4` 的 `<stat>` 是 **PDU 模式**参数，在 `CMGF=1`（文本）下回 `ERROR`
    ⇒ 设计"只读补读"段时要**显式回到对应模式**，别沿用上一段的模式〔`2026-09-22.md` §7〕
39. ★★★★★ 「**重启整机 ≠ 重启模组**」，而"一次性 setup"会在模组重启后变成**谎言**〔§13.1/13.2〕：
    ★ M.2 槽供电不随 SoC 复位而断 ⇒ 主机 `reboot` 之后模组的 NV/状态**原样保留**（判据 = `AT+CLCC` 跨重启存活）。
      要真清场必须 `AT+CFUN=1,1`。★ 细节：USB 断 → 重新枚举 ≈**21 s**，而且
      **ttyUSB 编号会变**（接口 1.2 从 `ttyUSB2` 变 `ttyUSB4`）⇒ **探针写死 `/dev/ttyUSB2` 就会指错口**；
      正解 = 从持有者处自动探测：`ls -l /proc/$(pidof ubus-at-daemon)/fd | grep -o '/dev/ttyUSB[0-9]*'`
    ★ 模组复位把 NV 打回默认：`AT+CNMI? → 0,0,0,0,0`（**通报被关掉**）、`AT+CPMS? → "SM",0,40`。
      而插件只重置端口/AT 状态机、**不清 `sms.setup_done`** ⇒ 状态机停在"布防好了"，**永久收不到 `+CMTI`**
      （MT 短信静默留在存储里，`sms_list` 永远 0 条）。修法：在 `fm160_cmd_ident_start()`（cmds.c:770）里作废 SMS setup
      ⇒ **凡是"设一次就假设它一直在"的初始化，都要在"怀疑换了设备"的时刻作废**，且宁可**读回断言**
      （像 `odhcpd_configured()` 那样）也不要靠记忆
40. ★★★★ 探针里每个"等/判"都要用**对手无法伪造**的量〔§13.5〕：
    ★ 283 第一版用 `dial.step == connected` 判"模组回来了"，而端口已消失时插件仍报旧值 ⇒ 循环第一次就跳出。
      ⇒ 等待条件要由**被测对象无法伪装**的事实组合（端口存在 + 实时 `AT+COPS?` 读到 act=7）同时成立
    ★ **参数里的引号必须逃逸进 JSON**：`AT+CMGS="+86…"` / `AT+CPMS="ME"` 不逃逸时 `ubus` 直接回
      `Parsing message data failed` —— 它**没有到达模组**，绝不能记成"模组拒绝"（已加 `jesc()` + 专用中止分支）
    ★ 报"设备异常"之前先自查探针：本次曾据 `ping -6 …:1189::1` 全丢包怀疑"LAN 前缀没下发"，
      实际插件装的是 `…:1189:1::1`（`::1::1`）——**我 ping 了一个不存在的地址**
41. ★★★★ 短信这条轴上，**格式（PDU/text）不是变量，制式才是**〔§13.3〕：
    2×2 全测（每条都做制式双向断言 + `CMEE=2` 读回断言）——
    NR SA：PDU **103 ms** / text **93 ms**（本地秒拒）；LTE：PDU **40013 ms** / text **40014 ms**（T1_RP 超时）
    ⇒ 同制式内两格式差 10 ms 与 1 ms，都在噪声里 ⇒ **"换个格式再试"零产额，别再花发送额度**；
      而"要发只能在 LTE 发"是硬约束（NR SA 下模组根本不提交）。
    ★ 锁 LTE：`AT+GTACT=2`（持久 + 立即生效，**无临时锁定命令**），读回 `2,,,101,103,…`（pref 清空、频段保留），
      5 s 内 act=7 且数据呼叫未断。★ 原值必须先记全：
      `20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079`

42. ★★★★ FM160 插件短信链路**真机验收通过**（`89614.1000.00.04.01.23`，`2026-09-22.md` §17）：
    收发双向、中文/全角标点逐字正确、+CMTI 实时秒级入库、CFUN 复位后 daemon 自动重布防。
    三个历史 bug 已修：① ident 时 `fm160_sms_setup_reset()` 作废布防缓存；② `+CMTI` 索引
    `atoi(e+1)` 遇 `",N"` 逗号返回 0（**接收路径从未工作过的根因**）；③ `parse_cmgr` 裸 `strstr`
    命中命令回显 `AT+CMGR=2` 把头部当 PDU（报 `not hexadecimal`）⇒ 解析响应用**行首守卫**
    （`fm160_resp_find`/`parse_cmgl` 同款），别用裸 strstr；④ unread 通知/入库双计数删其一。
    ★ 新固件差异：**无 SIM 时 CPMS/CMGL 全裸 ERROR**（先查 SIM 别怀疑代码）；**热插 SIM 需
    `CFUN=1,1` 才识别**；★ daemon `at` 方法每次手动探针开 **10 s 静默窗**压住后台布防——
    验收时忍住不探，隔 60–90 s 看 `ubus call fm160 status`。
43. ★★★ 构建树 `package/fm160d` 与 git HEAD 比对法（防未跟踪副本被覆盖丢改动）：
    本地 `git archive HEAD fm160d | gzip` 上传解压成参考目录，`diff -r --strip-trailing-cr`
    对树副本——本次实测树里有 HEAD 没有的死代码 `modesw.c:copy_str()`（无调用点，无害留待清理），
    CRLF 会让 diff 显示整文件不同，**必须 `--strip-trailing-cr`** 再看。

## 索引（完整结论见存档）
- 构建/源码树 `beaver@192.168.15.157`（IP 会变）、密钥须全盘符、`make -j12` 挂 tmux 〔§1–2〕
- 硬件/网络 BE14000 2GB；⚠️ 端口互换 `wan`=`lan8`(MT7530)、万兆口属 `br-lan`；natflow `995-0001` ⇒ `mtk_ppe*.c` 不编译 〔§3–4〕
- 驱动/迁移 RTL8261C = mainline `744-01..05` + 必放 `pending/`、PHY `0x001cc898`；YT9224 = `kmod-dsa-yt92xx`；passwall 必留 〔§5–6〕
- WED/bridger 别装（crash loop）；x-wrt 已收编 `ded36d2c106e`；`etype` 已过 `ntohs()`、双口互测须 `netns` 〔§7–9〕
- 编译纪律 `# CONFIG_X is not set` 压 `default y`；`worktree` 隔离 〔§10–11〕
- MT5000 RTL8366UB DSA + 1×2.5G WAN（PR `996b7d38`；`do_upgrade()`+`copy_config()` 两处）〔§14/14b〕
- H69K/iStoreOS 24.10.8 r29755、`192.168.100.1`、profile `hinlink_opc-h6xk`；MLO 判据 `radio mask: 3`；★ 凭据见 `~/.workbuddy/MEMORY.md` 〔§17–19〕
- FM160 真机 `blobmsg_open_*()` 回 HANDLE 非 `blob_buf`（⇒139）、`add_u32` 存 int32、`sendat` timeout=秒 〔§20〕
- FM160 拨号栈〔§24–35〕（§32 作废）剖面 `DIAG+MODEM+AT+PIPE+RMNET`；`fm160-qmi` 冷启动全自动 ✓；★ `SET_DATA_FORMAT` 冷启动必需；★ `AT+GTAUTOCONNECT=1` 堵死宿主 ⇒ `=0`+复位
- FM160 5G〔§36–38〕★ `GTACT?` 查能力、`COPS act` 查驻留（11 = NR SA）；★ `--ip-family` 一次一族（不传 = v6-only）；★ `qmap_mode` 必须 1；★ 换 mode 须 reboot
- FM160 ECM〔§39.20–22、§41–42〕剖面 GTUSBMODE 33（须同源工具链）；★ v6 已打通且**过冷启动**（zone 挂 `ecm`/`ecm6` + `network.ecm6 proto dhcpv6` + `filter_aaaa=0`，**零 sysctl**）；★ `option ipv6` 是空操作 ⇒ 必须另起 dhcpv6 接口；★ 不需要 `accept_ra`；★ 前缀/MAC 每次重启都变 ⇒ 判据禁写死；★ 模组=NAT、不发主动 RA（每问必答）〔§39.18〕；★ 插件四入口 `apply|withdraw <dev>|teardown|status`，默认 `dial_pdp=IPV4V6`/`lan_ipv6=1`（`dial_autostart=0`），wan 侧 `ndproxy_routing='0'`〔§41〕；★ 上游只回 RA / 无 PD ⇒ **先试 `extendprefix='1'`**（= RFC 7278，`dhcpv6.script:91-93`）〔§42〕
- **FM160 短信层**〔`2026-09-22.md` §13`–`14〕★ **制式是自变量**（NR SA ~100 ms 本地秒拒 / LTE 40 s T1_RP）；★ 卡侧能力用 **EF_SMSP↔`AT+CSCA?` 跨来源自证**排除；★ **CN 版固件无任何 IMS 状态命令**（`AT+CLAC` = 裸 `OK`；`+CAVIMS` 是**存量**标志）⇒ "等 IMS 注册 URC"流程不可执行；★ **语音对照** `ATD` 停在 `stat=2` = 无语音承载 ⇒ 数据通+语音死+短信死 = 一个根因面；★ ims PDP 未激活（`+CGACT: 2,0`、`+GTMPDN: 0`）；判据 `_tools/istoreos-h69k/out/SMS-VERDICT-2026-09-22.txt`
- 门禁/CI〔§43–44〕`fm160-luci` 本地跑法：`NODE=… PY=… LUCI=_tmp/master-luci sh tools/check.sh`；★ 全绿靠四修（i18n 池子 / cccheck 失败自证 / 删死代码 / `$( (`）；★ 本机 `sh`=bash 而 runner=dash ⇒ 改任何 `.sh` 都要 `dash -n` 过一遍
- FM160 短信〔`2026-09-22.md` §1–11〕★ **制式是自变量**：NR SA = `AT+CMGS` 本地 ~90 ms 秒拒；LTE = 提交下去 + **40 s T1_RP** 无应答（`ERROR` @ 40010 ms）；★ 我们这侧无罪（编码/长度/两阶段/SMSC 字段逐项自证，`CMGW` 同字节收下）；★ `+CSMS: 0,1,1,1` 模组自报支持 MO；★ 文本模式（mcuzone 参考）与 PDU 模式在两种制式下**结果相同**；★ 拨号两制式都停在 `dialing`；★ `AT+GTACT=2` = LTE only（持久、立即生效）；★ 工具 `sms_pdu_ref.py`（先算后发）+ 探针 260–280
- FM160 短信插件层验收+三修〔§42，`2026-09-22.md` §17〕；构建树 vs HEAD 比对法〔§43〕
- 面板/Doom 背光亮无画面先量 `…/spi0/statistics/bytes`（LVGL/DRM +153.6k；`/dev/fb0` 假信号）；调面板不刷固件 〔§12–13/15–16〕
