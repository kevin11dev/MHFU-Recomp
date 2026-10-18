#!/usr/bin/env python3
"""Name the PSP firmware imports in a decrypted EBOOT.

Reads the ELF's .lib.stub import table (library name, NID table, stub table)
and resolves each NID to a function name using PPSSPP's HLE tables
(Core/HLE/*.cpp in a PPSSPP checkout).

usage: psp_imports.py EBOOT.BIN PPSSPP_DIR OUT.tsv
Output columns: stub_address  library  nid  name
"""
import re
import struct
import sys
from pathlib import Path


def read_sections(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = []
    for i in range(shnum):
        sections.append(struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize))
    strtab_off = sections[shstrndx][4]

    def name_of(sh):
        start = strtab_off + sh[0]
        return data[start:data.index(b"\0", start)].decode()

    return {name_of(sh): sh for sh in sections}


def load_nid_names(ppsspp_dir):
    entry = re.compile(r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,[^,]*,\s*"([A-Za-z_0-9]+)"')
    names = {}
    for cpp in sorted(Path(ppsspp_dir, "Core", "HLE").glob("*.cpp")):
        for nid, name in entry.findall(cpp.read_text(errors="replace")):
            names.setdefault(int(nid, 16), name)
    return names


def main(eboot, ppsspp_dir, out):
    data = Path(eboot).read_bytes()
    sections = read_sections(data)
    phoff, = struct.unpack_from("<I", data, 0x1C)
    # Program header 0 maps the whole image; use it to turn addresses into file offsets.
    _, p_offset, p_vaddr = struct.unpack_from("<III", data, phoff)

    def at(addr):
        return addr - p_vaddr + p_offset

    def cstr(addr):
        off = at(addr)
        return data[off:data.index(b"\0", off)].decode()

    nid_names = load_nid_names(ppsspp_dir)
    stub = sections[".lib.stub"]
    pos, end = stub[4], stub[4] + stub[5]
    rows = []
    while pos < end:
        name_ptr, _ver, _attr, entry_len, _var_count, func_count, nid_ptr, stub_ptr = \
            struct.unpack_from("<IHHBBHII", data, pos)
        lib = cstr(name_ptr) if name_ptr else "?"
        for i in range(func_count):
            nid, = struct.unpack_from("<I", data, at(nid_ptr) + 4 * i)
            name = nid_names.get(nid, f"{lib}_{nid:08X}")
            rows.append((stub_ptr + 8 * i, lib, nid, name))
        pos += entry_len * 4

    with open(out, "w") as f:
        for addr, lib, nid, name in rows:
            f.write(f"{addr:08X}\t{lib}\t{nid:08X}\t{name}\n")
    known = sum(1 for r in rows if not r[3].endswith(f"{r[2]:08X}"))
    print(f"{len(rows)} imports from {len({r[1] for r in rows})} libraries, {known} named")


if __name__ == "__main__":
    main(*sys.argv[1:4])
