#!/usr/bin/env python3
"""
140-build-ipk.py —— 纯 Python 构造 OpenWrt 可安装的 .ipk

为什么不调 `ipkg-build`：
  · 本项目主力开发机是 Windows(Git Bash)，没有 fakeroot / OpenWrt 构建树；
  · .ipk 格式本身极简单（就是一个 "ar" 归档 + 两个 tar.gz），自己写反而
    完全可控、可复现、零外部依赖。

.ipk 结构（成员顺序固定，opkg 依赖它）：
    !<arch>\\n
    debian-binary   内容固定 "2.0\\n"
    control.tar.gz  ./control  必需；./conffiles ./postinst ./prerm 可选
    data.tar.gz     ./etc/...  ./usr/...    （条目带 "./" 前缀）

ar 成员头（60 字节，全 ASCII，左对齐空格填充）：
    name[16] mtime[12] uid[6] gid[6] mode[8] size[10] end[2]='`\\n'
    奇数长度内容后补一个 '\\n' 对齐到偶数字节。

用法：
    python 140-build-ipk.py --src fm160-qmi --out dist/ \\
        --name fm160-qmi --version 1.0.0-1 --arch all \\
        --depends "uqmi netifd" --conffiles /etc/config/fm160-qmi
"""
import argparse
import gzip
import io
import os
import sys
import tarfile
import time

EPOCH = 1700000000  # 固定 mtime，保证可复现（SOURCE_DATE_EPOCH 风格）


# ── tar.gz ────────────────────────────────────────────────────────────────
def add_bytes(tf, arcname, data, mode, mtime=EPOCH):
    ti = tarfile.TarInfo(arcname)
    ti.size = len(data)
    ti.mode = mode
    ti.uid = 0
    ti.gid = 0
    ti.uname = "root"
    ti.gname = "root"
    ti.mtime = mtime
    tf.addfile(ti, io.BytesIO(data))


def make_tar_gz(entries, out_path, mtime=EPOCH):
    """entries: list of (arcname, bytes-or-path, mode)"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for arc, src, mode in entries:
            if isinstance(src, (bytes, bytearray)):
                add_bytes(tf, arc, bytes(src), mode, mtime)
            else:
                with open(src, "rb") as f:
                    add_bytes(tf, arc, f.read(), mode, mtime)
    raw = buf.getvalue()
    with open(out_path, "wb") as f:
        # filenames='' + mtime=0 ⇒ 不把本机路径/时间写进 gzip 头，可复现
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
            gz.write(raw)
    return len(raw)


# ── ar ────────────────────────────────────────────────────────────────────
def ar_header(name, size, mtime=0, mode=0o100644):
    if len(name) > 15:
        raise ValueError("ar 短名上限 15 字节（超长需要 // 名字表）: %r" % name)
    h = "%-16s%-12d%-6d%-6d%-8o%-10d`\n" % (name + "/", mtime, 0, 0, mode, size)
    assert len(h) == 60, len(h)
    return h.encode("ascii")


def make_ar(out_path, members):
    """members: list of (name, bytes)"""
    with open(out_path, "wb") as f:
        f.write(b"!<arch>\n")
        for name, data in members:
            f.write(ar_header(name, len(data)))
            f.write(data)
            if len(data) % 2:
                f.write(b"\n")
    return os.path.getsize(out_path)


# ── 外层容器：★★★ OpenWrt 24.10 官方 ipk 其实是 gzip+tar，不是 ar ★★★ ────
#
# 真机取证（2026-09-21，本项目最强的一课）：
#   `opkg install` 我们自制的 **ar 版 ipk** 恒报
#       Collected errors:
#        * pkg_init_from_file: Malformed package file /tmp/xxx.ipk.
#   （设备 opkg 版本 `38eccbb1fd694d4798ac1baf88f9ba83d1eac616 (2024-10-16)`）
#   于是用 `opkg download zlib` 从官方镜像取一个**真包**做逐字节对照：
#       $ head -c 8 zlib_1.3.1-r1_aarch64_generic.ipk
#       1f 8b 08 00 00 00 00 00        ← **gzip magic**，不是 "!<arch>\n"
#       解压后是 **tar**，首成员 `./debian-binary`（GNU ustar，"ustar  \0"），
#       随后 `./control.tar.gz`、`./data.tar.gz`
#   ⇒ 官方 ipk = `gzip(tar( ./debian-binary, ./control.tar.gz, ./data.tar.gz ))`
#      —— 一个**嵌套归档**（外层 tar 里装着两个 tar.gz 文件），opkg 靠
#      libarchive 的嵌套打开能力读取。
#
# 对照结论：去掉成员名末尾的 `/' 也照样被拒 ⇒ **本 opkg 不认 ar 容器**，
#           必须输出 targz 外层。默认就用 targz。
def make_outer_targz(out_path, members, mtime=EPOCH):
    """members: list of (arcname, bytes)；arcname 已带 './' 前缀。GNU tar + gzip。"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for arc, data in members:
            add_bytes(tf, arc, data, 0o644, mtime)
    raw = buf.getvalue()
    with open(out_path, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
            gz.write(raw)
    return os.path.getsize(out_path)


# ── 主流程 ────────────────────────────────────────────────────────────────
# 这些目录下的文件一律 0755，即使源树没带 exec 位
BIN_DIRS = ("etc/init.d/", "usr/sbin/", "usr/bin/", "etc/rc.d/", "etc/hotplug.d/")


def is_exec(rel, ap):
    """判断是否应带 +x。

    ★★ 必须用「shebang / 目录约定」推导，不能只看 st_mode：
       Windows/NTFS 与 Git Bash **不保存可执行位**，`st_mode & 0o111` 恒为 0，
       直接按它推导会把 init 脚本和 /usr/sbin 下的可执行文件全打成 0644
       —— 装到设备上就是「command not found / 服务起不来」。
    """
    if rel.startswith(BIN_DIRS):
        return True
    if os.stat(ap).st_mode & 0o111:  # Linux 上有真实权限位时以它为准
        return True
    try:
        with open(ap, "rb") as f:
            return f.read(2) == b"#!"
    except OSError:
        return False


def collect(src, exclude, force_exec, force_plain):
    """遍历 src，返回 (arcname, abspath, mode)，arcname 带 './' 前缀。"""
    out = []
    src = os.path.abspath(src)
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for fn in sorted(files):
            ap = os.path.join(root, fn)
            rel = os.path.relpath(ap, src).replace(os.sep, "/")
            if rel in exclude:
                continue
            mode = 0o644
            if rel in force_exec:
                mode = 0o755
            elif rel in force_plain:
                mode = 0o644
            elif is_exec(rel, ap):
                mode = 0o755
            out.append(("./" + rel, ap, mode))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="包文件树根目录（内含 etc/ usr/ ...）")
    ap.add_argument("--out", default="dist", help="输出目录")
    ap.add_argument("--name", required=True)
    ap.add_argument("--version", default="1.0.0-1")
    ap.add_argument("--arch", default="all")
    ap.add_argument("--depends", default="")
    ap.add_argument("--section", default="net")
    ap.add_argument("--priority", default="optional")
    ap.add_argument("--maintainer", default="Beaverfffan")
    ap.add_argument("--description", default="")
    ap.add_argument("--conffiles", default="", help="空格分隔的绝对路径")
    ap.add_argument("--postinst", default=None, help="postinst 脚本文件路径")
    ap.add_argument("--exclude", default="", help="相对路径，空格分隔")
    ap.add_argument("--exec", dest="force_exec", default="", help="强制 0755 的相对路径")
    ap.add_argument("--plain", dest="force_plain", default="", help="强制 0644 的相对路径")
    ap.add_argument("--format", choices=("targz", "ar"), default="targz",
                    help="外层容器格式；targz = OpenWrt 24.10 官方形态（默认），ar = 老式（本机 opkg 拒收）")
    a = ap.parse_args()

    exclude = set(x for x in a.exclude.split() if x)
    force_exec = set(x for x in a.force_exec.split() if x)
    force_plain = set(x for x in a.force_plain.split() if x)
    entries = collect(a.src, exclude, force_exec, force_plain)
    if not entries:
        sys.exit("★ %s 下没有可打包的文件" % a.src)

    os.makedirs(a.out, exist_ok=True)
    data_tgz = os.path.join(a.out, "data.tar.gz")
    ctrl_tgz = os.path.join(a.out, "control.tar.gz")
    ipk = os.path.join(a.out, "%s_%s_%s.ipk" % (a.name, a.version, a.arch.split("_")[0]))

    make_tar_gz(entries, data_tgz)

    desc = a.description.replace("\n", "\n ")
    control = (
        "Package: %s\n"
        "Version: %s\n"
        "Depends: %s\n"
        "Source: package/%s\n"
        "SourceName: %s\n"
        "License: MIT\n"
        "Section: %s\n"
        "Priority: %s\n"
        "Maintainer: %s\n"
        "Architecture: %s\n"
        "Installed-Size: %d\n"
        "Description: %s\n"
        % (
            a.name, a.version, a.depends, a.name, a.name,
            a.section, a.priority, a.maintainer, a.arch,
            sum(os.path.getsize(e[1]) for e in entries) // 1024 + 1,
            desc,
        )
    )

    ctrl = [("./control", control.encode(), 0o644)]
    cf = [x for x in a.conffiles.split() if x]
    if cf:
        ctrl.append(("./conffiles", ("\n".join(cf) + "\n").encode(), 0o644))
    if a.postinst:
        with open(a.postinst, "rb") as f:
            ctrl.append(("./postinst", f.read(), 0o755))
    make_tar_gz(ctrl, ctrl_tgz)

    with open(data_tgz, "rb") as f:
        data_blob = f.read()
    with open(ctrl_tgz, "rb") as f:
        ctrl_blob = f.read()

    if a.format == "ar":
        size = make_ar(
            ipk,
            [("debian-binary", b"2.0\n"), ("control.tar.gz", ctrl_blob), ("data.tar.gz", data_blob)],
        )
    else:
        size = make_outer_targz(
            ipk,
            [("./debian-binary", b"2.0\n"),
             ("./control.tar.gz", ctrl_blob),
             ("./data.tar.gz", data_blob)],
        )

    print("=== %s ===" % ipk)
    print("  format: %s（外层容器）" % a.format)
    print("  files : %d" % len(entries))
    print("  size  : %d B (%.1f KiB)" % (size, size / 1024))
    print("  arch  : %s   depends: %s" % (a.arch, a.depends or "(none)"))
    for arc, _, mode in entries:
        print("    %-42s %o" % (arc, mode))
    os.remove(data_tgz)
    os.remove(ctrl_tgz)


if __name__ == "__main__":
    main()
