#!/bin/sh
# QMAP 速度 × 服务小区质量关联采样：每轮抓 earfcn/pci/rsrp/sinr/rsrq 后测 12s 单流
for i in $(seq 1 10); do
	ST=$(ubus call fm160 status 2>/dev/null)
	EF=$(echo "$ST" | grep '"earfcn"' | head -1 | grep -o '[0-9]*')
	PCI=$(echo "$ST" | grep '"pci"' | head -1 | grep -o '[0-9]*')
	RSRP=$(echo "$ST" | grep '"rsrp_dbm"' | head -1 | grep -oE '\-[0-9]*')
	SINR=$(echo "$ST" | grep '"sinr_db10"' | head -1 | grep -oE '\-[0-9]*|[0-9]*' | head -1)
	RSRQ=$(echo "$ST" | grep '"rsrq_db10"' | head -1 | grep -oE '\-[0-9]*')
	IFA=$(ip -4 -br a show wwan0 | awk '{print $3}' | cut -d/ -f1)
	rm -f /tmp/spd.bin
	wget -q -O /tmp/spd.bin --bind-address="$IFA" http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso &
	WP=$!
	sleep 12
	kill $WP 2>/dev/null; sleep 1
	B=$(stat -c %s /tmp/spd.bin 2>/dev/null || echo 0)
	awk -v i="$i" -v ef="$EF" -v pci="$PCI" -v rsrp="$RSRP" -v sinr="$SINR" -v rsrq="$RSRQ" -v b="$B" 'BEGIN{printf "run %2d  earfcn=%s pci=%s rsrp=%s rsrq=%s sinr_x10=%s  speed=%.2f MB/s\n", i, ef, pci, rsrp, rsrq, sinr, b/12/1048576}'
	rm -f /tmp/spd.bin
done
