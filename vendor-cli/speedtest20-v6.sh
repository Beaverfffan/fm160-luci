#!/bin/sh
# 20 秒下行测速（IPv6）：自动取 wwan0 全局 v6 地址，TUNA 镜像走 v6
IFADDR=$(ip -6 -br a show wwan0 scope global 2>/dev/null | awk '{print $3}' | cut -d/ -f1 | head -1)
[ -z "$IFADDR" ] && { echo "wwan0 无全局 IPv6 地址"; exit 1; }
OUT=/tmp/spd6.bin
rm -f "$OUT"
wget -q -O "$OUT" --bind-address="$IFADDR" http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso &
WP=$!
sleep 20
kill $WP 2>/dev/null
sleep 1
BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
awk -v b="$BYTES" 'BEGIN{printf "v6: downloaded %d bytes in 20s = %.2f MB/s\n", b, b/20/1048576}'
rm -f "$OUT"
