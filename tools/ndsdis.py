#!/usr/bin/env python3
"""21-B9yi(xu108): disassemble a region of a RAM dump (ARM or Thumb).

Until now this project hand-decoded instruction words while chasing bring-up
bugs (see docs/21-rom-bringup.md).  Hand decoding is slow and error prone, so
this helper wraps capstone: give it a dump written by `--dump`, the base address
the dump was taken from, and the code range, and it prints the disassembly plus
the raw words (alongside the trace's `inst=` values, so the two line up).

Name note: not `dis.py` -- that shadows Python's standard library `dis` module.

Examples:
    python tools\\ndsdis.py --dump build\\rc50_mainram.bin --base 0x02000000 ^
        --start 0x02009E40 --count 40 --mode arm
    python tools\\ndsdis.py --dump build\\rc50_mainram.bin --base 0x02000000 ^
        --start 0x02012740 --count 20 --mode thumb

capstone is not part of the standard library.  Install it next to the workspace:
    python -m pip install --target build\\pylibs capstone
(the script puts build/pylibs on sys.path itself, so the workspace stays clean.)
"""

import argparse
import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_here, "..", "build", "pycap"))
sys.path.insert(0, os.path.join(_here, "..", "build", "pylibs"))

try:
    import capstone
except ImportError:  # pragma: no cover - environment dependent
    sys.exit("capstone missing: python -m pip install --target build\\pycap capstone")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True, help="RAM dump file (e.g. build\\x_mainram.bin)")
    ap.add_argument("--base", default="0x02000000", help="address of dump offset 0")
    ap.add_argument("--start", required=True, help="first address to disassemble")
    ap.add_argument("--count", type=int, default=32, help="number of instructions")
    ap.add_argument("--mode", default="arm", choices=("arm", "thumb"))
    args = ap.parse_args()

    base = int(args.base, 0)
    start = int(args.start, 0)
    off = start - base
    if off < 0:
        sys.exit("start address is below the dump base")

    with open(args.dump, "rb") as f:
        data = f.read()
    if off >= len(data):
        sys.exit("start address is past the end of the dump")

    mode = capstone.CS_MODE_THUMB if args.mode == "thumb" else capstone.CS_MODE_ARM
    md = capstone.Cs(capstone.CS_ARCH_ARM, mode)
    md.detail = False

    code = data[off:off + args.count * 4 + 16]
    print("%s: %s @ %08X" % (args.dump, args.mode, start))
    n = 0
    for ins in md.disasm(code, start):
        i = ins.address - start
        raw = code[i:i + ins.size]
        raw_s = " ".join("%02X" % b for b in raw)
        print("  %08X  %-12s %-8s %s" % (ins.address, raw_s, ins.mnemonic, ins.op_str))
        n += 1
        if n >= args.count:
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
