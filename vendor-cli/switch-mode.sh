#!/bin/sh
# 模块 USB 模式切换（经 fm160d ubus），切后等待设备重枚举
# 用法：switch-mode.sh qmi|mbim|ecm
case "$1" in
	qmi)  MODE=32 ;;
	mbim) MODE=30 ;;
	ecm)  MODE=33 ;;
	*) echo "用法: $0 qmi|mbim|ecm"; exit 2 ;;
esac
echo "切换到模式 $MODE ..."
ubus call fm160 setusbmode "{\"mode\":$MODE}"
# 等待 USB 重枚举 + 驱动稳定
i=0
while [ $i -lt 30 ]; do
	sleep 4
	i=$((i+1))
	if [ "$1" = mbim ]; then
		[ -c /dev/cdc-wdm0 ] && ip link show wwan0 >/dev/null 2>&1 && break
	else
		[ -c /dev/cdc-wdm0 ] && [ -d /sys/class/net/wwan0 ] && break
	fi
done
echo "等待结束（$((i*4))s）：cdc-wdm0 $([ -c /dev/cdc-wdm0 ] && echo OK || echo 缺失)，wwan0 $(ip link show wwan0 2>/dev/null | head -1 || echo 缺失)"
