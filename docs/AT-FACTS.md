# FM160/FG160 官方文档事实摘录（实现依据）

> 来源（用户提供，均已抽取为可检索文本，见 `_ref/fm160/docs/`）：
> - `AT-Commands-FM160-FG160.txt` — FM160&FG160 AT Commands User Manual **V1.0.2**（348 页，109 条命令）
> - `Dialup-ECM-NCM-RNDIS-MBIM.txt` — ECM&NCM&RNDIS&MBIM 拨号集成指导_Linux **V1.0**（51 页）
> - `GNSS-Application-Guide.txt` — FM160 Application Guide_GNSS **V1.0**（22 页）
>
> 凡本文标注 ⚠️ 的都是「踩了就难回头」的点，实现里必须有对应守卫。

---

## 1. USB Profile：`AT+GTUSBMODE`（拨号模式的物理前提）

### 1.1 手册给出的候选值（11.1.2）

> 「supported mode depends on the target device and they may be as below」

| mode | 接口组成 | 含 AT 口 |
|---|---|---|
| 17 | DIAG + MODEM + AT + PIPE + RMNET + ADB | ✔ |
| 18 | DIAG + MODEM + AT + PIPE + ECM + ADB | ✔ |
| 20 | MODEM | ✘ |
| 21 | MODEM + AT | ✔（无数据口） |
| 24 | RNDIS + MODEM + DIAG + ADB | ✘ |
| 29 | MBIM + AT + DIAG | ✔ |
| 30 | MBIM + MODEM + DIAG + AT | ✔ |
| 31 | DIAG + MODEM + RMNET + DPL + QDSS + ADB | ✘ |
| 32 | DIAG + MODEM + AT + PIPE + RMNET | ✔ |
| 33 | DIAG + MODEM + AT + PIPE + ECM | ✔ |

- 持久化：Persistent = Yes；**改动后需 reset / power cycle 才生效**。
- 响应快（< 1s）。

### 1.2 拨号文档的 4 张端口表（**平台不同，mode 号含义不同**）

拨号文档 `2.1 USB 端口信息` 里有 **4 张表**，必须按平台对号入座：

| 表 | 章节 | 平台 | VID | PID 段 | mode |
|---|---|---|---|---|---|
| **表 1** | 2.1.1 | **高通**（FM150/FG150/FM100/FM101/FG101/FM130/**FM160**/NL95X） | `0x2CB7` | `010x` | 17,18,19,20,21,22,23,24,28,29,30,32,33 |
| 表 2 | 2.1.1 | 另一平台 | `0x1508` / `0x05C6` | 0x1000/0x1001/0x9025/0x90B6 | 17,18,19,22,24,25 |
| 表 3 | 2.1.2 | 展锐（UNISOC） | `0x2CB7` | `0x0A05/0x0A06/0x0A07` | 36,37,38,39,40,41 |
| 表 4 | 2.1.3 | MTK（FM350/FG360） | `0x0e8d` | `0x7126/0x7127` | 40,41 |

⚠️ **只有「表 1 且 VID:PID = 2CB7:010x」适用于 FM160。**
⚠️ **同一个 mode 号在不同表里含义不同**：mode 19 在表 1 有 AT 口（ECM+AT），在表 2 的 `05C6:9025` 里**没有 AT 口**（RmNet+Mystorage）。⇒ **不能用 mode 号本身判断安全性，必须先核对平台。**

**表 1 全表（本项目唯一依据）**

| mode | PID | 接口号 → 接口名 | 含 AT 口 |
|---|---|---|---|
| 17 | **0104** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 **RmNet** / 5 ADB | ✔ |
| 18 | **0105** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 ECM / 5 ECM / 6 ADB | ✔ |
| 19 | 0106 | 0 DIAG / 1 Modem / 2 **AT** / 3 ECM / 4 ECM | ✔ |
| 20 | 0107 | 0 Modem | **✘** |
| 21 | 0108 | 0 Modem / 1 **AT**（无数据口） | ✔ |
| 22 | 0109 | 0 Modem / 1 **AT** / 2 **RmNet** | ✔ |
| 23 | 010A | 0 Modem / 1 **AT** / 2 ECM / 3 ECM | ✔ |
| 24 | 010B | 0 RNDIS / 1 RNDIS / 2 Modem / 3 DIAG / 4 ADB | **✘** |
| 28 | 010F | 0 MBIM / 1 MBIM | **✘** |
| 29 | 0110 | 0 MBIM / 1 MBIM / 2 **AT** / 3 DIAG | ✔ |
| 30 | 0111 | 0 MBIM / 1 MBIM / 2 Modem / 3 DIAG / 4 **AT** | ✔ |
| 32 | **0104** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 **RmNet** | ✔ |
| 33 | **0105** | 0 DIAG / 1 Modem / 2 **AT** / 3 Pipe / 4 ECM / 5 ECM | ✔ |

⚠️ **17 与 32 共用 PID 0104；18 与 33 共用 PID 0105。**
⇒ **禁止用 VID:PID 推断当前 mode**，必须 `AT+GTUSBMODE?` 回读。PID 只能用来「模块在不在」+「是不是 010x 段（即表 1 适用）」。

⚠️ **无 AT 口 = 单向门**（`20`、`24`、`28`，加上 AT 手册 11.1.2.4 独有列出的 `31` = DIAG+MODEM+RMNET+DPL+QDSS+ADB）。切进去之后路由器侧再没有任何 AT 通道可以切回来。
⇒ **硬黑名单 `{20, 24, 28, 31}`，无旁路。** 其余「表 1 有、但 AT 手册 11.1.2.4 未列出」的 mode（19/22/23）属 `device dependent`，必须用户显式确认。

> 供查阅的机器可读版本：`_ref/fm160/docs/USB-MODES.txt`（由 `_tools/` 脚本从 PDF 文本直接抽取，含四张表的原始行）。

### 1.3 官方建议的上电/断电时序（拨号文档 3.4）

- 断电 → 再上电：**> 12 s**（等电容放电）
- 相邻两次**开机**：**> 90 s**
- 相邻两次**断电**：**> 300 s**
- 开机后等 **90 s** 仍枚举不出端口 ⇒ 判本次开机失败，重新开机（或延时 5 s 起连续发 AT，最长待 90 s）

---

## 2. 拨号：数据面怎么建

### 2.1 ECM / RNDIS / NCM（**不用** QMI/MBIM 协议栈，靠模块内部拨号）

官方流程（3.5/3.6）：

```
AT+CPIN?                      → +CPIN: READY
AT+CREG?                      → +CREG: 0,1 或 0,5     （CS 域）
AT+CGREG? / AT+CEREG?         → 0,1 或 0,5             （PS 域 / 4G）
AT+CGDCONT=1,"IP","<APN>"     → OK
AT+GTWWAN=1,1   (ECM/RMNET)   → OK        ← 或 AT+GTRNDIS=1,1（RNDIS）
AT+GTWWAN?                    → +GTWWAN: 1,1,"IP","pdns","sdns"   ← 拿到 IP 才算成功
```

- **结束判定**（官方明确）：收到 `OK` / `ERROR` / `+GTWWAN: 0` / **210 s 超时**，四者之一即认为本次指令结束，**然后**才用 `AT+GTWWAN?` 确认。
- **失败策略**（官方）：连续 5 次 `AT+GTWWAN?` 拿不到 IP ⇒ 回到 `AT+CPIN?`；连续 5 次拨号失败 ⇒ **复位模块**。
- **断开**：`AT+GTWWAN=0,<cid>`；连续 5 次仍失败 ⇒ 复位。
  ⚠️ 官方原话：**不能直接拔 USB 线断开数据业务，也不能直接断电或重启模块**。
- ⚠️ **文档自相矛盾**：AT 手册 11.1.15 说 ECM/RMNET 用 `+GTWWAN`、RNDIS 用 `+GTRNDIS`；但拨号文档 V1.0 的 **ECM 章节示例里用的是 `AT+GTRNDIS=1,1`**（含 FM160 示例）。
  ⇒ **实现必须在运行期探测**：`AT+GTWWAN=?` 与 `AT+GTRNDIS=?` 哪个返回 OK 用哪个，不硬编码。

### 2.2 QMI（mode 17/22/25/32）

- 主机驱动 `qmi_wwan` → `/dev/cdc-wdm0` + `wwan0`
- OpenWrt 侧：`uqmi` + netifd `proto qmi`

### 2.3 MBIM（mode 29/30）

- 主机驱动 `cdc_mbim` → `/dev/cdc-wdm0` + `wwan0`
- 拨号：`mbimcli --connect=session-id=0,apn=<apn>`；查：`--query-connection-state` / `--query-ip-configuration`
- 断开：`mbimcli -d /dev/cdc-wdm0 --disconnect="0"`
- OpenWrt 侧：`umbim` + netifd `proto mbim`
- ⚠️ 官方提醒：mbimcli 版本决定功能可用性（1.14 起），与模块无关。

### 2.4 多路 PDN（VLAN，本期可选）

```
AT+GTWWAN=1,1        # CID1
AT+GTWWAN=1,3        # CID3 → 模块内部自动建 VLAN 1
# 主机侧
ip link add link usb0 name usb0.1 type vlan id 1
dhclient usb0.1
echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter     # 必须关反向路由检查
```

相关专有命令：`+GTMPDN`（VLAN 多 PDN 开关）、`+GTMAPVLAN`（VLAN ID 映射）、`+GTIPPASS`（IP passthrough）、`+GTRMNETMAP`（RMNET NIC 映射顺序）。

---

## 3. 内核侧改动要求（拨号文档 §2）

### 3.1 必需驱动

| 用途 | 内核配置 |
|---|---|
| AT 串口 | `CONFIG_USB_SERIAL_OPTION`（USB driver for GSM and CDMA modems） |
| ECM | `CONFIG_USB_NET_CDCETHER`（CDC Ethernet support） |
| NCM | `CONFIG_USB_NET_CDC_NCM` |
| MBIM | `CONFIG_USB_NET_CDC_MBIM` |
| RNDIS | `CONFIG_USB_USBNET` + `CONFIG_USB_NET_RNDIS_HOST` |
| QMI | `CONFIG_USB_NET_QMI_WWAN` |

### 3.2 option 驱动需要认识 FM160 —— 结论：本项目**不打**这个补丁，但必须避开 mode 21 / 29

厂商文档给了两种写法（全局 `option_blacklist_info` 结构 或 就地在 `usb_device_id` 用 `RSVD(n)`），
并按 `USBMODE = n` 逐条分配保留接口位。

**但真正决定 AT 口能不能出现的不是这张表，而是内核 `drivers/usb/serial/option.c` 里有没有对应 PID 的条目。**
option 先按 VID:PID 匹配，匹配不上就根本不会去 probe 任何接口 —— USB 描述符再健康也没用。

#### (1) 厂商给的保留位表（原文 §2.3 `option.c` 节选，VID 0x2CB7）

| PID | mode | 厂商 reserved bits | 表 1 端口表里有吗 |
|---|---|---|---|
| 0104 | 17 / 32 | `RSVD(4)\|RSVD(5)` | ✓ |
| 0105 | 18 | `RSVD(4)\|RSVD(5)\|RSVD(6)` | ✓ |
| 0105 | 33 | `RSVD(4)` | ✓ |
| 0106 | 19 | `RSVD(3)\|RSVD(4)` | ✓ |
| 0109 | 22 | `RSVD(2)` | ✓ |
| 010A | 23 | `RSVD(2)\|RSVD(3)` | ✓ |
| 010B | 24 | `RSVD(0)\|RSVD(1)\|RSVD(4)` | ✓ |
| 010C / 010D / 010E | 25 / 26 / 27 | `RSVD(4)\|RSVD(5)\|RSVD(6)` | ✗ 只出现在 option.c 列表 |
| 010F | 28 | `RSVD(0)\|RSVD(1)` | ✓ |
| 0110 | 29 | `RSVD(0)\|RSVD(1)` | ✓ |
| 0111 | 30 | `RSVD(0)\|RSVD(1)` | ✓ |
| — | 20 / 21 / 31 | **厂商两张列表里都没有** | 20/21 在表 1，31 不在 |

⚠️ 20（0x0107）和 21（0x0108）在厂商自己的两张 `option.c` 列表里**都没出现**，尽管表 1 给了完整布局
（20 = 只有 Modem；21 = Modem + AT）。

#### (2) 内核实际带了哪几条（数据来自构建机已解出的 6.6.127 源码）

| PID | mode | 内核条目 | 与厂商建议的差别 |
|---|---|---|---|
| 0104 | 17 / 32 | `USB_DEVICE` + `RSVD(4)\|RSVD(5)` | **逐位一致** ✓ |
| 0105 | 18 / 33 | `USB_DEVICE_INTERFACE_CLASS(…,0xff)` + `RSVD(6)` | 少 4/5；但 4/5 是 ECM(class 0x02)，靠 class 已挡住 ⇒ 等效 |
| 0106 | 19 | 有，无 driver_info | — |
| 010A | 23 | 有 | — |
| 010B | 24 | 两条 `AND_INTERFACE_INFO`（Diag / AT） | — |
| 0111 | 30 | 有，无 driver_info | 0/1 是 MBIM 类 ⇒ 等效 |
| 0107 / 0108 / 0109 / 010C / 010D / 010E / 010F / 0110 | 20/21/22/25/26/27/28/**29** | **无条目** | — |

清零验证（在编译机上跑过，结果为 `0x0104=1 0x0105=1 0x0106=1 0x0107=0 0x0108=0 0x0109=0
0x010a=1 0x010b=2 0x010c=0 0x010d=0 0x010e=0 0x010f=0 0x0110=0 0x0111=1`）：

```sh
F=<build_dir>/…/linux-6.6.127/drivers/usb/serial/option.c
for p in 0104 0105 0106 0107 0108 0109 010a 010b 010c 010d 010e 010f 0110 0111; do
    printf '0x%s %s\n' "$p" "$(grep -c "0x2cb7, 0x$p" $F)"
done
grep -rn '0x0110\|0x010f' target/linux/     # 树内没有任何补丁补上它们
```

#### (3) 结论

* **本项目允许切换的 mode（17 / 18 / 32 / 33 / 30）全部有内核条目**，AT 口照常落在 `ttyUSB2`。
  厂商补丁对本项目**不必要** —— 这也正是 32 模式（0x0104）在 H69K 真机上 AT 正常的原因。
* ⚠️ **mode 29（0x0110）必须按「禁止」处理**：模块侧有 AT 口，主机侧没有 `ttyUSB*`。
  切过去 = 永久失去管理通道，且无法从系统侧复位。已写入 `api.js` 的 `USB_MODE_NO_KERNEL_DRIVER`，
  并从 MBIM 候选表里删除（MBIM 只剩 30）。
* mode 21（0x0108）同理不可切；它的 AT 口只会出现在 `ttyUSB1` 而非 `ttyUSB2`，
  属于"靠手工还能救回来"，但仍然不提供。
* 若将来确实要 21 / 29，每个 PID 一行补丁即可：
  `{ USB_DEVICE_INTERFACE_CLASS(0x2cb7, 0x0110, 0xff), .driver_info = RSVD(0) | RSVD(1) },`
  但那时必须重新真机验证，不能只凭这一行。

⚠️ 内核数据来自 **6.6.127**。本树目标已升到 **6.6.144**；option.c 对这几个 PID 的覆盖在两者之间没有变化，
但刷机后仍应复核一次（看 `ls /dev/ttyUSB*` 的数量即可）。

### 3.3 ⚠️ 零包（ZLP）机制 — 长 AT 命令的潜在杀手

文档 §2.4 要求在 `drivers/usb/serial/usb_wwan.c` 的 `usb_wwan_setup_urb()` 里：

```c
if (dir == USB_DIR_OUT) {
    struct usb_device_descriptor *desc = &serial->dev->descriptor;
    if (desc->idVendor == cpu_to_le16(0x2cb7))
        urb->transfer_flags |= URB_ZERO_PACKET;
}
```

**为什么重要**：AT 写入时若长度恰好是端点包长整数倍而没补零包，模块侧会一直等后续数据 ⇒ 表现为「AT 挂住、超时」。长 APN、`AT+CGDCONT`、以及**短信 PDU（`AT+CMGS`/`AT+CMGW` 通常 > 64 B）**最容易被命中。
⇒ 真机验证清单第一条：发一条 > 70 字节的 PDU 看是否稳定。若不稳 ⇒ 上 ZLP 补丁。

---

## 4. 命令地图（109 条中与本项目相关的）

### 4.1 模块身份 / 状态
`AT+CGMI` `AT+GMI` `AT+CGMM` `AT+GMM` `AT+CGMR` `AT+GMR` `AT+CGSN`(IMEI) `AT+GSN` `AT+CFSN`(出厂 SN) `AT+CNUM`(本机号) `AT+CCID`(ICCID) `AT+GTUSIM`

### 4.2 控制 / 温度 / 电源
`AT+CFUN`(0/1/4；返回 `+CFUN: 1` 才算正常) `AT+GTFMODE`(硬件飞行模式) `AT+CPWROFF` `AT+MTSM`(温度传感器) `AT+MMAD`(ADC 电压) `AT+GTDUALSIM` `AT+GTWAKE` `AT+SLPMODE`

### 4.3 SIM
`AT+CPIN` `AT+TPIN` `AT+CPINR`(剩余重试次数) `AT+CPWD` `AT+CLCK` `AT+CRSM` `AT+CSIM`

### 4.4 网络 / 信号 / 频段 / 小区
`AT+CSQ`(rssi,ber；rssi>0 且 ≠99 才有效) `AT+CESQ`(扩展：rsrp/rsrq) `AT+CREG` `AT+CGREG` `AT+CEREG` `AT+C5GREG` `AT+COPS` `AT+CPLS` `AT+CPOL` `AT+COPN` `AT+CEMODE` `AT+GTRAT` **`AT+GTACT`**(RAT+频段) **`AT+GTCCINFO`**(服务+邻区，含 RSRP/RSRQ/SINR) `AT+GTCAINFO`(CA) **`AT+GTCELLLOCK`**(锁小区)

### 4.5 数据
`AT+CGDCONT` `AT+CGATT` `AT+CGACT` `AT+CGPADDR` `AT+CGCONTRDP` `AT+CGEREP` `AT+CGAUTH`/`AT+MGAUTH` `AT+CSCON` `AT+GTSTATIS`(收发包计数)

### 4.6 专有
`AT+GTUSBMODE` `AT+GTWWAN` `AT+GTRNDIS` `AT+GTMPDN` `AT+GTMAPVLAN` `AT+GTIPPASS` `AT+GTAUTOCONNECT` `AT+GTAUTODHCP` `AT+GTDNS` `AT+GTPREDNSCFG` **`AT+GTURCMODE`** `AT+GTROAMCFG` `AT+GTECMDOWNEN` `AT+GTRMNETMAP`

### 4.7 短信
`AT+CSCS` `AT+CSMS` `AT+CPMS` `AT+CMGF` `AT+CSCA` `AT+CSMP` `AT+CSDH` `AT+CNMI` `AT+CNMA` `AT+CMGL` `AT+CMGR` `AT+CMGS` `AT+CMGW` `AT+CMSS` `AT+CMGD` `AT+CSCB` `AT+SMMFULL`

### 4.8 GNSS
`AT+GTGPSPOWER` `AT+GTGPS` `AT+GTGPSEPO` `AT+GTAGPSSERV` `AT+GTGPSCFG` `AT+GTGPSCERT`

---

## 5. 信号与频段编码（**仅用于显示映射**）

### 5.1 `AT+GTACT` 的 RAT 值

| rat | 含义 |
|---|---|
| 1 | UMTS |
| 2 | LTE |
| 4 | LTE/UMTS |
| 10 | Auto（注意：**写入 10 后回读会变成 20**） |
| 14 | NR-RAN |
| 16 | NR-RAN/WCDMA |
| 17 | NR-RAN/LTE |
| 20 | NR-RAN/WCDMA/LTE |

`<PreferredAct1/2>`：2 = WCDMA 优先，3 = LTE 优先，6 = NR 优先。

### 5.2 频段编码

| RAT | 编码 |
|---|---|
| UMTS | = 频段号本身（1..25） |
| LTE | = **100 + 频段号**（B3 → 103，B41 → 141，B64 → 164） |
| NR | = **500 + 频段号**（N1 → 501，N78 → 578，N79 → 579） |

写频段的写法示例（官方）：
- 只锁频段、不改 RAT：`AT+GTACT=,,,160,155`（前 3 个参数留空）
- LTE B3 + NR N78：`AT+GTACT=,,,103,5078`（手册原文如此，疑为 `578` 的排版错误）

⚠️ **一律以 `AT+GTACT=?` 返回的设备支持列表为准**。手册的 `5010`/`50512` 是 PDF 抽文本时的粘连，不要照抄。

### 5.3 `AT+GTCCINFO?` 输出结构（信号页的主数据源）

一条命令同时给出**服务小区 + 最多 10 个邻区**，分 4 种形态：

```
UMTS 服务小区:  <IsServiceCell>,<rat>,<mcc>,<mnc>,<lac>,<cellid>,<uarfcn>,<psc>,<band>,<ecno>,<rscp>,<rac>,<rxlev>,<reserved>,<Ec/Io_lev>
LTE 服务小区:   <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<earfcn>,<physicalcellId>,<band>,<bandwidth>,<rssnr_value>,<rxlev>,<rsrp>,<rsrq>
NR  服务小区:   <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<narfcn>,<physicalcellId>,<band>,<bandwidth>,<ss-sinr>,<rxlev>,<ss-rsrp>,<ss-rsrq>
EN-DC:          LTE 行 + NR 行 各一条（rat 4 与 9）
```

`<IsServiceCell>`：1 = 服务小区，2 = 邻区。`<rat>`：0 无效 / 2 WCDMA / 4 LTE / 9 NR-RAN。
响应时间 < 3 s，Persistent = Yes。

### 5.4 `AT+GTCELLLOCK` 语法

```
AT+GTCELLLOCK=<mode>[,<rat>,<type>,<earfcn>[,<PCI>][,<scs>[,<nrband>]]]
```

| 参数 | 取值 |
|---|---|
| mode | 0 关闭 / 1 开启 |
| rat | 0 LTE / 1 NR / 2 UMTS |
| type | 0 锁 PCI/PSC / 1 锁频点 |
| earfcn | 0 – 4294967295 |
| PCI | LTE 0–503 / NR 0–1007 |
| scs | 0 = 15 kHz / 1 = 30 kHz |
| nrband | 500 + N |

⚠️ 三条官方硬约束：
1. 只想锁「上次关机前注册的 LTE/SA PCI」→ 下发 `AT+GTCELLLOCK=1`
2. **下发后必须重启 UE**（配置写在 EFS，重启才生效）
3. **换 SIM 卡前必须先关闭本功能**

### 5.5 `AT+GTURCMODE` — 主动减少 URC 干扰

`AT+GTURCMODE=<report_flag>,[URC]`：`report_flag` 0 = 不上报、1 = 上报；`URC` 是匹配子串（最长 10 字符），**最多可屏蔽 10 条 URC**。
⇒ 对本项目极其有用：把用不到的 URC 关掉，AT 流更干净、解析碰撞更少。但 **`+CMTI` 绝不能关**。

---

## 6. GNSS 事实（GNSS 手册全文 22 页）

| 命令 | 作用 | 关键取值 |
|---|---|---|
| `AT+GTGPSPOWER` | GNSS 开关 | 0 关（默认）/ 1 开 |
| `AT+GTGPS[=<item>]` | 读 NMEA | item ∈ `"RMC"` `"GGA"` `"GSA"` `"GSV"`；不带参数 = 全部 |
| `AT+GTGPSEPO` | AGPS 开关 | 0 关（默认）/ 1 MSB / 2 MSA |
| `AT+GTAGPSSERV` | AGPS 服务器 | `AT+GTAGPSSERV="supl.qxwz.com",7276`（端口 1–65535） |
| `AT+GTGPSCFG=<x>,<value>` | 卫星/SUPL 配置 | x: 0 SUPL 版本（0=1.0,1=2.0）/ 1 xtra / 2 卫星组合 / 3 SUPL 证书 |
| `AT+GTGPSCERT` | 导入/删除 SUPL 证书 | `.der` 格式，num 1–9 |

卫星组合 `AT+GTGPSCFG=2,<v>`：0 = GPS+GLO，2 = GPS+GAL，3 = GPS+QZSS，4 = GPS+BDS+GAL，5 = GPS+BDS+GLO，6 = GPS+BDS+QZSS，7 = GPS+GLO+GAL，**14 = GPS+BDS+GAL+GLO+QZSS（全开）**，15 = 仅 GPS。

- 全部命令**不需要 SIM、不需要注网、不需要数据连接**，响应 < 500 ms。
- 除 `AT+GTAGPSSERV`/`GTGPSCFG`/`GTGPSCERT` 是「掉电保存 = Yes」，`GTGPSPOWER`/`GTGPS`/`GTGPSEPO` **掉电不保存** ⇒ 每次开机要重新 `AT+GTGPSPOWER=1`。

---

## 7. 其它值得注意的官方细节

- `AT+COPS=3,2` 后可 `AT+COPS?` 按名称查运营商（多 SIM 场景选 APN 用）。
- 专网卡若运营商没给账号密码：电信卡惯用 `card`/`card`；鉴权默认 PAP 或 PAP&CHAP。
- `AT+CGDCONT?` 在 FM160 上会返回多条（cid 1 ip / cid 2 ims / cid 3 cmnet / cid 4 cmwap / cid 5 sos），**不要假设只有 1 条**。
- `AT+CPIN?` 返回 `+CPIN: READY` 才算识别到 SIM；连续 90 s 查不到 ⇒ 官方建议复位模块。
- `AT+CSQ` 的 `<rssi>` 必须 > 0 且 ≠ 99；连续 90 s 不正确 ⇒ 复位模块。

---

## 8. 真机实测（H69K + FM160-CN，89614.1000.00.04.01.02）

以下全部来自设备实测；与手册不一致处，**以本节为准**。

### 8.1 端口布局（VID:PID = `2cb7:0104`，`AT+GTUSBMODE?` = 32 = QMI 模式）

`option` 驱动绑定 iface 0–3，`qmi_wwan` 绑定 iface 4：

| 接口 | 设备 | 用途 | 能否应答 `AT` |
|---|---|---|---|
| 0 | `/dev/ttyUSB0` | DIAG | 否（超时） |
| 1 | `/dev/ttyUSB1` | NMEA（GNSS 输出） | 否（超时） |
| 2 | `/dev/ttyUSB2` | **AT 口** | **是，约 1 s 内** |
| 3 | `/dev/ttyUSB3` | MODEM | 否 |
| 4 | `wwan0` | QMI 数据面 | — |

⇒ 探测顺序按 `bInterfaceNumber` 把 **iface 2 排最前**，省掉两次 4 s 超时。

**sysfs 陷阱**：`/sys/class/tty/ttyUSBn/device` 解析到的是 **usb_interface**，
所以 `<那>/../idVendor` **仍然是接口目录** —— 它只有 `bInterfaceNumber`，**没有 `idVendor`**。
`idVendor` 在再上一层（usb_device）。⇒ 必须 `realpath()` 之后**逐级上溯**找，
硬编码一级会让 VID 过滤全程静默失效。另：`bInterfaceNumber` 在 sysfs 里是 **`%02x` 十六进制**。

### 8.2 ★ `+CME ERROR` 是终结符，且传输层把它报成 `success`

at-daemon 的默认 end_flag 列表 = `OK` / `ERROR` / `+CME ERROR:` / `+CME ERROR:` / `NO CARRIER`。
匹配到任一即 `result = 0` ⇒ 回报 `status: "success"`。

**所以 `success` 的含义是「这次交互结束了」，不是「模块接受了命令」。**
实测后果：无 SIM 时 `AT+ICCID` 回 `+CME ERROR: 13`，被当成成功，错误文本被存成了 ICCID。

⇒ 判成功必须**先看响应文本**再看 status：含 `ERROR` 一律 `AT_STATUS_ERROR`，
统一在 `atq.c` 的 `sendat_cb` 里做（那是所有 AT 的唯一收口）。
`AT_STATUS_ERROR` 只记 `last_fail_ms`、**不累加熔断计数**（熔断只认 timeout），
所以无 SIM 的常态 CME ERROR 不会把模块误判成故障。

### 8.3 实测响应时间与取值形状

| 命令 | 实测 | 备注 |
|---|---|---|
| `AT` | 首次 ~1 s（含开端口），之后 < 50 ms | 探测用 |
| `AT+CSQ` | 24–40 ms | RSSI=25 → −63 dBm |
| `AT+CGMI` | 快速 | `Fibocom Wireless Inc.`（**无**前缀） |
| `AT+CGMM` | 快速 | `FM160-CN` |
| `AT+CGMR` | 快速 | `89614.1000.00.04.01.02` |
| `AT+CGSN` | 快速 | IMEI，纯数字无前缀 |
| `AT+CFSN` | 快速 | ⚠️ 回 `+CFSN: "FP62PE002F"` —— **带前缀和引号**，必须剥 |
| `AT+ICCID` | 18 ms | 无 SIM 时 `+CME ERROR: 13` |
| `AT+GTUSBMODE?` | 16 ms | 本机 `32` |

全链最差响应 **36 ms**，`queue_depth` 恒 0 —— 15 s / 10 s 的分层轮询对这块模块足够宽松。

⇒ 取值统一处理：先剥开头的 `+PREFIX:`，再剥首尾引号
（`cmds.c` 的 `first_value_line()`；`AT+CGMI` 那种裸串不受影响）。

### 8.4 设备上的第三方争用（**直接伤害 AT 稳定性**）

实测该设备（iStoreOS 24.10.8，刷过自制镜像）上同时存在：

* **`ModemManager` 在跑**（`S70modemmanager`，pid 9115）—— 它会去开它找到的每个 ttyUSB。
* **`S99adb-enablemodem` 在跑**，`adb wait-for-device` + `adb fork-server` 常驻。
  该脚本只为 TP-LINK LTE 模块（`0x2357:0x000D`）写，对 FM160 永不匹配，等于白占一个 adb server。
* `network.2_1` = `proto dhcp` on `wwan0`（qmodem 遗留命名），实测引发内核
  `wwan0: NETDEV WATCHDOG: transmit queue 0 timed out 5170 ms`。
* 另有 `qmi_wwan 2-1:1.4 wwan0: Cannot change a running device`。

⇒ 「fm160d 是 AT 口唯一属主」这个设计前提，在**这台机器上并不成立**。
动手前先确认没有别的进程会去开 `ttyUSB2`（`grep ttyUSB /proc/*/fd`）。

**已处置（2026-09-18，用户选的「最小改动」方案）**：
`/etc/init.d/modemmanager disable && /etc/init.d/modemmanager stop` ⇒
rc.d 链接已撤、`ModemManager`/`-wrapper`/两个 `-monitor` 全部退出、服务状态 `DISABLED`。
处置后实测：`ttyUSB0/1/3` **无任何持有者**，`ttyUSB2` 仅 `ubus-at-daemon` 持有，
`fm160d` PID 不变、`worst_response_ms` 仍为 40 ⇒ 处理是安全且有效的。
⚠️ 停之前它虽然**没抢到** AT 口（`ubus-at-daemon` 先开了），但 USB 一旦重枚举它就会去探所有 ttyUSB。

**按用户决定暂留**：`adb-enablemodem`（对 FM160 永不匹配，白占 adb server）与
`network.2_1`/`2_1v6`（`wwan0` 上的 dhcp/dhcpv6，M2 拨号阶段会一并处理）。

### 8.5 ★★ `sendat` 的 `timeout` 单位是**秒**，不是毫秒 —— 传错 = 整个 daemon 失联

三条独立证据都指向「秒」：

* `at-daemon/src/const.h`：`#define DEFAULT_TIMEOUT 5   // seconds`
* QModem 的参考客户端：`ubus_invoke(..., timeout * 1000 + 1000)` —— 即它对外按**秒**记账，
  只有 `ubus_invoke` 的预算才乘 1000 换成毫秒。
* 我们自己的 `fm160d`：
  `fm160d.h` 写着 `int timeout_ms;  /* sendat timeout, seconds internally */`，
  `atq.c` 做 `secs = (req->timeout_ms + 999) / 1000;` 再上线，
  而 `ubus_invoke()` 的客户端预算用 `req->timeout_ms + 2000`（**这里**才是毫秒）。
  ⇒ LuCI（`debug.js` 传 5000/20000 ms）→ `fm160d` → `at-daemon` 这条链**是对的**。

**传错的后果不是「早一点超时」，而是整个 daemon 失联：**

* `at_handler.c` 的等待是 `abs_timeout.tv_sec += timeout;` + `pthread_cond_timedwait()`，
  单位是**秒**。所以 `"timeout":2000`（本想 2 s）⇒ daemon 原地等 **2000 秒（33 分钟）**。
* 更致命的是 `ubus_sendat_method` 是**在 ubus 主循环里同步执行**的：
  这一条请求卡住时，`list` / `close` / `open` … **所有**方法都不再应答。
* 实测（2026-09-18 21:00）：一条 `timeout:2000` 的探针打出去之后 ——
  * `ubus call at-daemon list` 30 s 无响应，客户端报 `Request timed out`；
  * `fm160d` 每 4 s 打一条 `sendat invoke failed: Request timed out`，
    状态掉到 `port_found=false`、`port=""`、`consec_timeout=3`、`last_ok_age_ms` 一路涨到 198634；
  * `procd` 收到 SIGTERM 后**杀不掉它**（阻塞在 `pthread_cond_timedwait` 里没看信号），
    最后是 `not stopped on SIGTERM, sending SIGKILL instead`。

**为什么这个坑一直藏着**：此前所有探针命令都在 14–30 ms 内命中 `OK` 终结符，
`timeout` 路径**一次都没被走到**。只有「模块根本不回答」时才会炸。

⇒ **纪律**：任何直接调 `at-daemon sendat` 的脚本/工具，`timeout` 一律按**秒**写（`2`、`6`）；
传 `2000`/`6000` 这种数就是 33 分钟 / 100 分钟的失联。
`_probe/` 下的 `04`–`07` 原来写的正是 `T=6000` / `t=${3:-6000}`，已全部改成秒并加了警告头。

### 8.6 四个接口的应答性（逐口探完，2026-09-18）

同一时刻的映射（`realpath /sys/class/tty/ttyUSBn/device` ⇒ `.../2-1:1.n/ttyUSBn`）：

| 端口 | USB 接口 | 驱动 | 应答 AT？ | 实测证据 |
|---|---|---|---|---|
| `/dev/ttyUSB0` | `2-1:1.0`（DIAG） | `option1` | **否，完全静默** | 4 条全 `status:timeout`、`response_time_ms:2000`，且**没有** `partial_response` |
| `/dev/ttyUSB1` | `2-1:1.1`（官方称 NMEA） | `option1` | **是** | `AT`→`\r\nOK\r\n` 20 ms；`ATI` 整段身份 142 B / 22 ms；`AT+CGMM`→`FM160-CN` 14 ms |
| `/dev/ttyUSB2` | `2-1:1.2`（AT） | `option1` | **是** | 14–29 ms，`fm160d` 的正式 AT 口 |
| `/dev/ttyUSB3` | `2-1:1.3`（MODEM） | `option1` | **否，只回显** | 全 `status:timeout`；`partial_response` **就是我们自己发出去的** `AT\r\n\r\n` |

★★ **「iface 1 = NMEA、iface 2 = AT、iface 3 = MODEM」这个官方端口描述在本机不成立**：

* **iface 1 是一个能用的 AT 口**，而且**不吐 NMEA** —— 用永不匹配的终结符做 2 秒原始抓取，
  只收到 `\r\nOK\r\n`，没有 `$GPxxx`/`$GNxxx` 之类的语句流。
* **iface 3 反而是半个死口**：有回显、没有 AT 解释器（`partial_response` 恰好等于发出去的命令）。
* ⇒ 本机实际可用 AT 口**有两个**（iface 1 与 iface 2）。这不是坏事：iface 1 是天然的**备份 AT 口**。

定位接口号的方法（别只看第一层）：`bInterfaceNumber` 在 `/sys/class/tty/ttyUSBn/device`
**上一层的 interface 目录**里，且格式是 **`%02x`**（`00/01/02/03`）；
`idVendor` 还要再往上到 usb_device 节点才有。

### 8.7 `fm160d` 的探测序（实测）与一个待改的排序

实测启动日志：

```
AT candidates: 4 Fibocom (2cb7:*), 0 unclassified, 0 foreign
  | probe order: /dev/ttyUSB2(if2) /dev/ttyUSB3(if3) /dev/ttyUSB1(if1) /dev/ttyUSB0(if0)
AT port is /dev/ttyUSB2          <- 启动后 1 秒内命中首个候选
```

* 正常路径**不会**走到 if3/if0，因为它们排在第 3/4 位，而 if2 一击即中。
* 但 if3 与 if0 **永不响应**，每走到一个就要付一次完整超时（`fm160d` 的 `timeout_ms` 下限 1 s）。
  ⇒ if2 一旦短暂失败，就会先白付 1 s 在死口 if3 上，才轮到**真正可用的 if1**。
* **建议（尚未实施）**：候选序改为 `if2 → if1 → if3 → if0`，
  并把**完全静默的 iface 0 直接从候选里剔除**。
* 判据补充：`partial_response` 只在 `status: timeout` 时出现，
  正好用来区分「完全静默」（if0）与「有回显、无解析」（if3）。

复现脚本：`_probe/10-at-facts.sh`（已用正确单位；**故意跳过 ttyUSB2**，
因为那是 `fm160d` 正在轮询的口）。`_probe/09-tty-truth.sh` 只做只读取证，不发任何 AT。
