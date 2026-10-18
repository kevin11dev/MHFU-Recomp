#!/bin/sh
# Starts the game, building it first if needed (the first time, or after an update) from your
# own copy of the game: nothing of the game comes with this project.
#
#   ./play.sh                   start; the first time it finds your ISO or asks for its folder
#   ./play.sh /path/to/roms     use the ISO in this folder (or the folders in it)
#   ./play.sh /path/to/game.iso use this ISO
#   ./play.sh --choose-iso      ask for the ISO's folder again
#   ./play.sh --play            start straight away, without the launcher
#
# Everything it builds goes in build/ next to this script.
set -eu
REPO=$(cd "$(dirname "$0")" && pwd)
BUILD=$REPO/build
PSPRISM=$BUILD/psprism
GAME=$BUILD/game
LOG=$BUILD/build.log
PSPRISM_COMMIT=8764f39bb6ee0d10a9c07d8ca2934ce38a5ecb83

FONT_DIRS="/usr/share/fonts /usr/local/share/fonts $HOME/.local/share/fonts"
has_font() {
	# shellcheck disable=SC2086
	[ -n "$(find $FONT_DIRS -name "$1" 2>/dev/null | head -n 1)" ]
}

missing_tools() {
	missing=
	for tool in git make g++ python3 pkg-config sdl2-config; do
		command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
	done
	[ -e /usr/include/GLES3/gl3.h ] || missing="$missing OpenGL-ES-headers"
	{ has_font LiberationSans-Regular.ttf && has_font LiberationSerif-Regular.ttf; } ||
		missing="$missing Liberation-fonts"
	[ -n "$missing" ]
}

# FFmpeg plays the game's videos. Optional: without it they're black, with sound.
has_ffmpeg() {
	pkg-config --exists libavcodec libavformat libavutil libswscale 2>/dev/null
}

if [ "$(uname -s)" != Linux ]; then
	echo "This builds for Linux only so far (on Windows, use WSL; see the README)." >&2
	exit 1
fi

FFMPEG_MARKER=$BUILD/.ffmpeg-install-tried
if missing_tools || { ! has_ffmpeg && [ ! -e "$FFMPEG_MARKER" ]; }; then
	missing_tools && echo "Missing:$missing"
	has_ffmpeg || echo "Missing: FFmpeg (for the game's videos)"
	. /etc/os-release 2>/dev/null || true
	if [ -e /run/ostree-booted ] || [ "${ID:-}" = steamos ]; then
		echo "This system can't install build tools directly. Build inside a distrobox instead:" >&2
		echo "  distrobox create -i ubuntu:24.04 -n mhfu && distrobox enter mhfu" >&2
		echo "then run ./play.sh again in there." >&2
		exit 1
	fi
	SUDO=sudo
	[ "$(id -u)" = 0 ] && SUDO=
	echo "Installing the build tools (may ask for your password)..."
	if command -v apt-get >/dev/null 2>&1; then
		$SUDO apt-get update
		$SUDO apt-get install -y git make g++ python3 pkg-config libsdl2-dev libgles-dev fonts-liberation zenity
		$SUDO apt-get install -y libavcodec-dev libavformat-dev libavutil-dev libswscale-dev || true
	elif command -v pacman >/dev/null 2>&1; then
		$SUDO pacman -S --needed --noconfirm git make gcc python pkgconf sdl2 mesa ttf-liberation zenity
		$SUDO pacman -S --needed --noconfirm ffmpeg || true
	elif command -v dnf >/dev/null 2>&1; then
		$SUDO dnf install -y git make gcc-c++ python3 pkgconf-pkg-config SDL2-devel libglvnd-devel \
			liberation-sans-fonts liberation-serif-fonts zenity
		$SUDO dnf install -y 'pkgconfig(libavcodec)' 'pkgconfig(libavformat)' 'pkgconfig(libavutil)' \
			'pkgconfig(libswscale)' || true
	elif command -v zypper >/dev/null 2>&1; then
		$SUDO zypper --non-interactive install git make gcc-c++ python3 pkg-config SDL2-devel \
			Mesa-libGLESv3-devel Mesa-libGLESv2-devel liberation-fonts zenity
		$SUDO zypper --non-interactive install 'pkgconfig(libavcodec)' 'pkgconfig(libavformat)' \
			'pkgconfig(libavutil)' 'pkgconfig(libswscale)' || true
	else
		echo "Install g++, make, git, python3, pkg-config, the SDL2 and OpenGL ES development files," >&2
		echo "the Liberation fonts and (for videos) FFmpeg with your package manager, then run this again." >&2
		exit 1
	fi
	if missing_tools; then
		echo "Still missing after installing:$missing" >&2
		exit 1
	fi
	mkdir -p "$BUILD"
	if has_ffmpeg; then
		rm -f "$GAME/bin/mhfu"  # rebuild with video support
	else
		echo "FFmpeg isn't available here: the game works, but its videos will be black."
		: >"$FFMPEG_MARKER"  # don't ask again on every start
	fi
fi

mkdir -p "$BUILD"
step() {
	echo "$1"
	shift
	if ! "$@" >>"$LOG" 2>&1; then
		tail -n 20 "$LOG" >&2
		echo "Build failed; the full log is in $LOG" >&2
		exit 1
	fi
}

# Your copy of the game (recomp/src/find_game.cpp), found before anything is built from it: the
# one given, the one remembered, one where emulators keep games, or asked for. The game is built
# from its program (the disc's BOOT.BIN), which the finder takes out of it.
FINDER=$BUILD/find_game
if [ ! -x "$FINDER" ] || [ -n "$(find "$REPO/recomp/src/disc.cpp" "$REPO/recomp/src/disc.hpp" \
	"$REPO/recomp/src/find_game.cpp" -newer "$FINDER" | head -n 1)" ]; then
	step "Building the launcher's game finder..." g++ -std=c++20 -O2 -o "$FINDER" \
		"$REPO/recomp/src/disc.cpp" "$REPO/recomp/src/find_game.cpp"
fi
ISO=$("$FINDER" --program "$GAME/BOOT.BIN" "$@")

# The recompiler, with this project's changes applied.
if ! cmp -s "$REPO/recomp/psprism-mhfu.patch" "$PSPRISM/.mhfu-patch" 2>/dev/null; then
	: >"$LOG"
	[ -d "$PSPRISM/.git" ] || step "Downloading the recompiler (psprism)..." \
		git clone -q https://github.com/anthonyhfm/psprism "$PSPRISM"
	git -C "$PSPRISM" checkout -q -f "$PSPRISM_COMMIT"
	git -C "$PSPRISM" clean -q -fdx
	git -C "$PSPRISM" apply "$REPO/recomp/psprism-mhfu.patch"
	step "Building the recompiler (about a minute)..." \
		make -C "$PSPRISM" -j"$(nproc)" CXXFLAGS="-O2 -Wno-error"
	cp "$REPO/recomp/psprism-mhfu.patch" "$PSPRISM/.mhfu-patch"
	rm -f "$GAME/bin/mhfu"
fi

# The game, rebuilt when the recompiler or this project's sources change.
if [ ! -x "$GAME/bin/mhfu" ] || [ -n "$(find "$REPO/recomp/src" "$REPO/recomp/build_linux.sh" \
	"$REPO/recomp/mhfu.map" -type f -newer "$GAME/bin/mhfu" | head -n 1)" ]; then
	rm -f "$GAME/bin/mhfu"
	step "Translating and compiling the game from your copy (a few minutes, only this once)..." \
		"$REPO/recomp/build_linux.sh" "$PSPRISM" "$GAME" "$GAME/BOOT.BIN"
fi

# The finder has found the game, so the game is given just where it is.
PLAY=
for argument do
	case $argument in --play) PLAY=--play ;; esac
done
exec "$GAME/bin/mhfu" $PLAY "$ISO"
