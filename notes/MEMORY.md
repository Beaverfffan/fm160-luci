# GL.iNet / x-wrt 固件开发 — 速查索引

> 细节 = `BE14000-fixes/MEMORY-FULL-ARCHIVE.md`（**先读 §0**）；手法 = skill；时间线 = `memory/`。⚠️ **≤6 KB**（超尾截断）⇒ 新知识**先写存档**，只加一行

**进行中**：MT5000 PPE；MLO 在线。★★★ **FM160 六项已收口**（`fm160-qmi` v1.0.2-1）：①〔§33〕CM+netifd、**§32 作废**；②〔§34〕uqmi 零 DHCP、`cdc-wdm0` **单 reader**；③〔§35〕冷启动 ✓；④〔§36〕**QMI「偶发超时」= 请求级偶发失败**（稳态 <0.17%）、client id **语义失效**须 `--sync`；⑤〔§37〕**5G SA + 数据面打通 ✓**；⑥〔§38〕**ipk 外层 gzip+tar** 装机 ✓。**待办**：多路 PDN **未走通**；CM/supervisor 未纳入发布；`fm160d dialer.c`

## 0. 最容易再犯（必读；展开见存档 §0）
1. **面板**：ucode **在声明点绑顶层名** ⇒ 改 `panel.uc` 必跑 `_tools/ucode_scope_check.py`（症状 = 背光亮+全白+crash loop）
2. 多补丁共享插入点 ⇒ **新补丁编号必须最大排最后**（否则 FAILED）
3. `make defconfig` **会静默降级**（`.packageinfo` 陈旧 ⇒ `DEVICE_PACKAGES` 全 `=m`；依赖缺失 ⇒ 掉回默认）⇒ config 放树外+断言 profile
4. **多行 shell 绝不内联**穿过 ssh 链（曾搞废 LuCI）⇒ 落盘 `scp` + `sh /tmp/x.sh`
5. 别 `rm -rf tmp` / `make package/<x>/clean` ⇒ **内核整核重编**（`CONFIG_ALL_KMODS=y`）
6. 改 `/usr/share/hostap/*.uc` 后必须 `/etc/init.d/wpad restart`
7. **刷机成功 ≠ 跑新代码**（overlay 优先）⇒ 判据 = 设备摘要 vs 镜像摘要
8. **判据本身会烂** ⇒ **derive**+**三态**+**下限**；fixture 只建稳态 ⇒ 初始化态断言**恒真空过**
9. **门禁/判据**：`-32002` 时 CLI `ubus call` **无 session ⇒ 永远通过**（只有 HTTP `POST /ubus` 是真相）；**「本机绿」≠「门禁绿」** ⇒ 证据集**必须 derive**
10. **同类脚本要「全部」查一遍**（「FAIL 与 ok 矛盾」=判据写错）；**别被 shell 骗**：`$?` 穿管道 = **尾部**命令状态、`git push` 无 TTY 挂死
11. ★ **探测 ≠ 接受**：`=?` 被答 ≠ 操作被接受（**冻死**或**剖面拒**）；**照抄厂商 fork 的 flag 会静默丢帧**〔§24–27〕
12. ★ 改内核 **USB 设备表**可只换 `.ko` 真机验（`=m`·无 MODVERSIONS/签名·vermagic 匹配）；**BTF ⇒ `pahole` 必在 PATH** 〔§26〕
13. ★ **写状态实验**：①先排除混淆项②样本可疑**换新重测**③判据选**对手无法伪造**的（`rx_packets` 非 `tx_packets`）④副作用**明写**⑤**可重入回滚**⑥每条**只发一次**、读**原 JSON**；★「两条状态一起动过」**别只看一次**〔§27–28〕
14. ★ **vendor 三方 kmod**：`KCONFIG` 裸符号 = **条件非赋值**；`package/<x>/compile` 未选 `CONFIG_PACKAGE` = **空操作** ⇒ 用 `make -C $LINUX_DIR M=… modules`；`AUTOLOAD 82` vs mainline **无前缀** ⇒ **先占接口 = 顶掉**；必钉 **commit+blob+sha256**〔§29〕

## 索引（完整结论见存档）
- **构建/源码树** `beaver@192.168.15.157`（**IP 会变**）、密钥**须全盘符**、`make -j12` 挂 tmux；纯净仓 `xwrt-flint4-upstream` 〔§1–2〕
- **硬件/网络** BE14000 2GB；⚠️ **端口互换** `wan`=`lan8`(MT7530)、万兆口属 `br-lan`；natflow `995-0001` 让 `mtk_ppe*.c` **不编译** ⇒ 只打包 `dsa_port` 〔§3–4〕
- **驱动/迁移** RTL8261C 用 mainline `744-01..05`、PHY `0x001cc898`、**必放 `pending/`**；YT9224 = kmod 包 `kmod-dsa-yt92xx`；★ passwall 必留 〔§5–6〕
- **WED/上游化/取证** bridger **别装**（crash loop）；x-wrt 优先、已收编 `ded36d2c106e`；`etype` 已过 `ntohs()`、双口互测须 `netns` 〔§7–9〕
- **编译/纪律** `# CONFIG_X is not set` 压住 `default y`；`worktree` 隔离 〔§10–11〕
- **MT5000** RTL8366UB DSA + 1×2.5G WAN（PR `996b7d38` 选 **8021Q**、`do_upgrade()`+`copy_config()` **两处**）；剥屏脚本**编 BE14000 前 restore**、`hwnat=` 是 **uci 值非运行态** 〔§14/14b〕
- **MLO/FM160-H69K** MLO 已在线（判据 `radio mask: 3`）；`192.168.100.1`、profile `hinlink_opc-h6xk`、★ 凭据见 `~/.workbuddy/MEMORY.md` 〔§17–18〕
- **H69K 刷机** **24.10.8 r29755**；**绝不能 `-n`**；刷机**丢 216 包** ⇒ 只 opkg 装 ipk；★ **「`make` 跑完」≠成功，必读 `Error N`** 〔§19〕
- **FM160 真机** `blobmsg_open_*()` 回 **HANDLE 非 `blob_buf`**（⇒ 运行期 **139**）、`add_u32` 存 int32（须 u64）、`sendat` timeout=**秒** 〔§20〕
- **FM160 拨号栈〔§24–35〕** 剖面 = `DIAG+MODEM+AT+PIPE+**RMNET**`、安全集 **derive** `[17,18,30,33]`；`fm160-qmi` **冷启动全自动 ✓**；★ **`SET_DATA_FORMAT` 冷启动必需**（不设 ⇒ rx 恒 0 + `drop skb_len` **伪随机**）；★ 根因 **`AT+GTAUTOCONNECT=1`（出厂）自建呼叫堵死宿主** ⇒ `=0`+复位；★ 就绪门**只问 WDS**、PDH **可为负**、`ps|grep` **自匹配会杀自己**、USB/AT 按 **`idVendor=2cb7`** 反查；★ 不带 `-d` 的 udhcpc 走 `default.script` ⇒ **删别人的默认路由**；★ 写 `+GT*` 前必先 `?` 读原值、`GTUSBMODE` Persistent
- **FM160 5G/ipk〔§36–38〕** ★ **能力**查 `GTACT?`，**注没注上**查 `COPS act`（**11 = NR SA**）+`C5GREG`+`--get-system-info` —— **别拿一次观测当能力结论**；★ `--ip-family` **一次只给一族**（不传/`unspecified` → **v6-only**）；★ **官方 ipk = gzip+tar 非 ar**；★ `qmap_mode` **必须 1**（>1 ⇒ rx 恒 0、**静默失败**）；★ **`rmmod/insmod` 打死数据面 ⇒ 换 mode 要 reboot**；★ netifd `pending` **冲掉手工地址** ⇒ 先 `ifdown`
- **面板/Doom/其他** 背光亮无画面先量 `…/spi0/statistics/bytes`（LVGL +153,600／DRM +153,611；`/dev/fb0` 恒 0 假信号）；调面板**不刷固件**、让 DRM = `stop`+`kill -9`；`glinet-panel` `d2a130b`、`doom` `fe5046f`；BE3600=IPQ5332、内核 **6.18**；GNSS 影响 **0**、**NMEA 从 AT 口出** 〔§12–13/15–16/21–22〕
