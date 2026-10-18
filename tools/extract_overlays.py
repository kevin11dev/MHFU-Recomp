#!/usr/bin/env python3
"""Decrypt MHFU's DATA.BIN and pull out every code overlay.

DATA.BIN is encrypted with a byte substitution (a 256-byte table in the
EBOOT at 0x0899A018) followed by XOR with a key stream from two small
generators (EBOOT functions FUN_0884ec3c, FUN_0884ecac, FUN_0884ea1c):

    s1 = s1 * 0x7F8D % 0xFFF1
    s2 = s2 * 0x2345 % 0xFFD9
    key = s2 << 16 | s1          (one 32-bit word per 4 bytes)

The key stream is seeded with a sector number (byte offset / 2048): 0 for
the table of contents at the start of the file, and each file's first
sector for that file.

Overlays are stored as plain files inside DATA.BIN, each starting with an
"MWo3" header that names it, so we decrypt the first bytes of every file
and keep the ones with that header.

Only needs Python 3.8 or newer, no extra modules.

usage: python extract_overlays.py DATA.BIN EBOOT.BIN OUT.zip
  DATA.BIN   from your own disc: PSP_GAME/USRDIR/DATA.BIN
  EBOOT.BIN  the game's program: the disc's PSP_GAME/SYSDIR/BOOT.BIN (find_game --program),
             or the decrypted EBOOT PPSSPP dumps
  OUT.zip    zip to write the overlays and a manifest into
"""
import array
import os
import struct
import sys
import zipfile

SECTOR = 2048
SBOX_ADDR = 0x0899A018
TEXT_ADDR, TEXT_FILE_OFFSET = 0x08804000, 0x25B4  # ULES01213 EBOOT layout
TOC_SIZE, SIZE_TABLE_SIZE = 0x6808, 0x1968  # read and decrypted back to back by the game
OVERLAY_HEADER_SIZE = 0x40


def load_sbox(eboot_path):
    with open(eboot_path, "rb") as f:
        eboot = f.read()
    if eboot[:4] != b"\x7fELF":
        sys.exit("EBOOT.BIN is not a decrypted ELF: use the disc's PSP_GAME/SYSDIR/BOOT.BIN")
    offset = SBOX_ADDR - TEXT_ADDR + TEXT_FILE_OFFSET
    sbox = eboot[offset:offset + 256]
    if len(set(sbox)) != 256:
        sys.exit("substitution table not found: is this the EU (ULES01213) EBOOT?")
    return sbox


def decrypt(data, seed, sbox):
    """Decrypt bytes read from DATA.BIN; seed is the sector the read started at."""
    s1 = seed & 0xFFFF or 0x7F8D
    s2 = seed >> 16 or 0x2345
    padded = data.translate(sbox) + b"\0" * (-len(data) % 4)
    words = array.array("I")
    assert words.itemsize == 4
    words.frombytes(padded)
    if sys.byteorder == "big":
        words.byteswap()
    for i in range(len(words)):
        s1 = s1 * 0x7F8D % 0xFFF1
        s2 = s2 * 0x2345 % 0xFFD9
        words[i] ^= s2 << 16 | s1
    if sys.byteorder == "big":
        words.byteswap()
    return words.tobytes()[:len(data)]


def main(data_path, eboot_path, out_path):
    sbox = load_sbox(eboot_path)
    data_sectors = os.path.getsize(data_path) // SECTOR

    with open(data_path, "rb") as data:
        tables = decrypt(data.read(TOC_SIZE + SIZE_TABLE_SIZE), 0, sbox)
        toc = struct.unpack_from(f"<{TOC_SIZE // 4}I", tables)
        sizes = struct.unpack_from(f"<{SIZE_TABLE_SIZE // 4}I", tables, TOC_SIZE)

        # Sanity checks: a wrong key gives random numbers, not this structure.
        if data_sectors not in toc or toc[0] * SECTOR < TOC_SIZE + SIZE_TABLE_SIZE:
            sys.exit("table of contents didn't decrypt correctly: wrong EBOOT or DATA.BIN?")
        file_count = toc.index(data_sectors)
        if any(toc[i] >= toc[i + 1] for i in range(file_count)):
            sys.exit("table of contents didn't decrypt correctly: wrong EBOOT or DATA.BIN?")
        exact_sizes = dict(zip(sizes[0::2], sizes[1::2]))
        print(f"DATA.BIN: {file_count} files, {data_sectors * SECTOR:,} bytes")

        found = []
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as out:
            for index in range(file_count):
                sector = toc[index]
                data.seek(sector * SECTOR)
                header = decrypt(data.read(OVERLAY_HEADER_SIZE), sector, sbox)
                if header[:4] != b"MWo3":
                    continue
                overlay_id, addr, code, init_data, bss = struct.unpack_from("<5I", header, 4)
                name = header[0x20:header.index(b"\0", 0x20)].decode("ascii")
                size = OVERLAY_HEADER_SIZE + code + init_data
                span = (toc[index + 1] - sector) * SECTOR
                if size > span or exact_sizes.get(index, size) != size:
                    sys.exit(f"{name}: sizes don't match the table of contents")
                data.seek(sector * SECTOR)
                out.writestr(f"{name}.bin", decrypt(data.read(size), sector, sbox))
                found.append((name, overlay_id, addr, size, index, sector))
                print(f"  {name:22s} file {index:4d}  {size:9,d} bytes")

            manifest = ["name\tid\tload_address\tsize\tfile_index\tsector"]
            manifest += [f"{n}\t{i}\t{a:08X}\t{s}\t{f}\t{sec}" for n, i, a, s, f, sec in found]
            out.writestr("manifest.tsv", "\n".join(manifest) + "\n")

    print(f"Wrote {len(found)} overlays to {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
