# Android

An APK anyone can download from the project's releases, with no computer needed. It holds none of
the game's code: on first start the player picks their ISO, and the game runs from it.

## How it works

- **No translation on the phone.** The app runs the whole game program in psprism's interpreter,
  loaded from the player's disc as it starts (`open_disc` in `recomp/src/launcher.cpp` builds the
  guest memory from the disc's `BOOT.BIN`). `recomp/interpreter/CMakeLists.txt` builds it, with
  the glue in `recomp/interpreter/glue/` (made by `tools/gen_interpreter_glue.py` from psprism's
  output: the image's constants, the dispatcher's loop and the table of imported firmware
  functions, addresses and names only). The same file builds a desktop executable,
  `mhfu-interpreter`, to test on Linux.
- **The ISO, without a path.** Android's file picker gives the app an open file, not a path it
  may open again. `GameActivity` passes it to the launcher as `/proc/self/fd/N`, which
  `disc::open_iso` reads through the open file. The first start copies the game's files out of
  it into the app's files, as on Linux; the game's raw reads of the disc
  (`disc0:/sce_lbn...`, how it reads `DATA.BIN`) are served from those copies (refract's
  `raw_disc_source`), so after that the ISO isn't read again.
- **The rest is the desktop port:** refract, the SDL2 host (drawing with OpenGL ES, which has run faster on
  phones than Vulkan, or Vulkan: the launcher's Graphics setting), the launcher, the camera,
  60 fps, features and multiplayer, built for arm64-v8a with the NDK, and FFmpeg's H.264 decoder
  for the videos. Always the whole screen (Android's bars hidden), landscape either way round,
  with widescreen on by default (the launcher's Widescreen setting: refract's host draws the
  game's 3D world wider to fill a phone's screen, the HUD and menus in the middle, instead of
  bars at the sides). SDL's activity runs the launcher (`GameActivity`, in a process of its
  own); `SetupActivity` comes first: it unpacks the launcher's fonts and logo, has the player
  choose the ISO (kept for next time), and when the game couldn't start (`launch-problem.txt`) or
  stopped unexpectedly, says why (the end of `log.txt`). Saves are in the app's private files
  (`ms0/PSP/SAVEDATA/ULES01213`); holding the app's icon offers **Export save** and **Import
  save** (static shortcuts into `SetupActivity`), which copy the game's save file (`MHP2NDG.BIN`)
  out to, or in from, a file the player picks. The app is marked as a game
  (`android:appCategory`), so the phone's game mode and game booster treat it as one.
- **Measured on desktop** (one core of the cloud machine, 30 fps): interpreting the whole program,
  the game's thread used about 60% of a core walking through the village (40% translated) and
  57% standing in a dialog (28% translated), and held 26–30 frames a second.
- **On a phone:** a Samsung Galaxy A37 played from the opening into the village and the Gathering
  Hall (multiplayer) at a steady 30 fps.

## Building

Needs the Android SDK with NDK 29.0.14206865 and CMake 3.31.6, a JDK (17 or 21), git, curl,
make, Python 3 and the Liberation fonts. `android/prepare.sh`, which Gradle runs first, fetches
psprism (patched), SDL2 and FFmpeg (built with the NDK) and the fonts into `build/android`.

```sh
cd android
ANDROID_HOME=/path/to/android-sdk ./gradlew assembleRelease
# app/build/outputs/apk/release/app-release.apk
```

The version comes from git: the release's tag (`v1.0` is 1.0) or the commit, and the commit's time
(so each build is newer than the ones before, as Android needs to install it as an update).

## Releases

`.github/workflows/android.yml` builds the APK on GitHub. Publishing a release (Releases, Draft a
new release, with a new tag such as `v1.0`) builds it and attaches `MHFU-Recomp-1.0.apk` to the
release when it's done; running the workflow by hand (Actions, Android, Run workflow) builds a
test APK, kept with the run.

Every APK has to be signed with the same key: Android installs an update only over an app signed
with the same key, and uninstalling first deletes the player's saves. The workflow has it from
two repository secrets (Settings, Secrets and variables, Actions): `ANDROID_KEYSTORE`, the key's
PKCS12 file in base64, and `ANDROID_KEYSTORE_PASSWORD`; the key's alias is `mhfu`. Keep the file
and its password somewhere safe besides: GitHub never shows a secret again, and without the key
the app can't be updated. A local build uses it when given `MHFU_KEYSTORE` (the file) and
`MHFU_KEYSTORE_PASSWORD`; without them, it's signed with the debug key, for testing.

## To do

1. More testing on phones: hunts, other GPUs, controllers.
2. Touch controls.
3. Later: if phones are too slow interpreting, an optional
   one-time build of the game's code on the phone.
