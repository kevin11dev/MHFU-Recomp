#!/usr/bin/env python3
"""Turn Ghidra's function list into a psprism code map: recomp/mhfu.map, which the build uses.

Run it again when Ghidra's function list changes (tools/run_ghidra.sh). psprism addresses are
offsets from 0x08000000. Ghidra's function sizes count
every byte in a function's body even when it isn't contiguous, so each range
is clipped at the next function's start to keep ranges from overlapping.

usage: make_code_map.py decompiled/eboot/functions.tsv > recomp/mhfu.map
"""
import sys

BASE = 0x08000000
ENTRY = 0x08906934  # the game program's entry point


def main(functions_tsv):
    rows = []
    for line in open(functions_tsv).read().splitlines()[1:]:
        addr, size, name, _file, _status = line.split("\t")
        rows.append((int(addr, 16), int(size), name))
    rows.sort()

    print("# psprecomp-ghidra-map-v2")
    print(f"entry 0x{ENTRY - BASE:08x}")
    for i, (addr, size, name) in enumerate(rows):
        end = addr + size
        if i + 1 < len(rows):
            end = min(end, rows[i + 1][0])
        print(f"function_range 0x{addr - BASE:08x} 0x{end - BASE:08x} {name}")


if __name__ == "__main__":
    main(sys.argv[1])
