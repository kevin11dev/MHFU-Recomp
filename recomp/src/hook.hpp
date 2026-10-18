#pragma once

// Hooks: C++ that runs instead of one of the game's functions, and can call the original.
//
//   bool my_hook(psprecomp::State& state) {
//     const game::Ref<game::Hunter> hunter{state, state.gpr[4]};  // the arguments: $a0 to $a3, $t0...
//     psprecomp::patch::call_original<void>(hunter.address());     // the game's own function
//     ...
//     return return_from_hook(state);
//   }
//   hook(my_hook_node, game::eboot::hunter_read_pad, "my_hook", &my_hook);
//
// A hook on an overlay's function runs whenever code at its address runs, whichever overlay is
// loaded there: check game::loaded(state, function, ...) first, and return false to run the
// game's code instead.

#include "game.hpp"

#include <psprecomp/patch.hpp>

namespace mhfu {

// Returns from the hooked function to its caller, as the original would have.
inline bool return_from_hook(psprecomp::State& state) {
  if (state.stop_reason == psprecomp::StopReason::running) {
    state.pc = state.gpr[31];
    state.branch_pending = false;
  }
  return true;
}

// Runs `function` instead of the game's function at `address`. `node` must stay alive (a global).
inline void hook(psprecomp::patch::PatchNode& node, game::Address address, const char* name,
                 bool (*function)(psprecomp::State&)) {
  psprecomp::patch::register_raw_patch(node, address, name, function);
}

inline void hook(psprecomp::patch::PatchNode& node, game::OverlayFunction function, const char* name,
                 bool (*hook_function)(psprecomp::State&)) {
  hook(node, function.address, name, hook_function);
}

}  // namespace mhfu
