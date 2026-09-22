#!/bin/sh
# 实时信号 × 速度关联：每轮 AT+CESQ 取原始值后测 10s 单流
for i in $(seq 1 12); do
	CESQ=$(ubus call fm160 at '{"cmd":"AT+CESQ"}' 2>/dev/null | grep -oE '\+CESQ: [0-9,]+' | head -1 | sed 's/+CESQ: //')
	IFA=$(ip -4 -br a show wwan0 | awk '{print $3}' | cut -d/ -f1)
	rm -f /tmp/spd.bin
	wget -q -O /tmp/spd.bin --bind-address="$IFA" http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso &
	WP=$!
	sleep 10
	kill $WP 2>/dev/null; sleep 1
	B=$(stat -c %s /tmp/spd.bin 2>/dev/null || echo 0)
	awk -v i="$i" -v c="$CESQ" -v b="$B" 'BEGIN{printf "run %2d  CESQ[%s]  speed=%.2f MB/s\n", i, c, b/10/1048576}'
	rm -f /tmp/spd.bin
done
