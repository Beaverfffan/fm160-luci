# FM160 原厂 CLI 拨号复刻

按广和通官方文档复刻，不用任何 netifd 原生拨号（无 qmi/mbim proto handler）。
固件仅含：`kmod-qmi_wwan_f`（厂商 QMAP 驱动）、`fibocom-dial`（厂商 QMI 栈三件套）、
`kmod-usb-net-cdc-mbim` + `libmbim/mbim-utils`（mbimcli）、`fm160d`（模式切换/SMS/GNSS，非拨号）。
已剔除：modemmanager、uqmi、umbim、luci-proto-qmi、luci-proto-mbim、fm160-qmi。

## 文档对照

| 脚本 | 复刻来源 |
|---|---|
| switch-mode.sh | AT+GTUSBMODE（QMI=32 / MBIM=30 / ECM=33），经 fm160d ubus |
| qmap-up.sh | 《QMI&Gobinet 拨号指南 Linux V1.7》§6.5.2：设 qmap_mode → fibo_qmimsg_server → fibocom-dial -n/-m/-4/-6；§6.2 `-s APN` 写 PDP 预设 |
| qmap-down.sh | §6.2 `-k pid` 按 pid 终止 |
| mbim-up.sh | 《ECM&NCM&RNDIS&MBIM 拨号集成指导 Linux V1.8》§5.5：--query-subscriber-ready-status → --connect=session-id=0,apn=X → mbim-set-ip |
| mbim-set-ip.sh | §5.5 步骤 4 的 mbim-set-ip（原厂未公开脚本，按 --query-ip-configuration 输出解析复刻，含双栈） |
| mbim-down.sh | §5.5 步骤 6：--disconnect=0 |

## 关键事实（来自文档）

- fibocom-dial 有两种编译形态：`make`（QMI 取 IP 直配网卡）/ `make dhcp`（udhcpc）。
  lede 源码默认是前者——**不走 DHCP**，规避 stock 驱动上 QMAP 下行封装垃圾帧问题。
- qmi_wwan_f 单路 qmap_mode=0；多路先 `echo N > /sys/class/net/wwan0/qmap_mode` 出 wwan0.1..N，
  再逐路 `fibocom-dial -n <通道> -m <PDP CID>`。改 -N 总通道数会重建网卡（§6.2）。
- 多路 ping 不通先关 rp_filter（FAQ 5.7.1）。
- MBIM 多路用不同 session-id + 不同 APN，mbim-set-ip 第三参数传 session-id（§5.6）。
- MBIM 仅 SDX12/SDX62/SDX65 支持；FM160 是 SDX62 ✓。

## 实测记录

（刷机后在此追加）
