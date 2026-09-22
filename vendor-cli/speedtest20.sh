#!/bin/sh
# 20 秒下行测速：下载 TUNA 镜像，按落盘字节数算平均吞吐（自动取 wwan0 的 v4 地址）
IFADDR=$(ip -4 -br a show wwan0 2>/dev/null | awk '{print $3}' | cut -d/ -f1)
[ -z "$IFADDR" ] && { echo "wwan0 无 IPv4 地址"; exit 1; }
OUT=/tmp/spd.bin
rm -f "$OUT"
wget -q -O "$OUT" --bind-address="$IFADDR" http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso &
WP=$!
sleep 20
kill $WP 2>/dev/null
sleep 1
BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
awk -v b="$BYTES" 'BEGIN{printf "downloaded %d bytes in 20s = %.2f MB/s\n", b, b/20/1048576}'
rm -f "$OUT"
