#!/usr/bin/env python3
"""生成最小假 .nds，用于不依赖商业 ROM 的装载/解析测试。

用法：python make_fake_rom.py [输出路径]（默认 mini.nds，与脚本同目录）
"""
import struct
import sys
from pathlib import Path

ROM_SIZE = 0x5200  # 覆盖 ARM9/ARM7 镜像区并留余量

# 头字段（小端 u32）
ARM9_OFFSET = 0x00004000
ARM9_ENTRY = 0x02000800
ARM9_RAM = 0x02000800  # 镜像装载到入口地址，复位后从死循环开始执行
ARM9_SIZE = 0x00000100

ARM7_OFFSET = 0x00005000
ARM7_ENTRY = 0x03800000
ARM7_RAM = 0x03800000
ARM7_SIZE = 0x00000080

# ARM9 死循环：B 自己 (0xEAFFFFFE)，小端
B_SELF = bytes.fromhex("FE FF FF EA")

# ARM7 镜像：B self 死循环（阶段 8 起真正执行 ARM7，让它停在自己的 WRAM 里）


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "mini.nds"

    rom = bytearray(ROM_SIZE)

    # 厂商标记（0x000 起 4 字节）
    rom[0x000:0x004] = b"NTR" + b"\x01"

    # ARM9 头字段
    struct.pack_into("<I", rom, 0x020, ARM9_OFFSET)
    struct.pack_into("<I", rom, 0x024, ARM9_ENTRY)
    struct.pack_into("<I", rom, 0x028, ARM9_RAM)
    struct.pack_into("<I", rom, 0x02C, ARM9_SIZE)

    # ARM7 头字段
    struct.pack_into("<I", rom, 0x030, ARM7_OFFSET)
    struct.pack_into("<I", rom, 0x034, ARM7_ENTRY)
    struct.pack_into("<I", rom, 0x038, ARM7_RAM)
    struct.pack_into("<I", rom, 0x03C, ARM7_SIZE)

    # ARM9 镜像：死循环，入口处开始
    rom[ARM9_OFFSET:ARM9_OFFSET + len(B_SELF)] = B_SELF
    # ARM7 镜像：B self 死循环
    rom[ARM7_OFFSET:ARM7_OFFSET + len(B_SELF)] = B_SELF

    out.write_bytes(rom)
    print(f"wrote {out} ({len(rom)} bytes)")
    print(f"arm9 offset={ARM9_OFFSET:08X} entry={ARM9_ENTRY:08X} "
          f"ram={ARM9_RAM:08X} size={ARM9_SIZE:08X}")
    print(f"arm7 offset={ARM7_OFFSET:08X} entry={ARM7_ENTRY:08X} "
          f"ram={ARM7_RAM:08X} size={ARM7_SIZE:08X}")


if __name__ == "__main__":
    main()
