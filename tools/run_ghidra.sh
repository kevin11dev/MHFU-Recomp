#!/bin/sh
# Decompiles the game with Ghidra into decompiled/, with the names and types in re/ applied.
#
# The game program (EBOOT) is analyzed together with the overlays it runs with, taken from your
# own DATA.BIN. Overlays share addresses, so each scene is its own Ghidra program:
#   title    demo_task + demo_sub          (title screen and menus)
#   village  lobby_task + game_sub         (the village)
#   hunt     game_task + game_sub          (hunts; also exports the EBOOT)
# and each overlay is written to decompiled/<overlay>/, the EBOOT to decompiled/eboot/.
#
# Requires Ghidra 12.1.3 with the ghidra-allegrex extension installed and Python 3, and the game's
# program in ULES01213_EBOOT.BIN (git-ignored): find_game --program writes it there from your disc
# (docs/decompilation.md). With PPSSPP=<a PPSSPP checkout> (only Core/HLE is needed),
# symbols/imports.tsv is made again from it.
#
# usage: tools/run_ghidra.sh GHIDRA_INSTALL_DIR DATA.BIN [SCENE...]
set -eu

GHIDRA=$1
DATA=$2
shift 2
SCENES=${*:-title village hunt}
REPO=$(cd "$(dirname "$0")/.." && pwd)
EBOOT=$REPO/ULES01213_EBOOT.BIN
WORK=${WORK:-$REPO/build/ghidra}

if [ ! -f "$EBOOT" ]; then
	echo "$EBOOT not found: write the game's program there with find_game --program (docs/decompilation.md)" >&2
	exit 1
fi
mkdir -p "$WORK"
if [ -n "${PPSSPP:-}" ]; then
	python3 -I "$REPO/tools/psp_imports.py" "$EBOOT" "$PPSSPP" "$REPO/symbols/imports.tsv"
fi
if [ ! -f "$WORK/overlays.zip" ]; then
	python3 -I "$REPO/tools/extract_overlays.py" "$DATA" "$EBOOT" "$WORK/overlays.zip"
fi

for scene in $SCENES; do
	case $scene in
	title) overlays="demo_task demo_sub" ;;
	village) overlays="lobby_task game_sub" ;;
	hunt) overlays="game_task game_sub" ;;
	*) echo "unknown scene: $scene" >&2; exit 1 ;;
	esac
	dir=$WORK/$scene
	rm -rf "$dir/overlays" && mkdir -p "$dir/overlays" "$dir/project"
	for overlay in $overlays; do
		python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); open(sys.argv[3], "wb").write(z.read(sys.argv[2]))' \
			"$WORK/overlays.zip" "$overlay.ovl.bin" "$dir/overlays/$overlay.ovl.bin"
	done
	set -- -postScript ExportDecomp.java "$REPO/decompiled" overlays
	[ "$scene" = hunt ] && set -- "$@" -postScript ExportDecomp.java "$REPO/decompiled" eboot
	"$GHIDRA/support/analyzeHeadless" "$dir/project" mhfu \
		-import "$EBOOT" -overwrite \
		-loader PspElfLoader \
		-scriptPath "$REPO/tools/ghidra_scripts" \
		-preScript NamePspImports.java "$REPO/symbols/imports.tsv" \
		-preScript AddOverlayBlocks.java "$dir/overlays" \
		-postScript ApplySymbols.java "$REPO/re" \
		"$@" \
		-max-cpu "$(nproc)"
done
