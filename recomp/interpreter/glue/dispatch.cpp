// Made by tools/gen_interpreter_glue.py from psprism's output: everything is interpreted.
#include "generated.hpp"
#include <platform/platform.h>
#include <psprecomp/interpreter.hpp>
#include <psprecomp/patch.hpp>

namespace psprecomp::generated {
const char* function_symbol(std::uint32_t) { return nullptr; }

// Hooks, then the firmware functions; anything else is the interpreter's (false).
bool dispatch_block(State& state, std::uint32_t current_pc) {
  const auto dispatch_offset = current_pc - state.memory_base;
  if (const auto* patch = !psprecomp::patch::any_patches() ? nullptr : psprecomp::patch::find_patch(state, current_pc, nullptr)) [[unlikely]] {
    return psprecomp::patch::invoke_patch(*patch, state, current_pc);
  }
  return !state.branch_pending && dispatch_offset >= 0x0090de38U && dispatch_offset <= 0x0090e6b0U &&
         platform::dispatch_import(state, current_pc);
}

void run(State& state, std::uint32_t return_address, std::uint64_t max_steps) {
  state.stop_reason = StopReason::running;
  for (std::uint64_t block = 0; block < max_steps; ++block) {
    if (state.pc == return_address && !state.branch_pending) {
      state.stop_reason = StopReason::returned; return;
    }
    const std::uint32_t current_pc = state.pc;
    note_cpu_dispatch(state);
    const bool translated = dispatch_block(state, current_pc);
    if (translated) {
      note_cpu_translated_block(state);
    } else {
      note_cpu_interpreter_fallback(state);
    }
#ifdef PSPRECOMP_PROFILE_DISPATCH
    profile_report(state, current_pc);
#endif
    if (!translated && !interpret_allegrex(state, current_pc)) {
      if (state.stop_reason == StopReason::running) {
        state.stop_reason = StopReason::invalid_pc;
        state.fault_address = current_pc;
        state.fault_pc = current_pc;
      }
      return;
    }
    if (state.stop_reason != StopReason::running) { return; }
  }
  state.stop_reason = StopReason::step_limit;
}
} // namespace psprecomp::generated

namespace psprecomp::patch::bridge {
bool execute_function_at(State& state, std::uint32_t address, std::uint64_t max_blocks) {
  state.pc = address;
  for (std::uint64_t block = 0; block < max_blocks; ++block) {
    if (state.pc == CALL_RETURN_SENTINEL && !state.branch_pending) return true;
    const std::uint32_t current_pc = state.pc;
    const bool translated = generated::dispatch_block(state, current_pc);
    if (!translated && !interpret_allegrex(state, current_pc)) {
      if (state.stop_reason == StopReason::running) {
        state.stop_reason = StopReason::invalid_pc;
        state.fault_address = current_pc;
        state.fault_pc = current_pc;
      }
      return false;
    }
    if (state.stop_reason != StopReason::running) return false;
  }
  state.stop_reason = StopReason::step_limit;
  return false;
}
} // namespace psprecomp::patch::bridge
