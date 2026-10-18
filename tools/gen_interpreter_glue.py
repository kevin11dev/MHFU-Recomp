#!/usr/bin/env python3
"""Makes the glue for a build that runs the whole game in psprism's interpreter.

A translated build compiles every function of the game's program to C++. An interpreter build
compiles none: it loads the program from the player's disc and interprets it, so the build holds
no code of the game's (the Android app is built this way). It still needs psprism's glue: the
image's constants (generated.hpp), the dispatcher's loop (dispatch.cpp) and the table of the
firmware functions the program imports (platform/), which are addresses and names only.

This takes them from psprism's output for the game's program, without the translated functions:

usage: gen_interpreter_glue.py PSPRISM_OUTPUT_DIR GLUE_DIR
"""
import re
import shutil
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, glue = Path(sys.argv[1]), Path(sys.argv[2])
    header = (source / "generated.hpp").read_text()
    dispatch = (source / "dispatch.cpp").read_text()

    # generated.hpp: the constants and the dispatcher's functions, not the translated ones.
    header = "".join(line for line in header.splitlines(keepends=True) if not line.startswith("bool run_function_"))

    # dispatch.cpp: the range of the import stubs, and run() and execute_function_at() as they are.
    imports = re.search(r"dispatch_offset >= (0x[0-9a-fA-F]+U) && dispatch_offset <= (0x[0-9a-fA-F]+U)", dispatch)
    run = re.search(r"^void run\(.*?^}\n", dispatch, re.S | re.M)
    bridge = re.search(r"^namespace psprecomp::patch::bridge \{.*", dispatch, re.S | re.M)
    if not (imports and run and bridge):
        sys.exit("dispatch.cpp isn't laid out as expected: psprism changed?")
    glue_dispatch = f"""// Made by tools/gen_interpreter_glue.py from psprism's output: everything is interpreted.
#include "generated.hpp"
#include <platform/platform.h>
#include <psprecomp/interpreter.hpp>
#include <psprecomp/patch.hpp>

namespace psprecomp::generated {{
const char* function_symbol(std::uint32_t) {{ return nullptr; }}

// Hooks, then the firmware functions; anything else is the interpreter's (false).
bool dispatch_block(State& state, std::uint32_t current_pc) {{
  const auto dispatch_offset = current_pc - state.memory_base;
  if (const auto* patch = !psprecomp::patch::any_patches() ? nullptr : psprecomp::patch::find_patch(state, current_pc, nullptr)) [[unlikely]] {{
    return psprecomp::patch::invoke_patch(*patch, state, current_pc);
  }}
  return !state.branch_pending && dispatch_offset >= {imports.group(1)} && dispatch_offset <= {imports.group(2)} &&
         platform::dispatch_import(state, current_pc);
}}

{run.group(0)}}} // namespace psprecomp::generated

{bridge.group(0)}"""

    if glue.exists():
        shutil.rmtree(glue)
    (glue / "platform" / "windows").mkdir(parents=True)
    (glue / "generated.hpp").write_text(header)
    (glue / "dispatch.cpp").write_text(glue_dispatch)
    shutil.copy(source / "platform" / "platform.h", glue / "platform" / "platform.h")
    shutil.copy(source / "platform" / "windows" / "platform.cpp", glue / "platform" / "windows" / "platform.cpp")
    print(f"Wrote the interpreter glue to {glue}")


if __name__ == "__main__":
    main()
