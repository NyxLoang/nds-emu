#!/usr/bin/env python3
"""21-B9yi(xu108): generate src/bios/bios9_rom.h (FreeBIOS ARM9 image).

Source of the bytes: the very same FreeBIOS image the reference core (melonDS)
uses, i.e. `bios_ntr_arm9[]` in its src/FreeBIOS_Data.h (BSD-2, see the banner
written into the generated header).  This mirrors what
`build/gen_bios7_rom.py` does for the ARM7 image.

Why we need it: melonDS's `NDS::SetupDirectBoot()` does

    // Copy the Nintendo logo from the NDS ROM header to the ARM9 BIOS if using FreeBIOS
    // Games need this for DS<->GBA comm to work
    memcpy(ARM9BIOS.data() + 0x20, header.NintendoLogo, 0x9C);

and FFXII really does read it back: its boot code runs
`memcpy(0x020798A4, 0xFFFF0020, 0x9C)` (found with the trace/`--watch`, see
docs/21-rom-bringup.md xu108).  Our emulator returned 0 for every ARM9 BIOS read,
so that 156-byte copy became zeros and the game state diverged from the
reference from frame ~50 onward.

Usage:
  python tools/gen_bios9_rom.py [FreeBIOS_Data.h] [out.h]
  (defaults: %TEMP%\\melonds-ref\\src\\FreeBIOS_Data.h -> src/bios/bios9_rom.h)
"""

import os
import re
import sys


def parse_array(text: str, name: str):
    m = re.search(r"unsigned char\s+" + name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("array %s not found" % name)
    body = m.group(1)
    vals = [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]
    if not vals:
        raise SystemExit("array %s has no values" % name)
    return bytes(vals)


BANNER = """/*
   FreeBIOS —— NDS ARM9 BIOS 替代镜像（4KB，真实字节）
   Copyright (c) 2013, Gilead Kutnick
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   1) Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
   2) Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.

   ---------------------------------------------------------------------------
   21-B9yi（续108）：本文件由 tools/gen_bios9_rom.py 生成，来源是参考核
   （melonDS）使用的同一份 FreeBIOS ARM9 镜像（`bios_ntr_arm9[]`）。

   为什么需要：melonDS 的 `NDS::SetupDirectBoot()` 在「没有真 BIOS 文件」时会把
   ROM 头里的**任天堂 logo（0x9C 字节）拷进 ARM9 BIOS 偏移 0x20**，注释写的是
   「Game 需要它做 DS<->GBA 通信」；FFXII 确实把它读回去用：
   `memcpy(0x020798A4, 0xFFFF0020, 0x9C)`（用 trace + `--watch` 定位，
   见 docs/21-rom-bringup.md 续108）。本模拟器此前对 ARM9 BIOS 区一律读 0，
   于是这 156 字节变成全 0、游戏状态从第 50 帧左右起与参考核分叉。

   镜像长度 %#06x 字节，其余按参考核口径补 0。
*/

#ifndef NDS_EMU_BIOS9_ROM_H
#define NDS_EMU_BIOS9_ROM_H

#define BIOS9_ROM_SIZE 0x1000u
#define BIOS9_ROM_VALID %#06xu

static const unsigned char bios9_rom[BIOS9_ROM_SIZE] = {
"""


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("TEMP", "."), "melonds-ref", "src", "FreeBIOS_Data.h")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "bios", "bios9_rom.h")

    with open(src, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    img = parse_array(text, "bios_ntr_arm9")
    size = 0x1000
    if len(img) > size:
        raise SystemExit("FreeBIOS ARM9 image larger than %#x" % size)
    data = img + bytes(size - len(img))

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(BANNER % (len(img), len(img)))
        for i in range(0, size, 16):
            row = ", ".join("0x%02X" % b for b in data[i:i + 16])
            f.write("    " + row + ",\n")
        f.write("};\n\n#endif /* NDS_EMU_BIOS9_ROM_H */\n")
    print("wrote %s (%d valid bytes out of %d)" % (out, len(img), size))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
