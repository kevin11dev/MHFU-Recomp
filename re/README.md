# Reverse engineering database

What is known about the game's code and data, in plain text, so that every tool works from the
same names:

- `functions.tsv`: functions, by module (`eboot` or an overlay such as `game_task`) and address,
  with a name, a C signature using the types below, and what the function does.
- `globals.tsv`: global variables in the game program: address, name, type, notes.
- `structs.tsv`: the layout of the game's structures: one line per field (struct, offset, type,
  name, notes). A field of type `size` gives the structure's size; a field named `base` gives the
  structure all of another's fields (for example `Hunter` has all of `Character`'s, which are
  shared by hunters, monsters and villagers; the game's classes interleave theirs with them).

Types: `u8 s8 u16 s16 u32 s32 f32 void` (with `*` for pointers), `vec3` (three `f32`),
`T[n]` for arrays, and any structure in `structs.tsv`. The build checks them (`gen_layout.py`) and
stops at a type it doesn't know: Ghidra would leave every name out instead.

Used by:
- `tools/ghidra_scripts/ApplySymbols.java`: names functions, sets their signatures, creates the
  structures and labels the globals in Ghidra before `decompiled/` is exported (on your computer,
  from your own copy; it isn't in the repository), so the
  decompiled C reads `hunter->stamina` instead of `*(short *)(param_1 + 0x55a)`.
- `tools/gen_layout.py`: writes `game_layout.hpp` (at build time, by `recomp/build_linux.sh`): the
  structures' fields with their types, the functions' and globals' addresses and the overlays,
  which the recomp's own code (`recomp/src/*.cpp`, through `recomp/src/game.hpp`) uses.
- `recomp/make_code_map.py`, through `decompiled/eboot/functions.tsv`: the translated functions
  are named after them.

To name something: add it here, run `tools/run_ghidra.sh` to bring `decompiled/` up to date, and
use it from `recomp/src` by its name.

Names are ours, not Capcom's. Offsets are in bytes. Functions in overlays are only valid while
that overlay is loaded (overlays sharing an address: `lobby_task` in the village, `game_task` on
hunts, both at 0x09A5F200; `game_sub` at 0x09C19000 in both).
