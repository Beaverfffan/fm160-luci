#!/bin/sh
# mbim-set-ip 复刻（广和通原厂脚本未公开，按 MBIM 集成指导 V1.8 §5.5 第 4 步行为实现）
# 用法：mbim-set-ip.sh <dev> <网卡> [session-id=X]
# 从 mbimcli --query-ip-configuration 取双栈配置，直接配到网卡（无 DHCP）
set -e
DEV=${1:-/dev/cdc-wdm0}
IF=${2:-wwan0}
SID=${3:-session-id=0}
SID=${SID#session-id=}   # mbimcli --query-ip-configuration 只要裸数字
log() { echo "[mbim-set-ip] $*"; }

OUT=$(mbimcli -p -d "$DEV" --query-ip-configuration="$SID" 2>&1) || {
	log "query-ip-configuration 失败：$OUT"; exit 1; }
echo "$OUT"

# ── IPv4 ──────────────────────────────────────────────────────────────
V4A=$(echo "$OUT" | sed -n "s/.*IP \[0\]: '\([0-9.]*\/[0-9]*\)'.*/\1/p" | head -1)
V4G=$(echo "$OUT" | sed -n "s/.*Gateway: '\([0-9.]*\)'.*/\1/p" | head -1)
V4M=$(echo "$OUT" | sed -n "s/.*MTU: '\([0-9]*\)'.*/\1/p" | head -1)
V4D=$(echo "$OUT" | sed -n "s/.*DNS \[[0-9]*\]: '\([0-9.]*\)'.*/\1/p" | head -2)

if [ -n "$V4A" ]; then
	ip link set "$IF" up
	ip addr flush dev "$IF" label "$IF" 2>/dev/null || ip addr flush dev "$IF"
	ip addr add "$V4A" dev "$IF"
	[ -n "$V4M" ] && ip link set dev "$IF" mtu "$V4M"
	ip route replace default via "$V4G" dev "$IF" 2>/dev/null || \
		ip route replace default dev "$IF"
	for d in $V4D; do grep -q "$d" /etc/resolv.conf 2>/dev/null || echo "nameserver $d" >> /etc/resolv.conf; done
	log "IPv4 已配置 $V4A via $V4G"
fi

# ── IPv6 ──────────────────────────────────────────────────────────────
V6A=$(echo "$OUT" | grep -A20 'IPv6 configuration' | sed -n "s/.*IP \[0\]: '\([0-9a-fA-F:]*\/[0-9]*\)'.*/\1/p" | head -1)
V6G=$(echo "$OUT" | grep -A20 'IPv6 configuration' | sed -n "s/.*Gateway: '\([0-9a-fA-F:]*\)'.*/\1/p" | head -1)

if [ -n "$V6A" ]; then
	ip -6 addr flush dev "$IF" scope global 2>/dev/null || true
	ip -6 addr add "$V6A" dev "$IF"
	if [ -n "$V6G" ]; then
		ip -6 route replace default via "$V6G" dev "$IF" 2>/dev/null || \
			ip -6 route replace default dev "$IF"
	fi
	log "IPv6 已配置 $V6A via ${V6G:-dev}"
fi

log "网卡配置完成"
