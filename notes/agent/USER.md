---
summary: "User profile record"
read_when:
  - Bootstrapping a workspace manually
---

# USER.md - About Your Human

_Learn about the person you're helping. Update this as you go._

- **Name:** 待确认（GitHub 上是 `Beaverfffan`）
- **What to call them:** 待确认 —— 还没见你自称过，这个不该我替你填
- **Pronouns:** _(optional)_
- **City:** 待确认（时钟 GMT+8）
- **Notes:** 简体中文交流，句子短，很少寒暄。习惯用「继续」「收口」「仍没做」这类极简指令
  驱动一长串多步任务 —— 也就是说默认授权是「按上一条的意图往下做完」，
  不必每步回头确认。

## Context

**在做的事**：OpenWrt / x-wrt 固件移植与排障。设备是 GL.iNet 系列
（GL-BE14000、GL-BE3600、GL-MT5000、GL-MG1300）加一台 H69K（RK3568 / iStoreOS）。
日常出现的东西：DSA 交换芯片与端口 tag、natflow / MTK PPE 硬件加速、U-Boot 与刷机、
mipi-dbi 小屏面板与 LVGL、蜂窝模组（广和通 FM160）的 AT/QMI 拨号栈。

**工作方式上他明确要的**：

- **要证据，不要结论**。他反复接受的是带数值、hex、字节数、sha256、时间戳的表格与根因链；
  「我觉得」在他的语境里不算完。
- **要可复现**。脚本落盘、判据可 derive、失败要能分「未证明」与「失败」；
  不接受「本机绿就算绿」。
- **错了就撤回**。他见过我推翻自己的整条归因链并照常推进 —— 这类更正比硬撑有价值。
- **先侦察再动手**，关键决策点讲清楚再推进；有冲突时**保留双方**，不擅自丢一边。

**别做的事**：

- 别用「很好的问题！」这类填充语开头；直接给结果。
- 别把没验过的东西写成已验过的。他不介意「未走通」，介意「假装走通」。

**在意的东西**：`passwall` 这类 feeds 来的包调试时**留着别删**；swap 只用 zram 或 NVMe
（机械盘的延迟他不要）；`192.168.x.x` 这类内网地址可以写进公开文档，**IMEI / 序列号不行**。

**GitHub 推送**：他给过一个 classic PAT 并交代「记住它，别再忘了」，
存在 `~/.workbuddy/MEMORY.md`；推送时用 `GIT_ASKPASS` 走环境变量、不落盘、不要再问。

---

The more you know, the better you can help. But remember - you're learning about a person, not building a dossier. Respect the difference.
