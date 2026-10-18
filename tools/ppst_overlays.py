#!/usr/bin/env python3
"""Extract the loaded code overlays from a PPSSPP save state.

The save state holds the PSP's 32 MB of user RAM. Each overlay the game has
loaded starts with an "MWo3" header (Metrowerks overlay) carrying its ID, load
address and name, so we scan the overlay load addresses for those headers and
cut each overlay out using its size from symbols/overlays.tsv.

The EBOOT's code in RAM is compared against the dumped EBOOT first, to make
sure the save state comes from the same game version.

Needs the zstandard module (pip install zstandard).

usage: ppst_overlays.py STATE.ppst EBOOT.BIN OVERLAYS.tsv OUT_DIR
Writes OUT_DIR/<name>.bin for each loaded overlay and prints one line per
overlay: name, load address, size.
"""
import struct
import sys
from pathlib import Path

import zstandard

HEADER_SIZE = 48 + 128  # SChunkHeader + title (save state revision >= 5)
COMPRESS_ZSTD = 2
RAM_BASE = 0x08000000
TEXT_ADDR, TEXT_FILE_OFFSET, TEXT_SIZE = 0x08804000, 0x25B4, 0x109E38  # .text of ULES01213


def load_state(path):
    raw = Path(path).read_bytes()
    revision, compress, size, uncompressed_size = struct.unpack_from("<iiII", raw)
    if revision < 5 or compress != COMPRESS_ZSTD:
        sys.exit(f"unsupported save state (revision {revision}, compression {compress})")
    body = raw[HEADER_SIZE:HEADER_SIZE + size]
    return zstandard.ZstdDecompressor().decompress(body, max_output_size=uncompressed_size)


def main(state_path, eboot_path, overlays_tsv, out_dir):
    state = load_state(state_path)
    eboot = Path(eboot_path).read_bytes()
    probe = eboot[TEXT_FILE_OFFSET:TEXT_FILE_OFFSET + 64]

    # The RAM image is stored as one block; find it by locating the EBOOT's code.
    hit = state.find(probe)
    if hit == -1:
        sys.exit("EBOOT code not found in save state: different game or version?")
    ram = state[hit - (TEXT_ADDR - RAM_BASE):]
    text = TEXT_ADDR - RAM_BASE
    if ram[text:text + TEXT_SIZE] != eboot[TEXT_FILE_OFFSET:TEXT_FILE_OFFSET + TEXT_SIZE]:
        sys.exit("EBOOT code in save state differs from the dumped EBOOT: different version?")

    overlays = {}
    for line in Path(overlays_tsv).read_text().splitlines()[1:]:
        name, addr, size = line.split("\t")
        overlays[name] = (int(addr, 16), int(size))
    load_addresses = sorted({addr for addr, _ in overlays.values()})

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    found = 0
    for addr in load_addresses:
        header = ram[addr - RAM_BASE:addr - RAM_BASE + 64]
        if header[:4] != b"MWo3":
            continue
        _, header_addr = struct.unpack_from("<II", header, 4)
        name = header[0x20:header.index(b"\0", 0x20)].decode()
        if name not in overlays or header_addr != addr:
            sys.exit(f"unexpected overlay header at {addr:08X}: {name!r}")
        size = overlays[name][1]
        (out / f"{name}.bin").write_bytes(ram[addr - RAM_BASE:addr - RAM_BASE + size])
        print(f"{name}\t{addr:08X}\t{size}")
        found += 1
    if not found:
        print("no overlays loaded in this save state", file=sys.stderr)


if __name__ == "__main__":
    main(*sys.argv[1:5])
