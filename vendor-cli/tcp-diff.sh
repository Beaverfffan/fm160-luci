#!/bin/sh
# 慢/快两次下载前后对比 TcpExt 计数器增量（busybox 无 diff，用 awk）
IFA=$(ip -4 -br a show wwan0 | awk '{print $3}' | cut -d/ -f1)
snap() {
	awk 'NR==1{h=$0; next} NR==2{v=$0;
		n=split(h,ha," "); m=split(v,va," ");
		for(i=1;i<=n && i<=m;i++) printf "%s=%s\n", ha[i], va[i];
	}' /proc/net/netstat
}
run_once() {
	URL=$1
	snap > /tmp/tcp_before
	rm -f /tmp/spd.bin
	wget -q -O /tmp/spd.bin --bind-address="$IFA" "$URL" &
	WP=$!
	sleep 10
	kill $WP 2>/dev/null; sleep 1
	snap > /tmp/tcp_after
	SZ=$(stat -c %s /tmp/spd.bin 2>/dev/null || echo 0)
	echo "--- speed: $(awk -v b=$SZ 'BEGIN{printf "%.2f", b/10/1048576}') MB/s"
	awk -F= 'NR==FNR{b[$1]=$2; next} {d=$2-b[$1]; if(d!=0 && $1 !~ /Octets|InNoECT|InDelivers|OutRequests|DelayedACKs|TCPHPHits/) printf "%-28s +%d\n", $1, d}' /tmp/tcp_before /tmp/tcp_after | sort -t+ -k2 -rn | head -18
	rm -f /tmp/spd.bin
}
echo "=== run A ==="; run_once "http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
echo "=== run B ==="; run_once "http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
echo "=== run C ==="; run_once "http://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/22.04/ubuntu-22.04.5-desktop-amd64.iso"
