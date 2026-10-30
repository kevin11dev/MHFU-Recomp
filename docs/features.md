# Adding a gameplay feature

A feature is a small change to how the game plays, like hold to gather, that players turn on or
off on the settings screen. Each one is a single file in `recomp/src/features/`, and gets its own
row on the settings screen (Off or On) without touching anything else: the launcher lists every
feature, keeps the player's choice in `config.txt` (`feature.<key>=1` or `0`) and starts the ones
turned on. The settings screen scrolls when there are more rows than fit.

A feature that is off changes nothing: its hooks aren't even registered, so the game runs as it
did without it. Features are off by default, so the game plays as the original until the player
chooses otherwise (Quit Game, which only adds a row to a menu, is the exception).

## The parts

- `recomp/src/features.hpp`: `MHFU_FEATURE`, which describes a feature to the launcher.
- `recomp/src/hook.hpp`: `hook`, which runs C++ in place of one of the game's functions, and
  `return_from_hook`.
- `recomp/src/game.hpp`: reads and changes the game's memory by the names in `re/`
  (`hunter.get(Hunter::pad_held)`, `deref(state, global::quest)`, `loaded(state,
  overlay::game_task)`).
- `re/`: the names of the game's functions, globals and structures. `decompiled/` is the game's
  code with those names, to read what a function does; it isn't in the repository: make it from
  your own copy with `tools/run_ghidra.sh` ([`docs/decompilation.md`](decompilation.md)).

## Steps

1. **Find where the game does it.** Look in `re/functions.tsv` and `decompiled/` for the
   function to change. Name what you find in `re/` as you go (`re/README.md`): a field you need
   goes in `structs.tsv`, a function in `functions.tsv`. The build makes them usable from C++ by
   those names.
2. **Write `recomp/src/features/<name>.cpp`.** A `start` function registers its hooks, and
   `MHFU_FEATURE` names the feature:

   ```cpp
   #include "../features.hpp"
   #include "../game.hpp"
   #include "../hook.hpp"

   namespace mhfu {
   namespace {

   using namespace game;

   psprecomp::patch::PatchNode read_pad_node;

   bool read_pad_hook(psprecomp::State& state) {
     const Ref<Hunter> hunter{state, state.gpr[4]};            // the first argument, $a0
     psprecomp::patch::call_original<void>(hunter.address());   // the game's own function
     // ... change what it did ...
     return return_from_hook(state);
   }

   void start(int game_fps) { hook(read_pad_node, eboot::hunter_read_pad, "my_feature", &read_pad_hook); }

   }  // namespace

   MHFU_FEATURE(my_feature, {"my_feature", "My feature", "What it does, in a sentence or two.", false, &start});

   }  // namespace mhfu
   ```

   The five values are the key in `config.txt`, the name on the settings screen, what it does
   (shown there while it's chosen), whether it's on before the player chooses, and `start`. `start` is called once, before the game starts, with
   the frame rate chosen (30 or 60).
3. **Build.** `recomp/build_linux.sh` (or `./play.sh`) compiles every file in `features/`; no
   list to add it to.
4. **Test it on and off.** Turn it on in the settings screen and play; then off, and check that
   the game behaves as before. Try both 30 and 60 fps.

## Guidelines

- **Let the game do the work.** Change what the game sees, the buttons pressed or a value it
  reads, and let its own code act on it, rather than doing what the game would (starting an
  action, giving an item). The game then keeps its own rules: animations, sounds, what it checks
  first. Hold to gather presses the button again; the game decides whether the hunter gathers.
- **Overlays share addresses.** Code on a hunt (`game_task`) and in the village (`lobby_task`)
  is loaded at the same place, so a hook on an overlay's function also runs for whatever else is
  there. Check `loaded(state, function, offset, first, second)` (the overlay and two of the
  function's instructions) and return `false` to run the game's code when it's not yours.
- **The game program is quicker than overlays.** The game program's functions (`eboot::`) are
  translated to C++; overlays run in the interpreter. Calling an overlay's function from a hook
  every frame is fine, a few hundred times a frame isn't.
- **60 fps.** At 60 fps the game runs each step twice as often, and `src/fps60.cpp` evens out
  what the 60 fps code doesn't. A feature that counts frames or moves things a step a frame
  should use the `game_fps` that `start` gets, to keep the same speed in seconds.
- **Keep it in its file.** A feature shouldn't need changes elsewhere, apart from names in
  `re/`. If it needs something shared, put that in `game.hpp` or `hook.hpp` for the next one.

## Example: hold to gather

[`recomp/src/features/hold_to_gather.cpp`](../recomp/src/features/hold_to_gather.cpp). In the
later games, holding the button at a gathering spot keeps gathering; in MHFU you press it again
for each gather.

What the game does (names from `re/`):
- Each frame `hunter_read_pad` copies the pad into the hunter: `Hunter::pad_held` (the buttons
  down) and `Hunter::pad_pressed` (those pressed this frame).
- With the weapon sheathed, `hunter_sheathed_input` acts on `pad_pressed`: Circle tries to
  gather (`hunter_try_gather`), Square uses the item chosen in the item bar, a pickaxe or bug net
  mining or catching bugs at a spot that allows it.
- A hunt's gathering spots come from `quest_gather_points(quest, area)`: a list of
  `GatherPoint`s, each with a position, a radius, a kind (0 gathering, 3 mining, 4 bugs) and the
  gathers left.
- The hunter's action is `Character::action_group` and `action_id`; 0 and 0 is standing with the
  weapon sheathed, what a gather ends in.

What the feature does: it hooks `hunter_read_pad`, lets it copy the pad, and then, if the hunter
is standing at a spot with gathers left and is holding its button (Circle, or Square with a
pickaxe or bug net chosen for those spots), adds the button to `pad_pressed`. The game sees a new
press and starts the next gather by its own rules. When the spot runs out, or the button is let
go, it stops.

Found along the way: the 60 fps code didn't halve the speeds of the gathering, mining and
bug-catching motions (`hunter_gather_action` sets them itself), so at 60 fps they played twice as
fast. That is a 60 fps fix, so it went into `src/fps60.cpp`, not the feature.
