"""Adds a jump trace to the generated dispatcher: prints the last 64 non-sequential blocks on a stop."""
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('''void run(State& state, std::uint32_t return_address, std::uint64_t max_steps) {
  state.stop_reason = StopReason::running;
  for (std::uint64_t block = 0; block < max_steps; ++block) {
    if (state.pc == return_address && !state.branch_pending) {
      state.stop_reason = StopReason::returned; return;
    }
    const std::uint32_t current_pc = state.pc;''', '''static thread_local std::uint32_t trace_ring[64];
static thread_local std::uint32_t trace_translated[64];
static thread_local std::uint32_t trace_pos;
static void trace_dump(const State& state) {
  std::fprintf(stderr, "[trace] stop reason=%u fault_pc=%08x; last jumps (oldest first):\\n", static_cast<unsigned>(state.stop_reason), state.fault_pc);
  for (std::uint32_t i = 0; i < 64; ++i) {
    const auto k = (trace_pos + i) & 63U;
    if (trace_ring[k] != 0) std::fprintf(stderr, "[trace]   %08x %s\\n", trace_ring[k], trace_translated[k] ? "translated" : "interpreted");
  }
}
void run(State& state, std::uint32_t return_address, std::uint64_t max_steps) {
  state.stop_reason = StopReason::running;
  for (std::uint64_t block = 0; block < max_steps; ++block) {
    if (state.pc == return_address && !state.branch_pending) {
      state.stop_reason = StopReason::returned; return;
    }
    const std::uint32_t current_pc = state.pc;
    static thread_local std::uint32_t trace_previous;
    const bool trace_jump = current_pc != trace_previous + 4U;
    trace_previous = current_pc;
    if (trace_jump) trace_ring[trace_pos & 63U] = current_pc;''', 1)
s = s.replace('''    const bool translated = dispatch_block(state, current_pc);
    if (translated) {''', '''    const bool translated = dispatch_block(state, current_pc);
    if (trace_jump) { trace_translated[trace_pos & 63U] = translated; ++trace_pos; }
    if (translated) {''', 1)
s = s.replace('''      return;
    }
    if (state.stop_reason != StopReason::running) { return; }
  }
  state.stop_reason = StopReason::step_limit;
}''', '''      trace_dump(state);
      return;
    }
    if (state.stop_reason != StopReason::running) { trace_dump(state); return; }
  }
  state.stop_reason = StopReason::step_limit;
}''', 1)
assert s.count('trace_dump(state)') == 2, 'patch failed'
open(p, 'w').write('#include <cstdio>\n' + s)
