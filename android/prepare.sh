#!/bin/sh
# Fetches what the Android app is built from into build/android (Gradle runs this before each
# build): psprism at play.sh's PSPRISM_COMMIT with recomp/psprism-mhfu.patch applied, SDL2's
# source, FFmpeg's H.264 decoder for the game's videos (built with the NDK in ANDROID_NDK), and
# the launcher's fonts and logo (the app's assets). Nothing of the game's: the app reads that from
# the player's own ISO.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$REPO/build/android
PSPRISM_COMMIT=$(sed -n 's/^PSPRISM_COMMIT=//p' "$REPO/play.sh")
SDL_VERSION=2.32.10
FFMPEG_VERSION=7.1.2
mkdir -p "$OUT"

if ! cmp -s "$REPO/recomp/psprism-mhfu.patch" "$OUT/psprism/.mhfu-patch" 2>/dev/null; then
	[ -d "$OUT/psprism/.git" ] || git clone -q https://github.com/anthonyhfm/psprism "$OUT/psprism"
	git -C "$OUT/psprism" checkout -q -f "$PSPRISM_COMMIT"
	git -C "$OUT/psprism" clean -q -fdx
	git -C "$OUT/psprism" apply "$REPO/recomp/psprism-mhfu.patch"
	cp "$REPO/recomp/psprism-mhfu.patch" "$OUT/psprism/.mhfu-patch"
fi

if [ ! -f "$OUT/SDL2/.version-$SDL_VERSION" ]; then
	rm -rf "$OUT/SDL2" && mkdir -p "$OUT/SDL2"
	curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" |
		tar xz -C "$OUT/SDL2" --strip-components=1
	touch "$OUT/SDL2/.version-$SDL_VERSION"
fi

# FFmpeg: only what the game's videos need (refract's media engine: H.264, and swscale to convert
# the pictures), static, for arm64-v8a and Android 10 (the app's minSdk).
if [ ! -f "$OUT/ffmpeg/.version-$FFMPEG_VERSION" ]; then
	: "${ANDROID_NDK:?set ANDROID_NDK to the NDK (Gradle does)}"
	case $(uname -s) in Darwin) host=darwin-x86_64 ;; *) host=linux-x86_64 ;; esac
	tools=$ANDROID_NDK/toolchains/llvm/prebuilt/$host/bin
	rm -rf "$OUT/ffmpeg-src" "$OUT/ffmpeg-build" "$OUT/ffmpeg"
	git clone -q --depth 1 --branch "n$FFMPEG_VERSION" https://github.com/FFmpeg/FFmpeg "$OUT/ffmpeg-src" 2>/dev/null
	mkdir -p "$OUT/ffmpeg-build"
	(cd "$OUT/ffmpeg-build" && "$OUT/ffmpeg-src/configure" --prefix="$OUT/ffmpeg" --target-os=android --arch=aarch64 \
		--enable-cross-compile --cc="$tools/aarch64-linux-android29-clang" --cxx="$tools/aarch64-linux-android29-clang++" \
		--ar="$tools/llvm-ar" --ranlib="$tools/llvm-ranlib" --nm="$tools/llvm-nm" --strip="$tools/llvm-strip" \
		--enable-pic --enable-static --disable-shared --disable-programs --disable-doc --disable-everything \
		--disable-network --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample \
		--disable-autodetect --enable-decoder=h264 --enable-swscale >/dev/null &&
		make -j"$(nproc)" >/dev/null && make install >/dev/null)
	rm -rf "$OUT/ffmpeg-src" "$OUT/ffmpeg-build"
	touch "$OUT/ffmpeg/.version-$FFMPEG_VERSION"
fi

# The launcher's fonts (Liberation, standing in for the PSP's, and Cinzel) and logo. Distributions
# keep the Liberation fonts in different places, so search the usual font directories.
ASSETS=$OUT/assets/Resources
mkdir -p "$ASSETS/fonts" "$ASSETS/art"
FONTS=${FONTS:-/usr/share/fonts /usr/local/share/fonts $HOME/.local/share/fonts}
for family in Sans Serif; do
	for style in Regular Bold Italic BoldItalic; do
		# shellcheck disable=SC2086
		font=$(find $FONTS -name "Liberation$family-$style.ttf" 2>/dev/null | head -n 1)
		if [ -z "$font" ]; then
			echo "Liberation$family-$style.ttf not found: install the Liberation fonts" >&2
			exit 1
		fi
		cp "$font" "$ASSETS/fonts/"
	done
done
cp "$REPO/recomp/fonts/Cinzel-Bold.ttf" "$ASSETS/fonts/"
cp "$REPO/recomp/art/logo.png" "$ASSETS/art/"
