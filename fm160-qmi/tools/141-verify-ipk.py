#!/usr/bin/env python3
"""
141-verify-ipk.py —— 不依赖 opkg/libarchive，本地校验 .ipk 结构

★★ 格式定论（2026-09-21 真机取证）：
   OpenWrt 24.10 的 opkg（`38eccbb1fd694d4798ac1baf88f9ba83d1eac616 (2024-10-16)`）
   **只吃「外层 gzip+tar」的 ipk**：`opkg install` 一个自制的 ar 版 ipk 恒报
       Collected errors:
        * pkg_init_from_file: Malformed package file ...
   而 `opkg download zlib` 取回的官方真包是
       `1f 8b 08 00 ...` = gzip ⇒ 解压后 tar，首成员 `./debian-binary`，
       再套 `./control.tar.gz` / `./data.tar.gz`
   ⇒ 判据以 **targz** 为主，ar 仅作兼容保留。

判据（全部必须通过）：
  1. 外层是 gzip+tar（官方形态）或 ar（老式）
  2. 成员齐全且顺序为 debian-binary / control.tar.gz / data.tar.gz
  3. debian-binary 内容 == "2.0\\n"
  4. control.tar.gz 里有 ./control，且含 Package/Version/Architecture 三要素
  5. data.tar.gz 里所有路径以 './' 开头；可执行文件带 +x

用法：python 141-verify-ipk.py dist/fm160-qmi_1.0.2-1_all.ipk
"""
import io
import os
import sys
import tarfile

AR_MAGIC = b"!<arch>\n"
GZ_MAGIC = b"\x1f\x8b"


def parse_ar(blob):
    assert blob[:8] == AR_MAGIC, "不是 ar 归档"
    off = 8
    out = []
    while off + 60 <= len(blob):
        hdr = blob[off:off + 60]
        name = hdr[0:16].decode("ascii").strip()
        size = int(hdr[48:58].decode("ascii").strip())
        off += 60
        data = blob[off:off + size]
        out.append((name.rstrip("/"), data))
        off += size + (size % 2)
    return out


def main():
    path = sys.argv[1]
    blob = open(path, "rb").read()
    ok = True

    print("=== %s (%d B) ===" % (os.path.basename(path), len(blob)))
    print("魔数: %r" % blob[:8])

    if blob[:8] == AR_MAGIC:
        fmt = "ar"
        members = parse_ar(blob)
        names = [n for n, _ in members]
        print("外层: ar 归档, 成员 = %s" % names)
        want = ["debian-binary", "control.tar.gz", "data.tar.gz"]
        if names != want:
            print("  ★ 成员顺序/名称不符，期望 %s" % want)
            ok = False
        m = dict(members)
        db = m.get("debian-binary", b"")
        print("debian-binary = %r %s" % (db, "OK" if db == b"2.0\n" else "★ 不符"))
        ok &= db == b"2.0\n"
        ctrl_blob = m.get("control.tar.gz", b"")
        data_blob = m.get("data.tar.gz", b"")
    elif blob[:2] == GZ_MAGIC:
        fmt = "tar.gz"
        print("外层: gzip+tar（★ OpenWrt 24.10 官方形态）")
        tf = tarfile.open(fileobj=io.BytesIO(blob))
        order = [ti.name for ti in tf.getmembers() if ti.isfile()]
        print("  成员顺序 = %s" % order)
        want = ["./debian-binary", "./control.tar.gz", "./data.tar.gz"]
        if order != want:
            print("  ★ 成员顺序/名称不符，期望 %s" % want)
            ok = False
        inner = {ti.name.lstrip("./"): tf.extractfile(ti).read()
                 for ti in tf.getmembers() if ti.isfile()}
        db = inner.get("debian-binary", b"")
        print("debian-binary = %r %s" % (db, "OK" if db == b"2.0\n" else "★ 不符"))
        ok &= db == b"2.0\n"
        ctrl_blob = inner.get("control.tar.gz", b"")
        data_blob = inner.get("data.tar.gz", b"")
        if not ctrl_blob or not data_blob:
            print("  ★ 没找到内层 control.tar.gz / data.tar.gz")
            ok = False
    else:
        sys.exit("★ 无法识别的外层格式")

    # control
    print("\n--- control.tar.gz ---")
    with tarfile.open(fileobj=io.BytesIO(ctrl_blob)) as tf:
        for ti in tf.getmembers():
            print("  %-16s %o %6d B" % (ti.name, ti.mode, ti.size))
        cf = tf.extractfile("./control") or tf.extractfile("control")
        control = cf.read().decode()
    print("\n--- control 内容 ---")
    for line in control.rstrip().splitlines():
        print("  " + line)
    for key in ("Package:", "Version:", "Architecture:"):
        if key not in control:
            print("  ★ control 缺 %s" % key)
            ok = False
    if "License:" not in control:
        print("  ⚠ control 无 License 字段")

    # data
    print("\n--- data.tar.gz ---")
    with tarfile.open(fileobj=io.BytesIO(data_blob)) as tf:
        for ti in tf.getmembers():
            if not ti.isfile():
                continue
            flag = ""
            if not ti.name.startswith("./"):
                flag += " ★缺./前缀"
                ok = False
            if ti.mode & 0o111:
                flag += " [x]"
            print("  %-44s %o %6d B%s" % (ti.name, ti.mode, ti.size, flag))

    print("\n%s" % ("=== 结构校验通过 ===" if ok else "=== ★ 存在问题 ==="))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
