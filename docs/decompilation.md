# Decompilation notes

The reference decompilation of the EBOOT and how the game's code and data are laid out. The
recomp in `recomp/` builds on these findings; see the [README](../README.md) for the project
itself.

Decompilation of **Monster Hunter Freedom Unite** (PSP, EU release `ULES01213`), starting from the
game's program: the disc's `PSP_GAME/SYSDIR/BOOT.BIN` (the same code as the decrypted `EBOOT.BIN`
PPSSPP dumps).

Ghidra's C-like output for every function in the EBOOT and in the overlays it runs with (the
title screen's, the village's and the hunts'), with the names and types of the reverse
engineering database in [`re/`](../re/README.md) applied: `hunter->stamina` rather than
`*(short *)(param_1 + 0x55a)`. Functions not named yet keep Ghidra's names (`FUN_0880f68c`,
`DAT_...`, `param_1`); naming more is a matter of adding them to `re/` and running Ghidra again.

> The game's program, its overlays and the decompiled code are Capcom's, so none of them are in
> the repository: the tools below make them on your computer from your own copy, at the paths
> marked *local* (git-ignored). Don't commit or share them.

## Contents

| Path | What it is |
| --- | --- |
| `ULES01213_EBOOT.BIN` | *Local:* the game's program (ELF), from your disc ([Regenerating](#regenerating)) |
| `decompiled/eboot/*.c` | *Local:* decompiled C, one file per 16 KB of code, named by start address |
| `decompiled/eboot/functions.tsv` | *Local:* every function: address, size, name, file it's in |
| `decompiled/eboot/strings.tsv` | *Local:* every string Ghidra found, with its address |
| `symbols/imports.tsv` | PSP firmware imports: stub address, library, NID, name (no header row) |
| `symbols/overlays.tsv` | The game's code overlays: name, load address, size in memory |
| `savestates/*.ppst` | *Local, optional:* PPSSPP save states to take overlays from |
| `overlays/*.ovl.bin` | *Local, optional:* overlay images cut out of the save states' RAM |
| `decompiled/<overlay>/` | *Local:* decompiled overlays, laid out like `decompiled/eboot/` |
| `re/` | The reverse engineering database: names, signatures and notes for functions, globals and structures |
| `tools/` | Scripts that regenerate everything above |

## What's in the EBOOT

- **Code:** `.text` at `0x08804000`–`0x0890DE38` (1.04 MB). Ghidra found 3,115 functions
  covering 95.6% of it, and all of them decompiled.
- **Imports:** 272 PSP firmware functions from 34 libraries, all named (threads, file I/O,
  audio, graphics, ad-hoc networking, and so on). Their names appear in the decompiled code,
  e.g. `sceKernelCreateThread`.
- **Compiler:** `MW MIPS C Compiler (2.4.1.01)` — Metrowerks CodeWarrior for PSP, from the
  `.comment` section. A byte-matching decomp would need this exact compiler.
- **Module name:** `MonsterHunterPortable2ndG` (MHFU is the western release of MHP2G).
- **Files it opens:** `disc0:/PSP_GAME/USRDIR/DATA.BIN` (main data archive),
  `ms0:/PSP/SAVEDATA/ULES01213DAT/` (the optional data install), and `MHP2ND.BIN` /
  `MHP2NDG.BIN` (save data from earlier games).

## Overlays: the missing gameplay code

Much of the game is not in the EBOOT. It loads **overlays** — extra code files swapped into
fixed addresses as needed. The EBOOT only records their names and sizes
(`symbols/overlays.tsv`):

| Overlay | Address | Size in memory | Likely contents |
| --- | --- | --- | --- |
| `game_task.ovl` | `0x09A5F200` | 1.7 MB | Gameplay (quests, hunting) |
| `game_sub.ovl` | `0x09C19000` | 1.0 MB | More gameplay, loaded alongside `game_task` |
| `edit_task.ovl` | `0x09A5F200` | 1.4 MB | Character creation |
| `download_task.ovl` | `0x09A5F200` | 1.3 MB | Downloadable content |
| `lobby_task.ovl` | `0x09A5F200` | 0.8 MB | Multiplayer lobby |
| `em01.ovl` … `em83.ovl` (17) | `0x09D1A180` | 50–290 KB each | Monsters (`em` = enemy) |
| `stage000.ovl` … `stage266.ovl` (267) | `0x09D63000` | ≤ 9 KB each | Maps / areas |

"Likely contents" are guesses from the names. Overlays sharing an address replace each other, so
only one task overlay and one monster overlay can be loaded at a time. Calls from the EBOOT into
overlay addresses show up in the decompiled code as calls to unknown addresses.

The overlay files live on the disc, probably inside `DATA.BIN`, which is encrypted. Instead we
take them from **PPSSPP save states**, which hold the PSP's whole RAM: `tools/ppst_overlays.py`
finds each loaded overlay by its header and cuts it out. Only the overlays loaded at that moment
are in a save state, so different moments in the game give different overlays.

### Overlays so far

| Save state | Overlays | Functions |
| --- | --- | --- |
| `title_screen.ppst` | `demo_task` (title screen / intro demo, 10 KB code) | 11, covering 95.1% |
| | `demo_sub` (intro movie player: "movie manager", video and audio threads, 7.5 KB code) | 21, covering 85.3% |

The rest (including `game_task`, `game_sub` and the monsters) would each need a save state made at
the right moment, so the better route is decrypting `DATA.BIN`, below, which gives all of them at
once.

### Overlay format

Each overlay starts with a 0x40-byte Metrowerks overlay header:

| Offset | Contents |
| --- | --- |
| `0x00` | Magic `MWo3` |
| `0x04` | Overlay ID: its position in `symbols/overlays.tsv`, counting from 1 |
| `0x08` | Load address |
| `0x0C` | Code size |
| `0x10` | Initialized data size |
| `0x14` | BSS size |
| `0x18`, `0x1C` | Two pointers to the end of the data (likely constructor/destructor tables) |
| `0x20` | Name, e.g. `demo_task.ovl` |

Code follows the header, then data, then BSS; the three sizes plus the header add up to the size
in `symbols/overlays.tsv`. The pointer meanings are a guess. Data and BSS in the extracted images
hold whatever the game had stored there when the save state was made, not their initial values.

### Getting every overlay at once: decrypting `DATA.BIN`

`DATA.BIN` is encrypted with a simple cipher whose code is in the EBOOT
(`decompiled/eboot/0884C000.c`): each byte goes through a 256-byte substitution table at
`0x0899A018`, then each 32-bit word is XORed with a key stream from two generators,
`s1 = s1 * 0x7F8D % 0xFFF1` and `s2 = s2 * 0x2345 % 0xFFD9` (key `s2 << 16 | s1`), seeded with the
sector the read starts at. The table of contents at the start (seed 0) lists 6,657 files. Overlays
are stored uncompressed as individual files: `demo_task` is file 63 and `demo_sub` file 75, whose
sizes match their header exactly.

`tools/extract_overlays.py` decrypts the table of contents, finds every file that starts with an
`MWo3` header and writes them all to a zip with a manifest. It only needs Python 3.8+, so it can
run on the PC that has the game:

```sh
python extract_overlays.py DATA.BIN ULES01213_EBOOT.BIN overlays.zip
```

On the real `DATA.BIN` (EU disc) it finds all 298 overlays in under a second; each one's name, ID,
load address and size (with BSS) match the EBOOT's overlay table, and `demo_task` and `demo_sub`
are byte-identical to the copies from the save state. It stops with an error if the table of
contents doesn't decrypt into a valid layout.

## Regenerating

Needs Ghidra 12.1.3 with the [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex)
extension installed (it adds the PSP CPU's instructions), Python 3, and the `DATA.BIN` of your
own disc (`PSP_GAME/USRDIR/DATA.BIN`), which the overlays are taken from; after playing once, the
launcher's copy is in `~/.local/share/mhfu-recomp/disc/PSP_GAME/USRDIR/`. The tools read the game's
program from `ULES01213_EBOOT.BIN`; `find_game` writes it there from your disc:

```sh
# The game's program, from your ISO (or the folder it's in)
g++ -std=c++20 -O2 -o build/find_game recomp/src/disc.cpp recomp/src/find_game.cpp
build/find_game --program ULES01213_EBOOT.BIN /path/to/your/roms

# Every scene: decompiled/eboot/ and the overlays below
tools/run_ghidra.sh /path/to/ghidra_12.1.3 /path/to/DATA.BIN

# Only some of them
tools/run_ghidra.sh /path/to/ghidra_12.1.3 /path/to/DATA.BIN village hunt
```

Overlays share addresses, so the EBOOT is analyzed once for each scene, together with the
overlays loaded in it: `title` (`demo_task`, `demo_sub`), `village` (`lobby_task`, `game_sub`) and
`hunt` (`game_task`, `game_sub`; this one also exports `decompiled/eboot/`). After Ghidra's
analysis, `ApplySymbols.java` applies `re/`, then the code is exported. The Ghidra projects and
the extracted overlays go to `build/ghidra/` (git-ignored). A scene takes about five minutes on 4
cores. With `PPSSPP=/path/to/ppsspp` (only `Core/HLE` is used) `symbols/imports.tsv`, the names of
the firmware functions, is made again.

`tools/ppst_overlays.py` takes overlays out of PPSSPP save states instead (needs the `zstandard`
module); the title screen's were the first ones found that way.
