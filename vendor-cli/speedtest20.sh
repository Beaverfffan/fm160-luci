#!/bin/sh
# 20 秒下行测速：下载 TUNA 镜像，按落盘字节数算平均吞吐
IFADDR=10.169.38.210
OUT=/tmp/spd.bin
rm -f "$OUT"
wget -q -O "$OUT" --bind-address="$IFADDR" http://mirrors.tuna.tsinghua.edu.cn/ubuntu-cdimage/20.04/ubuntu-20.04.6-desktop-amd64.iso &
WP=$!
sleep 20
kill $WP 2>/dev/null
sleep 1
BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
awk -v b="$BYTES" 'BEGIN{printf "downloaded %d bytes in 20s = %.2f MB/s\n", b, b/20/1048576}'
rm -f "$OUT"
