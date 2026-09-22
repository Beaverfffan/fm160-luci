# 用户级记忆（跨项目）

## GitHub 推送凭据

用户 GitHub ID `Beaverfffan`。推送用 **classic PAT**：

```
<GITHUB_PAT_REDACTED>
```

★ 用户 2026-09-19 明确交代「**记住它，别再忘了**」，并说明「这个 token 无所谓的」
（= 无需为安全性反复询问）。⇒ **以后遇到 `git push` 需要凭据，直接用它，不要再问。**

### 怎么用（Windows 工作站实测可行）

`git push` 在无 TTY 环境下会**挂在凭据提示上**，表现是**空输出 + 一直不返回**
（不是报错，也不是「沙箱拦截」）。绕法 —— 用 `GIT_ASKPASS`，token 只走环境变量、不落盘：

```bash
cd <repo>
export FM160_TOKEN='<上面的 token>'
AP=/c/Users/Administrator/AppData/Local/Temp/fm_askpass.sh
printf '#!/bin/sh\nprintf "%%s\\n" "$FM160_TOKEN"\n' > "$AP"
chmod +x "$AP"
GIT_ASKPASS='C:/Users/Administrator/AppData/Local/Temp/fm_askpass.sh' \
GIT_TERMINAL_PROMPT=0 timeout 120 \
git -c credential.helper= push origin main
rm -f "$AP"
```

⚠️ **三个坑**（都踩过）：

1. **`GIT_ASKPASS` 必须是 Windows 侧路径**（`C:/...`）。给 MSYS 的 `/tmp/xxx.sh` 会报
   `error: cannot spawn /tmp/xxx.sh: No such file or directory`
   —— git 是 Windows 程序，不认 MSYS 的 `/tmp`。
2. **`-c credential.helper=`（空值）** 用来**禁用已配置的 Git Credential Manager**，
   否则它可能先弹交互框。本机全局配的是 PortableGit 的 `git-credential-manager.exe`。
3. **别用管道取退出码**：`git push … | tail; echo $?` 拿到的是 `tail` 的 `0`，
   会在命令其实卡住/失败时给出「成功」。要真状态就别管道（或 `set -o pipefail`）。

### 判断仓库是公开还是私有

`git ls-remote https://github.com/<owner>/<repo>.git` **匿名能读到** ⇒ **公开**：
读不需要凭据，**写必须 token**。别把「ls-remote 通了」当成「凭据没问题」。
查有没有已存凭据（**只看有无 `password=`，不打印值**）：

```bash
printf 'protocol=https\nhost=github.com\n\n' | GIT_TERMINAL_PROMPT=0 git credential fill
```

## Windows 本机 Python：openpyxl 只在隔离环境里

★ 2026-09-21 实测：本机 `python3` / `python` / 托管 `binaries\python\versions\3.13.12\python.exe`
**三个都 import 不到 openpyxl**。能用的只有隔离环境：

```
C:/Users/Administrator/.workbuddy/binaries/python/envs/default/Scripts/python.exe
```

（该 venv 里已有 openpyxl 3.1.5、et-xmlfile、pdfplumber 等。）

- 生成/读写 xlsx 的脚本一律用上面这个解释器路径，别用 `python3`。
- `python3 -m pip install --upgrade pip` 在本机**会被 SIGTERM 掉**（exit 1、无输出）；
  直接 `-m pip install <pkg>`（不升级 pip）就能从本地缓存装上。
- 与 `tencent-docs-sheet-generation` skill 的冲突：该 skill 文档里写的是 `python3`，
  在本机不成立 —— 用 venv 路径替换即可，其余流程照做。

## 记忆文件不要自加体积上限

★ 2026-09-21 用户明确「memory 可以解开大小限制」。此前我在项目级 `MEMORY.md` 里自加过
「⚠️ ≤6 KB ⇒ 新知识先写存档，只加一行」，于是每轮收尾都要做「归并/压缩」，还被用户
注意到并纠正。

⇒ **不要给自己的记忆文件设体积上限**，更不要为凑体积去删已有条目。
速查条目可以直接写全（速查的价值就是不用再翻页）；完整推导仍写进该项目的存档文件。
真觉得体积有问题，就说清代价并问用户，而不是默默压缩。

