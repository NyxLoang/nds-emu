#!/usr/bin/env python3
"""从 ndstool 的 encryption.cpp 提取 NDS KEY1 (Blowfish) 密钥表，生成 C 头片段。

密钥表 = P 数组(18 字) + 4 个 S 盒(各 256 字)，共 0x1048 字节，
来自 NDS ARM7 BIOS 的 0x30..0x1077。来源：
  https://github.com/devkitPro/ndstool/blob/master/source/encryption.cpp
（`const unsigned char encr_data[]`，与 melonDS/nds-bootstrap 使用的一致。）

用法：
  python tools/gen_key1_table.py <ndstool_encryption_cpp路径> <输出.inc>
"""

import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    # 只取 encr_data 数组内的字节，避免把后面代码里的 0xNN 也抓进来。
    m = re.search(r"const unsigned char encr_data\s*\[\s*\]\s*=\s*\{(.*?)\};",
                  text, re.S)
    if m is None:
        print("未找到 encr_data 数组", file=sys.stderr)
        return 1

    body = m.group(1)
    tokens = re.findall(r"0x([0-9A-Fa-f]{2})", body)
    if len(tokens) != 0x1048:
        print(f"字节数错误：期望 0x1048 (4168)，实际 {len(tokens)}", file=sys.stderr)
        return 1

    lines = []
    for i in range(0, len(tokens), 16):
        chunk = tokens[i:i + 16]
        lines.append("    " + ",".join(f"0x{t}" for t in chunk) + ",")

    out = (
        "/* 本文件由 tools/gen_key1_table.py 自动生成，请勿手改。\n"
        "   NDS KEY1 (Blowfish) 密钥表：P 数组 18 字 + S 盒 4×256 字，共 0x1048 字节。\n"
        "   来源：ndstool source/encryption.cpp 的 encr_data（= NDS ARM7 BIOS 0x30..0x1077）。 */\n"
        + "\n".join(lines)
        + "\n"
    )

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(out)

    print(f"OK: {len(tokens)} 字节 -> {out_path}")
    print(f"前 8 字节: {tokens[:8]}")
    print(f"后 8 字节: {tokens[-8:]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
