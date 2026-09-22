#!/bin/sh
# FM160 原厂 MBIM 拨号（复刻 Fibocom ECM&NCM&RNDIS&MBIM 拨号集成指导 V1.8 §5.5）
# 前置：模块已切 MBIM 模式（GTUSBMODE 30），cdc_mbim 驱动已枚举 wwan0 + /dev/cdc-wdm0
# 用法：mbim-up.sh [APN]     默认 ctnet
set -e
APN=${1:-ctnet}
DEV=/dev/cdc-wdm0
IF=wwan0
LOG=/var/log/vendor-mbim.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

[ -c "$DEV" ] || { log "$DEV 不存在"; exit 1; }
ip link set "$IF" up 2>/dev/null || true

# 1. 初始化查询（等同文档步骤 1）
log "查询订阅者就绪状态"
mbimcli -p -d "$DEV" --query-subscriber-ready-status >>"$LOG" 2>&1 || {
	log "subscriber-ready 失败，模块未就绪"; exit 1; }

# 2. 拨号（等同文档步骤 2，双栈 APN）
log "发起 MBIM 连接 apn=$APN"
mbimcli -p -d "$DEV" --connect="session-id=0,apn=$APN" >>"$LOG" 2>&1 || {
	log "connect 失败"; exit 1; }

# 3. 把拿到的 IP 配到网卡（复刻 mbim-set-ip）
/usr/sbin/mbim-set-ip.sh "$DEV" "$IF" session-id=0 >>"$LOG" 2>&1 || exit 1

log "MBIM 拨号完成：ifconfig $IF 应已拿到地址"
