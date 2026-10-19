# Developer notes: the static recompilation

How the port is built, what was changed in psprism and why, and how to test it. For what the
project is and how to play, see the [README](../README.md).

## Base: psprism

[psprism](https://github.com/anthonyhfm/psprism) (GPL-3.0) is an existing PSP static recompiler.
It runs Daxter natively on macOS and Windows and comes with `refract`, a native replacement for
the PSP's system libraries: threads, file I/O (including the raw `disc0:/sce_lbn` reads MHFU
uses), graphics, SAS audio, Atrac3+, MPEG cutscenes, save dialogs. Code it didn't translate ahead
of time runs through a built-in interpreter, which also handles VFPU instructions.

`psprism-mhfu.patch` applies to commit `8764f39bb6ee0d10a9c07d8ca2934ce38a5ecb83` and adds:

| Change | Why |
| --- | --- |
| `vocp` VFPU instruction | MHFU uses it 50 times; psprism rejected it. |
| Keep a pending branch when a translated function declines an address | Generated functions cleared the pending jump before checking whether they handle the address, so a `jr $ra` executed by the interpreter was lost when its delay slot fell back to the interpreter too. MHFU's static constructors live in `.data` (interpreted), so boot ran off their end into data. |
| Virtual timers (`sceKernelCreateVTimer`, `StartVTimer`, `SetVTimerHandlerWide`) | MHFU's thread scheduler and the Capcom sound engine tick on 60 Hz VTimers. Handlers are called as `handler(uid, &schedule, &actual, common)`. |
| **Font library** (`sceFont*`, `refract/src/font/`) | The game draws its text with the PSP's system fonts (`jpn0.pgf`, even in the EU release). Glyphs come from the Liberation fonts via stb_truetype, at the firmware fonts' sizes (10.125 pt and 7 pt at 128 dpi); font info reports the hardware-measured metrics from pspautotests, because MHFU sizes its character cells from them. |
| `vcmovt`/`vcmovf` the right way round | psprism moved on the opposite condition. MHFU normalizes vectors with `vcmp EZ` + `vrsq` + `vcmovt` (use 0 when the length is 0), so zero vectors became infinite and the hunter's world matrix NaN: the 3D model in character creation was missing. Bit 19 clear is `vcmovt` (as in PPSSPP). |
| **SAS voices like the PSP** (`sas_state.hpp`) | A sound's sample is read again from memory at key-on (a new sound bank loaded at the same address played the old sound); a sustain that fades to 0 moves to release and a release that reaches 0 ends the voice; key-off always enters release when there's an envelope; VAG data ends only at flag 7 and loops at flag 3 (PPSSPP's `VagDecoder`); `SetVoice` accepts what it can't play with success, and a voice with nothing to play ends, so the sound engine sees it free again. |
| Audio buffering (`audio_engine`, `sdl_gles.cpp`) | The PSP lets a blocking audio write return once the previous block has started; refract's guest threads take turns on one emulated CPU, so the sound thread was often late and the device ran dry (choppy sound). The SDL host now lets about 46 ms queue ahead (`set_blocking_backlog`); the default, and the tests, keep the PSP behaviour. `REFRACT_AUDIO_WAV=FILE` records the output, `REFRACT_AUDIO_STATS=1` prints underruns at exit. |
| `sceAtracGetNextDecodePosition` | Called before every Atrac decode by the sound engine; returns the next sample, or "all data decoded" at the end. |
| Videos: `sceMpegAvcDecode` with a frame width of 0 | Means the width given to `sceMpegCreate` (MHFU passes 0, then 512); refract rejected it, so every frame failed. `build_linux.sh` builds with FFmpeg when its development files are installed. |
| Save folder for the save name `<>` | `<>` means no save name, so MHFU saves to `ULES01213` like on a PSP (and in PPSSPP), not `ULES01213<>`; an existing `ULES01213<>` folder is moved. |
| **Launcher and resolution** (`launcher_screen.hpp`, `sdl_gles.cpp`, `refract/frontend.hpp`) | `run_launcher` shows a launcher in the game window before the game starts: a menu (play, settings, controls, quit) over the game's art, a settings page (the choices under their sections' headings, each option shown and the chosen one underlined, or `< value >` when they don't fit; what the chosen setting does underneath; scrolled with the mouse wheel or by moving) and a controls page (the PSP's face buttons drawn in their colours). It's drawn in software at the window's own resolution (a 960x544 grid scaled to fit, the art covering the rest), with smooth (bicubic, widened when shrinking) image scaling, anti-aliased shapes and outlined text, and redrawn only when something changes; headings use Cinzel (`font_engine`'s `UiFace::display`, Liberation Serif Bold without it); `load_image` reads images with stb_image (public domain). `set_display_settings` sets the window size or fullscreen and the drawing scale. Render targets are drawn at that scale (TextureResource sizes stay in PSP pixels, with a `scale`; only allocation, viewport and scissor use the backend's pixels), capped by the GPU's largest texture size. `REFRACT_WINDOW_SIZE=WxH` creates the window at a size (SDL's offscreen driver can't resize it). |
| Translated code that declines to run (`patch.hpp`) | `add_interpreted_range` makes the functions in a range decline (the dispatcher falls back to the interpreter, which runs what is in memory), for code patched at run time: MHFU's 60 fps code (`src/cheats.hpp`). The first 9 KB of user memory are no longer handed out as heap, as on a PSP: cheats keep their code there. |
| **Decoded textures kept** (`runtime.cpp`) | Every texture was decoded again for every display list of every frame; with people or monsters on screen that took most of the frame, and 60 fps fell to 30. Decoded textures are now kept, found by a hash of their data, palette and layout. That hash, and the check of whether the CPU changed the shown framebuffer, read 8 bytes at a time; the framebuffer is converted only when it changed. |
| Faster dispatch for interpreted code (`dispatch.inc`, `functions.inc`, `patch.hpp`) | Each instruction run by the interpreter went through the patch lookup twice, a symbol lookup and the last translated function before being interpreted. Addresses past the translated program now go straight to the interpreter, and patch lookups are skipped when nothing is registered (one bounds check for interpreted ranges). |
| **Frame interpolation** (`sdl_gles.cpp`, `frame_pacer.hpp`) | Like Zelda 64: Recompiled's, at 60, 120 or 144 pictures a second (`set_frame_interpolation`). The renderer keeps what each shown frame drew into its framebuffer. When a frame is shown, its 3D draws are matched to the previous frame's: the same model (vertex and index data at the same PSP address, which the GE code now records with each draw) drawn the same way with the same texture. Of a model drawn several times (ground tiles, props), it takes the copy that, moved along with the camera, lands where this one is. The GE code also records each draw's transform from the model to camera space; MHFU folds the camera into it, so between frames it changes the same way for everything that stands still, and the change most models drawn once in both frames agree on is the camera's motion. (Matching by texture and vertex count in draw order paired neighbouring ground tiles outdoors, which slid into each other; taking the copy nearest on screen still picked a different copy than the camera's motion for about 3% of repeated models outdoors.) For each picture until the next frame, the frame is drawn again into a separate target with matched vertices' clip-space positions blended from the previous frame's, starting from the frame's own picture. Pictures come at the rate chosen, at most one per refresh of the screen: each is due one interval after the last one was due, so vsync rounding each to a refresh keeps the rate (120 a second on a 144 Hz screen). Waiting most of a refresh after the last picture, as before, missed every other refresh when the swap waits for vsync and drawing takes more than a tenth of a refresh: 30 pictures a second on a 60 Hz screen (simulated). 3D draws without a match are moved with the camera only (the GE code records each draw's camera-to-output matrix too), so they stay in line with the rest. 2D draws (menus, text) are drawn as they are, and so are draws whose vertices on screen moved more than a quarter of the screen. Vertices near or behind the camera are left out of that check: their screen positions swing wildly as the camera moves a little, and leaving them out took matching outdoors from 93% to 99.7% of 3D draws (the big ground pieces around the hunter had them, and weren't blended). A frame isn't blended if less than half its 3D draws matched, or less than half of what is in view was in view before (a cut). `FramePacer` evens out the few milliseconds frames come early or late (an even timeline at the game's average pace, drifting towards the arrival times, starting over at a hitch); without it, motion judders about six times as much (simulated: `refract_ge_tests`). The picture is one game frame behind. In the opening and the village, over 99% of 3D draws matched. `REFRACT_SMOOTH_STATS=1` prints how much is blended. The display also shows the framebuffer of the last flip drawn, not one flipped after the drawing was taken. |
| Audio stays off once shut down (`linux_headless.cpp`, `sdl_gles.cpp`) | Every sound the game sends starts the audio output if it isn't running. At exit, the game's sound thread kept sending: the headless host started a new worker over the one being joined (abort, "terminate called without an active exception"), and the SDL host could open the device again. |
| FPS counter, `REFRACT_FPS=1` | `set_fps_counter` shows how often the game shows a new frame (each `sceDisplaySetFrameBuf`) in a corner of the window; `REFRACT_FPS=1` prints it. (The earlier "86 fps" counted finished display lists; the game shows 29.97 frames a second.) |
| **On-screen keyboard** (`sdl_gles.cpp`, text-entry `sceUtilityOsk`) | Drawn over the game: type on the keyboard, or pick keys with the D-pad and confirm button. The game gets no input while it's open, and buttons still held when it closes reach the game only after release. The field's limit falls back to the buffer size like on the PSP. UI text uses Liberation Sans via the font engine. |
| **Ad hoc network** (`refract/src/net/`, `stubs/net/`, `sceUtilityNetconf*`, `third_party/aemu_postoffice/`) | Multiplayer. refract had placeholders for `sceNet`, `sceNetAdhoc` and `sceNetAdhocctl` (no peers, nothing received). They now go through an ad hoc server the way PPSSPP's "relay" mode and the aemu plugin for PSPs do, with aemu_postoffice's clients (GPL-3.0, vendored unchanged): `adhoc_control.cpp` keeps a session with the server's adhocctl service (TCP 27312) on a thread of its own, logging in with the player's address, nickname and the game's product code, sending scans, joins and leaves, and taking in the groups and the players in them (what `sceNetAdhocctlGetScanInfo`, `GetPeerList`, `GetState` and the handlers' events report); it logs in again if it loses the server. PDP and PTP sockets each get a session with the server's relay (TCP 27313), which forwards data by address and port, so nobody needs to open ports; relay ports are the game's plus 10000, as in PPSSPP and aemu. A PDP broadcast goes to each player in the group. Blocking calls wait 1 ms at a time with the game's other threads running. `sceNetAdhocPtpConnect` connects on a thread of its own and tries again for 30 s, since the relay refuses until the other side listens. Sends bigger than the relay's block (PDP 10 KB, PTP 50 KB) go in pieces. The netconf dialog joins the group asked for and finishes when the server answers (30 s at most, 10 s if the server isn't reached). `sceNetAdhocGetPtpStat` is added; `sceWlanGetSwitchState` reports the switch on only when a server is set. The address is a random one the launcher makes once (locally administered, as PPSSPP makes them). `refract::adhoc::server_answers` tells whether a server takes connections on its adhocctl port, for the launcher to show. `REFRACT_ADHOC_LOG=1` prints the game's calls and the client's own log. |
| `sceKernelSysClock2USec`, `sceKernelSysClock2USecWide` | Split a time in microseconds into whole seconds and the microseconds left over, as on the PSP (PPSSPP); refract gave the time's two 32-bit halves. MHFU's network session adds them up every frame for its clock, which so ran thousands of seconds a frame: at the start of a quest together, its 60 seconds for the players to connect to each other ran out at once, and the other player "broke off from the group" (`refract_ge_tests` checks both). |
| `sceKernelDcacheInvalidateRange`, `sceKernelIcacheInvalidateAll`, `sceOpenPSIDGetOpenPSID` | Called at boot; no-ops / a fixed ID. |
| **SDL2 host** (`refract/src/host/sdl_gles.cpp`) | Window, renderer, audio, keyboard and game controllers for Linux and Android, drawing with Vulkan or OpenGL ES 3.0 (below). A port of the Direct3D 11 backend: one render target per PSP framebuffer address, the GE's blending, depth/stencil, alpha and color tests, texture functions and render-to-texture. Supports offscreen runs with screenshots and scripted input for testing. Every display list's drawing reaches the render targets in order: the backend this was ported from replaced what the render thread hadn't drawn yet, which loses whole lists (a frame is drawn by several, and targets keep their contents), so rooms lost walls and menus flickered whenever drawing fell behind, as it did at 720p/1080p. If drawing is more than two frames behind, the game waits for it (at most 50 ms a frame). |
| **Vulkan and OpenGL ES** (`gpu.hpp`, `gpu_vulkan.cpp`, `gpu_gl.cpp`, `vulkan_shaders/`) | The renderer decides what is drawn where (render targets, widescreen, frame interpolation); a backend keeps its images and render targets and draws into them. Vulkan is the default (the launcher's Graphics setting, `config.txt`'s `graphics`), OpenGL ES 3.0 the other: where Vulkan can't start (no driver, no device that draws on the screen, only a software one, or a build without the Vulkan headers), the game draws with OpenGL ES, and a change of setting opens a new window. The Vulkan backend is Vulkan 1.0 with a swapchain, loaded through SDL, with the same shaders compiled to SPIR-V (`vulkan_shaders/compile.sh`, committed so the build needs no shader compiler). Each frame records the texture uploads and the drawing in two command buffers submitted together; a texture changed after the frame drew with it gets a new image, so earlier draws keep the old one. Render targets are tracked in the layout their last use left them, each run of draws into one is a render pass that keeps its contents, and pipelines are made as draws need them from a cache kept in the data directory (`vulkan-pipelines.bin`), so a state seen once doesn't stall again on the next run. Image memory comes from 32 MB blocks. On Android the swapchain keeps the screen's own orientation and the picture is drawn turned (pre-rotation), sparing the compositor a turn each frame: Android gives the window's size as the window is (landscape on a phone whose screen is portrait), so the swapchain is that the other way round (drawing the swapchain landscape squeezed the picture into a band). Should a swapchain's size not match the window (SDL's size), the picture is drawn scaled to it, which Android's scaling onto the window undoes. `REFRACT_VULKAN_TEST_ROTATION=90` (or 180, 270) and `REFRACT_VULKAN_TEST_IMAGE=WxH` do this offscreen, for tests; drawn turned or scaled, the launcher and the game come out the same. Offscreen (SDL's offscreen driver, for tests) it draws into an image of its own. `REFRACT_GRAPHICS=vulkan` or `opengl` overrides the setting, `REFRACT_VULKAN_VALIDATION=1` turns on the validation layer, `REFRACT_VULKAN_ALLOW_CPU=1` allows a software device. On Mesa's software renderers (llvmpipe and lavapipe) the two draw the same pictures through the opening, the menus and character select, and Vulkan draws the village as it should; the validation layer reports nothing through the opening, the menus, the launcher and the FPS counter, nor in a test of target copies and textures changed mid-frame. |
| Headless Linux host (`refract/src/host/linux_headless.cpp`) | Runs without a window: virtual audio device draining the mixer in real time (optionally to a WAV file), scripted controller input, frame and primitive counters. |
| 8-argument guest callbacks | For the VTimer handler. |
| Right stick (`host.hpp`, both hosts) | `ControllerState` carries a controller's right stick (round dead zone taken out), for game code patched to use it. In input scripts, bits 24 to 27 push it up, right, down and left. |
| Hooks before interpreted ranges (`patch.hpp`) | `find_patch` looked at interpreted ranges first, so a hook on a function the 60 fps code changes never ran. Address hooks now come first; a hook's call to the original bypasses it and lands in the interpreter (`runtime_tests`). |
| **Faster hook lookup** (`patch.hpp`, `dispatch.inc`) | Every translated block and function call checks whether a hook is registered there, and that went through the whole list of hooks. With the 60 fps code's 26 hooks it took 43% of the game thread's time on a hunt, and 60 fps fell to 30 (the game slowing down with it) with monsters or villagers about. Hooks at fixed addresses now also go in a bitmap that is checked first, and the dispatcher looks up function names only when a hook is found by name. On a hunt at 60 fps the lookup takes 4.5% of the game thread, and the thread's time per frame went from about 22 to 15 ms on the test machine (`runtime_tests` checks the bitmap). |
| **Thread priorities** (`runtime.cpp`, `sceKernelChangeThreadPriority`, `sceKernelSuspendDispatchThread`) | Guest threads took turns first come, first served: at every system call and every slice of 4096 blocks, the running thread let any waiting thread run. The PSP runs the highest-priority thread that is ready; a thread keeps the CPU until it blocks or a higher-priority one becomes ready (then it runs again before the others of its priority), and keeps it while it has suspended dispatching. MHFU relies on that. `user_main` runs at 48, which it sets as it starts (the call did nothing in refract); so does the `DATA.BIN` loader thread, `fakeRofsLoader`. Loading a quest, `user_main` reads the header of each file it asks for from inside the buffer the loader fills. When the loader ran in between, it wrote the previous file over the next header. The game then asked for a size of `0x80000006` (the sound engine's error for a sound bank without its header), and the loader retried that read forever: "Now Loading" never ended, in 2 of 7 test runs with two games at once. Now the CPU goes to the waiting thread with the highest priority, `sceKernelChangeThreadPriority` changes it, `sceKernelGetThreadCurrentPriority` reads it, and `sceKernelSuspendDispatchThread` keeps the CPU (`refract_ge_tests` checks the order threads run in). A thread preempted by a higher-priority one is queued before it lets go: the sound thread (priority 17) applies a voice's changes one flag bit at a time, and loops forever if one it has passed is set again, which a lower-priority thread did when it slipped in after an interrupt handler. Priorities also ended the sound engine's "no free voice work" errors (`[PSP](ERR)` lines in Japanese: all 32 of its sound slots taken, so it skipped sounds), probably voice changes lost to the same kind of interleaving: test runs logged about 600 of them by the start of a hunt, and now none, up to 4,000 frames into a hunt at 60 fps and 5,600 at 30. |

The game is built from the player's own disc. `play.sh` first builds and runs `find_game`
(`src/find_game.cpp`, a few seconds to build), which finds the ISO: the one given (a file, or a
folder searched three folders deep), the one remembered in `config.txt`, one where emulators keep
games (PPSSPP's recent games and last folder, from its `ppsspp.ini`, native or Flatpak; `roms`,
`games`, `emulation`, `retrodeck`, `downloads` and `iso` folders, any case, in the home folder
and on drives under `/run/media` and `/media`), or the folder the player chooses (`zenity`, `kdialog`,
or the terminal), until it's found or the player cancels. Disc images are known by what's in
them: `PARAM.SFO`'s `DISC_ID` and an FNV-1a hash of the part of `PSP_GAME/SYSDIR/BOOT.BIN` (the
unencrypted program; the dumped EBOOT is the same with 340 bytes after it, left from the
decryption) loaded into memory, which must be `expected_image_hash` (`src/disc.hpp`). What isn't
the game is listed when it isn't found: other games, other versions or changed copies, and
compressed images (`.cso`, `.zso`, `.chd`, which can't be read yet). With `--program FILE` it
writes `BOOT.BIN` there, and `build_linux.sh` translates that, with `mhfu.map` for where the
functions are. The game is then started with the ISO found.

The launcher (`src/launcher.cpp`) replaces psprism's generated entry point. It finds the
player's ISO the same way (`src/disc.cpp`, for running `bin/mhfu` without `play.sh`), checks it,
builds the guest memory image from `BOOT.BIN`, copies the disc's files to the data directory
once, shows the launcher in the window as last set (settings under Display: frame rate,
resolution, window or fullscreen, frame interpolation, FPS counter; Gameplay: right-stick camera
and a row for each gameplay feature; Online: multiplayer; kept in `config.txt` with the ISO
path), and starts the game. The launcher shows the project's logo (`art/logo.png`, copied to
`Resources/art`) over the game's own art from the player's disc, the PSP menu's background
(`PSP_GAME/PIC1.PNG`). Without the logo it falls back to the game's, cut out of the PSP menu's
overlay (`PIC0.PNG`: the lettering, without the smoke round it, which grows ragged scaled up),
with RECOMPILED under it. The bottom left shows
the disc (release and ISO file name, cut short with an ellipsis before the button hints at the
bottom right) and the build (`git describe`).

Multiplayer: the launcher's Multiplayer row picks the server (`multiplayer_server=` in
`config.txt`, empty for off; another host can be set there) and shows whether each answers
(checked on threads of their own from the launcher's start, waiting at most 2.5 s for them), and `player_address=` and
`nickname=` (from `$USER` at first) are kept with it; it hands them to `refract::adhoc::configure`
before the game starts. The game's own flow, as far as it was followed: at the Gathering Hall's
door in Pokke Village (area `0x8b`; the door's zone at about (9126, -700, 9130), radius 200), □
(`0x8000`) enters the hall (area `0x55`) alone and ○ (`0x2000`) in ad hoc mode (bit 2 of the flag
`FUN_09a789d0` reads). In ad hoc mode it calls `sceNetInit`, `sceNetAdhocInit` and
`sceNetAdhocctlInit` with product `ULES01213`, scans, and the hall's network task
(`FUN_09a70730`, state 5) shows the hall numbers: up and down choose, ✕ joins, ○ cancels. Joining
connects through the netconf dialog (action 2) to group `MHP2Q0` plus the two-digit number
(`MHP2Q000` for the first). Once connected it opens PDP port 10000 and broadcasts 24 to 810-byte
packets, and reads the peer list every frame. The hall uses no PTP. Departing on a quest, the
game's network session (`0x09a1d5d0`, updated by `FUN_088a9660`) connects the party: each player
opens a PTP stream to each player before it in the party, to port 20000 plus its own number, and
listens on 20000 plus the number of each player after it (`FUN_088a748c`), with 60 seconds to
do it (its clock is `sceKernelGetSystemTimeWide`, through `sceKernelSysClock2USecWide`); then
the quest's data goes over PDP port 20001.

Gameplay features (`src/features.hpp`, `src/features/`) are changes to how the game plays that
players turn on or off: one file each, describing itself with `MHFU_FEATURE` and registering its
hooks (`src/hook.hpp`) when it's turned on, so one that is off changes nothing. The launcher adds a
row for each (Off by default) and keeps the choice as `feature.<key>` in `config.txt`. How to add
one: [`docs/features.md`](../docs/features.md). Hold to gather (`features/hold_to_gather.cpp`)
hooks `hunter_read_pad`: while the hunter stands at a gathering spot with gathers left (on a hunt;
`quest_gather_points`) holding its button (Circle, or Square with a pickaxe or bug net chosen), it
counts the button as pressed again, and the game starts the next gather by its own rules.

The right-stick camera (`src/camera.cpp`) hooks the hunting areas' camera update,
`FUN_08886e50`. The camera manager (pointer at `0x089cbbe4`, the field camera shown while its byte
at `+0xa8d` is 0) copies the pad to `+0xa70` (held) and `+0xa72` (pressed) each frame, and the
camera's eye (`+0x0`) and the point it looks at (`+0x10`) to its own `+0x20` and `+0x30`. The
camera turns its yaw (16-bit angle at `+0x120`, eased into the shown yaw at `+0x122`) by a fixed
step a frame while left or right is held (`0x47e`, halved by the 60 fps code, flipped by the
game's reverse-camera option), skipping its swing back behind the hunter; up and down step
between five heights (the one in use at `+0x110`: `+4` eye height above the hunter, `+8`
distance, `+0x10` height of the point looked at, `+0x18` least eye height above the floor). It
eases the eye's height an eighth of the way a frame, keeps it off the floor, raises it by up to 300
over high ground (`FUN_0888d1bc`), and moves the point looked at to keep the hunter's head in view
(`FUN_0888f6a0`); a tilt made by changing the heights (the first version) floated, stopped at
about 10° up near the ground, and jumped when those kicked in.

Now the stick (or the d-pad, as the stick pushed all the way) turns the camera through the game's
own d-pad path: the hook holds left or right for the camera, and sets the yaw shown and the yaw
beforehand so that the game's step lands on the yaw shown plus the stick's share of the step (the
step is learned from the frame before). Up and down are taken out of what the camera sees, so its
height stays, and tilt the camera instead (100°/s at full tilt; the eye between 35° below and 70°
above the point looked at, the tilt held to that range so it never builds up past it). The camera
only takes the pad (into `+0x124` held and `+0x126` pressed, `FUN_0888d004`) while `FUN_0888d600`
says it's the camera's, not while a message or the menu is up (the d-pad then works the menu) or
in some of the hunter's actions, and camera 5 isn't running; otherwise it takes L alone, or
nothing. The hook asks the same before turning or tilting, so neither the d-pad nor the stick
moves the camera then. L turns the camera back behind the hunter when it is let go (the hunter's
`+0x488`) within 10 frames of being pressed (`+0x61c`, counted by `hunter_read_pad`;
`FUN_0888e494` and game_task's `FUN_09a6ed48`): a tap. Held, L only shows the item bar. The hook
eases the tilt back when the game turns the camera back for a tap (the first version did on
pressing L, which tilted the camera every time the item bar came up). After the
update the hook places the eye itself: at the height's distance from the point looked at (the
hunter's position at the height's look height, eased like the game's), along the yaw shown, at the
height's angle plus the tilt. The game's camera sweep (`0x09c3aaa8` in `game_sub.ovl`, radius 28,
the game's flags) brings it in front of walls at once; something thin in between (a tree) is looked
past when the sweep back from the eye stops less than 120 behind where it stopped (behind a wall it
doesn't stop at all: surfaces face out) and the eye's place is open from above. The sweep doesn't
see floors, so the floor height under the eye (`0x09c3da00`, from 150 higher when it's below the
hunter's feet, as the game's camera does) brings it in until it is 40 above the floor (the game
keeps its own 80 above): looking up near the ground brings the camera in low behind the hunter. It eases out
again. The sweeps and floor queries run in the interpreter (overlay code), about 0.2 ms each, two or
three a frame. While the game's camera follows something else (blending from another camera, the
hunter carted or carving: the height it wants doesn't match the one in use), the game's camera
shows, faded over a quarter of a second. The house and the village use another camera
(`FUN_08888b84`) that the d-pad doesn't turn. Registering the hook costs every translated function
call a hook lookup: about 0.6% of a core in the village at 30 fps (16.1% to 16.7%).

60 fps uses the community "60 FPS" code for ULES01213 (`src/cheats.hpp`, with a small CWCheat
engine). It changes six functions of the program, adds code at 0x08802000, and patches
overlays. The six functions run in the interpreter while it's on, the code is written before the
game starts, and it's applied again every 50 ms, like a cheat engine, for overlays loaded later.
(It was taken out for a while because of missing walls and flickering at 60 fps; those came from
the renderer dropping display lists, fixed since.)

The 60 fps code halves the game's frame steps, but some counters still step as at 30 fps, so what
they time happens twice as fast. `src/fps60.cpp` hooks the functions that step them and adjusts
around the game's own code, so that two 60 fps frames move them as one 30 fps frame did: a
character's time in its action, the village's animations, timers and its task's waits, the play
time, typing text, held directions in menus (the pad's repeat), the blend from one motion into
the next (given twice as many frames), the hunter's timers on a hunt (hunger, items, ailments,
health recovery), the steps of hunters' and monsters' actions (`action_timer`), a monster's
timers (ailments, traps, rage and others), the moves monsters make by a speed a frame (half a
step each frame), the speeds some of the hunter's actions give their motions themselves
(gathering, mining and catching bugs among them: halved in game_task's code as the 60 fps code
does the others), how long L is held for a tap that turns the camera back (20 frames instead of 10,
in game_task's code), and cutscene cameras (each step of a camera script, or the key times of a
camera track, made twice as many frames, so the camera still moves every frame). These were
found by dumping memory at the same moments at 30 and 60 fps and listing what changes twice as
fast, and by dumping the hunter or a monster every frame and comparing how long each of its values
is held. Hooks in overlays check that the overlay is loaded and the code is as expected.

## Status

The game boots and runs through its opening on Linux with the SDL host: language selection →
notices → Capcom logo → Dolby Pro Logic II → title screen → game menu → character creation,
with text, the 3D hunter, name entry, music and sound effects.

| Step | Result |
| --- | --- |
| Translate the EBOOT | All 265,354 instruction words; all 272 imports recognized. |
| Compile | The generated C++ (3,150 functions) plus refract builds with g++ 13 on Linux. |
| Boot | Static constructors, `user_main`, the `DATA.BIN` loader threads, a module load, VBlank interrupts, both VTimers, the Capcom Sound Engine, then the menus above. |
| Rendering | SDL2 host with Vulkan and OpenGL ES 3.0, both tested with Mesa's software renderers. |
| PSP system functions | 228 of 272 implemented, including every one the game has called so far. Missing: HTTP/SSL/internet (for DLC downloads), `sceKernelGetThreadExitStatus`. The ad hoc functions (multiplayer) are implemented. |
| Overlays | Not translated yet; the interpreter runs them. `tools/extract_overlays.py` pulls all 298 out of `DATA.BIN`. |

Known issues:
- Save and load dialogs are accepted automatically; there is no on-screen UI for them yet.
- At 60 fps a few things still step as at 30: some weapons step the hunter a fixed distance a frame
  while drawing; a few short steps of the hunter's actions end on something other than their
  count (drawing the weapon starts in 0.05 s instead of 0.1 s); in cutscenes, a countdown in some
  characters. Monsters' moves have been checked on game_task's code (`monster_fall`,
  `monster_move_facing` and the actions that use them), not yet in a fight with a large monster.
  The village overlay has motion speeds like the ones halved on hunts that the 60 fps code leaves
  (`FUN_09aaaf88` at 0x09aab004 and 0x09aab018, `FUN_09ab22f8` at 0x09ab2430), not yet checked.
- Hold to gather works on hunts; the farm in the village isn't covered yet.
- Multiplayer: tested with two games in the same hall through a local aemu_postoffice server, and
  with a player on PPSSPP through a public server (MGN): the hall, setting out on a quest together
  (where the clock fix above was found) and hunting its monsters. Not tested yet: finishing a
  quest together, more than two players. PTP is also tested through the stubs (two processes:
  listen, accept, connect, 120 KB each way, the other side closing). The public servers couldn't be
  reached from the development machine, so they're untested here; they run the same server.
  Windows isn't built yet (`sock_impl_windows.c` and the CMake setup are in place, untested).

## Next

1. On-screen **save/load dialogs**, and **reverb** (the game turns it on; the voices it has played so
   far send nothing to it).
2. Play further and fix what breaks.
3. **Translate the overlays** ahead of time instead of interpreting them.
4. **Windows** (the SDL host plus a launcher port) and **Android** (SDL2 app with touch controls).

## Building and running (Linux)

See [Building](../README.md#building) in the main README. Then, for testing:

```sh
recomp/run.sh ~/mhfu-build /path/to/game.iso    # same as running ~/mhfu-build/bin/mhfu
```

Without a display: `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy FRAMES=3600 SHOTS=/tmp/shots
INPUT="300:4000:6,1500:8:6" recomp/run.sh ...` runs one minute of game time, presses cross at
frame 300 and start at 1500, and saves a screenshot every 60 frames. A `FRAME:'text` step types
text into the on-screen keyboard; this reaches character creation and names the hunter:
`300:4000:6,900:4000:6,1500:8:6,1800:4000:6,2400:8:6,2700:4000:6,3060:4000:6,3200:'Kevin,3400:8:6`.
`MHFU_DATA_DIR=DIR` keeps
the extracted data and saves somewhere other than `~/.local/share/mhfu-recomp`.
`REFRACT_VERBOSE=1` logs system calls, threads and graphics lists.
In `REFRACT_INPUT_SCRIPT`, mask bits 28 to 31 push the analog stick up, right, down and left
(`21000:40000000:400` runs towards the camera for 400 frames), bits 24 to 27 the right stick.
`REFRACT_SAS_STATS=1` reports the sound voices playing every 5 s.
`REFRACT_RENDER_SCALE=N` draws at N times the PSP's resolution whatever the settings say (1 is
quickest without a GPU). `REFRACT_SCREENSHOT_EVERY=1 REFRACT_SCREENSHOT_FROM=A
REFRACT_SCREENSHOT_UNTIL=B` saves every picture in a range; with frame interpolation, screenshots
(`smooth_N`) count every picture shown.

`HOST=headless recomp/build_linux.sh ...` builds a variant with no window or GPU at all;
`REFRACT_HEADLESS_WAV=FILE` records its audio. `TRACE=1 recomp/build_linux.sh ...` adds a jump
trace (`add_trace.py`) that prints the last 64 jumps when a guest thread stops, which is how the
boot problems above were found.

Multiplayer without the internet: build aemu_postoffice's server
([Kethen/aemu_postoffice](https://github.com/Kethen/aemu_postoffice)), set its `ip_addr` to
`127.0.0.1`, start it, and run two games with `multiplayer_server=127.0.0.1` in their
`config.txt`, each with its own `MHFU_DATA_DIR` (and so its own `player_address=` and save).

## Files

- `psprism-mhfu.patch`: the changes to psprism listed above.
- `src/launcher.cpp`: the launcher and entry point.
- `fonts/`: Cinzel Bold (SIL Open Font License, `Cinzel-OFL.txt`), the launcher's headings.
- `art/logo.png`: the launcher's logo, made for this project.
- `src/cheats.hpp`: the CWCheat engine and the 60 fps code.
- `src/fps60.cpp`: what makes 60 fps run at normal speed where the 60 fps code doesn't.
- `src/camera.cpp`: the right-stick camera.
- `src/features.hpp`, `src/features/`: gameplay features, each with its own setting
  ([`docs/features.md`](../docs/features.md)); `src/hook.hpp`: hooking the game's functions.
- `src/game.hpp`: reads and changes the game's memory by the names in `re/`
  (`monster.get(Monster::rage_timer)`); the build writes the layout it uses, `game_layout.hpp`,
  from `re/` with `tools/gen_layout.py`.
- `src/disc.hpp`, `src/disc.cpp`: reading, checking and finding the player's ISO; `src/find_game.cpp`:
  finds it and takes the game's program out of it for the build (run by `play.sh`).
- `mhfu.map`: where the game program's functions are, for psprism, so each becomes its own C++
  function (named after it, `func_08865934_character_update.cpp`). `make_code_map.py` makes it
  from `decompiled/eboot/functions.tsv`; run it again when Ghidra's function list changes.
- `build_linux.sh`, `run.sh`: build and run.
- `add_trace.py`: optional jump trace for the generated dispatcher.
