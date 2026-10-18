#!/bin/sh
# Runs the Linux build of the recompiled game, for testing.
#
# usage: recomp/run.sh WORK_DIR [GAME.iso]
#   WORK_DIR  what build_linux.sh built
#   GAME.iso  your disc image; without it the launcher uses the remembered one or asks
#
# Optional environment:
#   FRAMES=N            quit after N frames
#   SHOTS=DIR           save a screenshot every 60 frames (SDL host) to DIR
#   INPUT="FRAME:MASK[:LENGTH],..."  scripted button presses (MASK: PSP button bits in hex)
#   MHFU_DATA_DIR=DIR   keep extracted files and saves in DIR instead of ~/.local/share/mhfu-recomp
#   SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy   run without a display
set -eu
WORK=$1
shift
if [ -n "${FRAMES:-}" ]; then
	export REFRACT_EXIT_AFTER_FRAMES="$FRAMES" REFRACT_HEADLESS_FRAMES="$FRAMES"
fi
if [ -n "${SHOTS:-}" ]; then export REFRACT_SCREENSHOT_DIR="$SHOTS"; fi
if [ -n "${INPUT:-}" ]; then
	export REFRACT_INPUT_SCRIPT="$INPUT" REFRACT_HEADLESS_INPUT="$INPUT"
fi
exec "$WORK/bin/mhfu" "$@"
