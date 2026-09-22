#!/bin/sh
# 多镜像源对照：同链路交替从 USTC / 阿里云 / TUNA 下载 10s
IFA=$(ip -4 -br a show wwan0 | awk '{print $3}' | cut -d/ -f1)
test_url() {
	name=$1; url=$2
	rm -f /tmp/spd.bin
	wget -q -O /tmp/spd.bin --bind-address="$IFA" "$url" &
	WP=$!
	sleep 10
	kill $WP 2>/dev/null; sleep 1
	B=$(stat -c %s /tmp/spd.bin 2>/dev/null || echo 0)
	awk -v n="$name" -v b="$B" 'BEGIN{printf "%-10s %8.2f MB/s\n", n, b/10/1048576}'
	rm -f /tmp/spd.bin
}
for r in 1 2 3; do
	test_url USTC   "http://mirrors.ustc.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
	test_url ALIYUN "http://mirrors.aliyun.com/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
	test_url TUNA   "http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
done
