# iPhone / iPad build

This builds the recompiled game for iOS / iPadOS 16 or newer (arm64): the same ps2-web-recomp runtime and the same
code generated from your disc as the macOS `ps2run`, plus a small UIKit front end in `ios_main.mm`.
It needs no SDL. The disc is not packed into the app: the app reads it from its own Documents folder.

The app contains code translated from the game, so keep the `.ipa` on your own machine and your own devices.
Don't share it or upload it anywhere.

## What you need

- Xcode with the iOS SDK (tested with Xcode 27.2), CMake 3.20 or newer, Ninja (`brew install cmake ninja`)
- the translator and the game's boot program, as for the macOS build: `ps2-web-recomp/build/tools/eerecomp`,
  `games/p3fes/disc/SLUS_216.21` and `games/p3fes/entries.txt`, plus the vector unit programs in `generated/p3fes/vu1_*.cpp`.
  If the macOS build (`build/native-p3fes`) runs, you already have them.
- wgpu-native's iOS build, for GPU drawing (Metal). Without it the CPU renderer draws, which is far too slow for the
  3D scenes. Use the same version as `ps2-web-recomp/third_party/wgpu-native` (`wgpu-native-meta/wgpu-native-git-tag`,
  v29.0.1.1 at the time of writing), from https://github.com/gfx-rs/wgpu-native/releases:
  unpack `wgpu-ios-aarch64-release.zip` into `ps2-web-recomp/build/wgpu-ios/device` and
  `wgpu-ios-aarch64-simulator-release.zip` into `ps2-web-recomp/build/wgpu-ios/sim`. Each folder then holds `include/` and `lib/`.

## Build

```sh
portios/build.sh          # device: ps2-web-recomp/build/ios-p3fes/P3FES.ipa (unsigned)
portios/build.sh sim      # Simulator: ps2-web-recomp/build/ios-sim-p3fes/P3FES.app
```

The iOS app does not use the 3DS tuning. `generated/p3fes` is translated for the 3DS (hot copies of busy functions, the
lossy skips, 32-bit narrowing of 64-bit guest maths, loop locals, unchecked stack accesses), so `build.sh` translates the
game again into `generated/p3fes-ios` with none of that (`EERECOMP_WIDE=1 EERECOMP_NOLOOPS=1 EERECOMP_SPCHECK=1`, no
`--natives`, no `--seen`) and compiles it with `PS2_NO_HOT`. It takes about a second and runs again when the translator,
the boot program, `entries.txt` or `build.sh` is newer than that folder. The vector unit programs are the desktop ones
from `generated/p3fes` (the 3DS has its own). The runtime's 3DS changes are all inside `#ifdef __3DS__` or `PS2_PICA`.

The script uses `build/wgpu-ios/device` or `build/wgpu-ios/sim` when they are there and says
"drawing with the GPU" while configuring. To use wgpu from another folder, set `P3FES_WGPU_DIR` to it.

The iOS build compiles a copy of `runtime/src/gs/gpu/gpu.cpp` made in the build folder. The copy has one change: it asks the GPU for
all the limits it supports. wgpu gives some iOS GPUs (the Simulator's at least) reduced default limits otherwise, and
the GS shaders need more than those. The copy also accepts a CAMetalLayer, so the app presents frames straight to the
screen from the render thread. The ps2-web-recomp tree itself is not changed. If gpu.cpp changes so that the copy
can't be made, CMake stops with a message.

The script finds ps2-web-recomp next to this repository; set `PS2_ROOT` if it is somewhere else. A full build takes about
1.5 minutes on an M-series Mac. The build folders are new ones (`build/ios-p3fes`, `build/ios-sim-p3fes`) and do not
touch the native or 3DS builds. After the generated code changes, run the script again.

## Put it on your iPhone or iPad

The `.ipa` is unsigned. Any sideloading tool that signs with your own Apple ID can install it, for example
AltStore, SideStore or Sideloadly. With a free Apple ID the app has to be re-signed every 7 days.

Signing it yourself for a device (Apple Development certificate and a provisioning profile with the device in it) also
works, for example `codesign` plus a profile made in Xcode. `build.sh` doesn't do that yet.

## Put the disc on the device

1. Start the app once. It shows where the disc goes.
2. Copy your `.iso` into the app's folder:
   - on the device: Files app > On My iPhone (or iPad) > P3FES
   - from a Mac: Finder > your device > Files > P3FES, then drag the `.iso` in
3. The game starts as soon as the file is there. The first `.iso` in the folder (sorted by name) is used.

The folder also holds:

- `memcard.ps2`: the memory card, created on the first save
- `memcard.ps2.bak`, `memcard-server.ps2.bak`: the card save sync replaced last (this device's, the server's)
- `states/`: save states (`slot-1.state` to `slot-4.state`, about 46 MB each, and `before-load.state`)
- `log.txt`: the runtime's log from the last start. If something goes wrong, read it in the Files app.
- `.boot.elf`: the game's boot program, copied from the disc at each start

## Controls

- Game controllers (Xbox, PlayStation, MFi, Backbone and similar) work: A = Cross, B = Circle, X = Square,
  Y = Triangle, shoulders = L1/R1, triggers = L2/R2 (pressure sensitive), Menu = Start, Options = Select, sticks, D-pad.
- Without a controller, the app's own on-screen controls appear as see-through outlines: D-pad and face buttons at the
  top, left stick at the bottom left, L1/L2 and R1/R2 in the top corners, SELECT and START at the bottom right, and MENU at
  the bottom left. A pressed control lights up. A finger can slide across the D-pad and the face buttons, and a finger
  between two face buttons presses both. Any touch low on the left side of the screen takes the stick. Every press lasts
  at least 0.1 s, so a quick tap is never shorter than the game's own check of the pad.
- MENU pauses the game. There you save and load states, pick the screen shape and resolution, show the speed, set how
  visible the controls are (10 to 80%, 30% at first) and set up save sync.
- Connecting a controller hides the on-screen controls except MENU.

## Screen shape

MENU > Screen picks how much of the screen the 3D scenes fill. Menus, text, the HUD, portraits and movies stay where they
are in 4:3, at their normal shape, in all three:

- **4:3**: the picture as on the PS2, with black bars at the sides.
- **Wide** (the default): the 3D view is widened to 1.70:1, the value the PC window's widescreen mode uses, at which no
  scene is known to get stuck. Narrow bars remain at the sides of a phone. Never wider than the screen.
- **Full screen**: the 3D view as wide as the screen (about 2.17:1 on an iPhone 18 Pro, the area by the camera
  included). Some scenes wait for a character to walk out of view, and with the wider view such a scene may not go on
  (this happened in the PC window at true 16:9, see docs/known-issues.md). If a scene seems stuck, switch to Wide or 4:3
  until it has passed: the change is immediate, also in the middle of a scene.

On an iPad (about 1.43:1, e.g. iPad Pro 11") Wide and Full screen are the same: the screen is filled, and the view is
narrower than Wide's 1.70, so the stuck-scene risk does not apply. Checked on the iPad Pro 11" (M5) simulator: the
lockers dialogue fills the screen at 60 fps.

How it works (`wide.cpp`, and `ios_main.mm` for the field of view):

- The game's horizontal field of view (a float at `007cad1c` that holds 4/3) is set to the picture's shape every frame,
  as the PC window's widescreen patch does. It is only rewritten while it holds 4/3 or a widened value.
- The picture is shown at that shape: `present()` in the iOS copy of `gpu.cpp` reads the shape from the app.
- P3 draws its 2D layer through the same path as the 3D scene, but flat: all corners of a triangle at one distance (one
  q, one depth), either in front of everything (depth 0xF000 and up) or, for logo screens, textured at a fixed q far above
  any the scene uses. `draw()` in the gpu.cpp copy hands every triangle to `portios_widen` first, which narrows that 2D
  layer toward the middle by exactly as much as the picture is widened, and clips it to the 4:3 part (the game parks some
  menu parts just past the edge). Untextured full-width shapes (fades, tints, the clear) and copies of the picture itself
  keep the whole width.
- Checked in the Simulator: the field with its HUD, dialogue with portraits, a battle with its command wheel and the green
  Dark Hour tint, the level-up screen, the Velvet Room, an FMV (4:3 with the movie's dark blue across the sides), the logo
  screen, and switching between the three while the game runs, at 60 fps. Not seen yet: other menus (the pause menu,
  shops, Social Link screens, the fusion screens), every kind of effect and transition.
- The CPU renderer stays 4:3.
- `P3FES_WIDE_LOG=1` (`SIMCTL_CHILD_P3FES_WIDE_LOG=1` in the Simulator) writes to `log.txt`, every 2 seconds, how one frame
  was drawn and which parts were narrowed, to sort out something placed wrongly.

## Resolution

MENU > Resolution draws the game at 1x to 4x the PS2's 640x448 (the default is 2x, 1280x896). The list marks the scale
that matches the screen: 3x on an iPhone (1206 lines), 4x on an iPad Pro 11" (1668 lines). It takes effect the next time
the app starts, because the GPU renderer sets up its memory for one scale when the game starts. MENU > Show speed puts the
frames a second and the scale in the top left corner.

The renderer imitates the PS2's GPU exactly, and its work grows with the square of the scale. In the Simulator on an M1 Pro
(14 GPU cores), lockers scene: 60 fps at 1x and 2x, 47 at 3x, 29 at 4x (42 and 26 before `flip.cpp`), limited by the GPU
(no CPU thread is busy).

Faster drawing (`flip.cpp`, 11 November 2026): the renderer's drawing pass is most of the GPU's work (about 80%). It
checked every pixel of a textured triangle for a texture drawn backwards at 1:1 ("flip_fix"), work that is the same for
the whole triangle. The iOS build now does it once per triangle on the CPU, and its copy of the shader leaves it out.
Measured with the native build's GPU timestamps at 2x (M1 Pro): the drawing pass went from 13.1 to 12.1 ms a frame in the
school lobby, 21.3 to 17.6 in the locker room and 15.6 to 12.7 in a battle. The pictures are the same: frames compared
with the old shader are identical in the battle, and elsewhere differ by 1/255 in at most 62 pixels of 1.1 million (float
rounding). Tried and dropped, because they were slower on this GPU: testing depth before working out colour and texture,
and copying each tile's triangles into workgroup memory. An M1 iPad has 8 GPU cores, so expect 2x to be about the most that keeps 60 there; the screen-matching scale is
likely too slow on both devices. The runtime allows up to 3x; the iOS copy of gpu.cpp allows 4x. Save states load at any
scale (the runtime converts the picture memory).

## Save states

MENU > Save state keeps the whole game as it is, at any moment, in one of four slots; MENU > Load state goes back to one.
Each slot shows when it was saved. The game stays paused while a state is saved or loaded (about a second).

- A state does not hold the memory card, so loading one never takes back a save made in the game. A game loaded from a
  state may not notice saves made after that state until it reads the card again.
- Before each load the game as it is goes to `before-load.state`, so "Undo the last load" in the load list takes you back.
  If a state can't be loaded (a damaged file, or one from another version of the app), the game goes on from there too.
- States are made for this build of the app. After an update, a state from the old version may no longer load; the
  memory card always carries over.
- With save sync on, states go to your server too (see below). They are also in the app's folder, so the Files app can
  copy them.
- The runtime saves states only between two slices of the game, where no translated code is running; the iOS copy of
  `system.cpp` (made by `CMakeLists.txt` in the build folder) lets the app save or load one at that point.

## Save sync with your own WebDAV server

MENU > Save sync settings: the address of a folder on your WebDAV server (Nextcloud, ownCloud, a NAS, Apache or nginx
WebDAV, `rclone serve webdav`, ...), a user name and a password. "Save and check" tests it. Plain `http://` and servers on
your local network work too, and "self-signed certificate" accepts a home server's own certificate. The password is
kept in the iOS keychain (where the keychain is not available, as in the Simulator, it is kept in the app's private
Application Support folder instead).

- The card is stored on the server as `<folder>/memcard.ps2` (or at the exact address, if it ends in `.ps2`).
- When the game starts, the app compares the server's card with this device's: it downloads the server's if only that one
  changed since the last sync, uploads this device's if only that one changed, and asks which to keep if both did (or
  on a device's first sync, when they differ). The card it replaces is kept in the app's folder.
- While the game runs, every save the game writes is uploaded about 2 seconds later, and once more when the app leaves the
  screen. The game holds the card in memory once it has started, so a newer card on the server can only be used from the
  next start: the app then asks, and "Use the server's save" in the menu downloads it and closes the app.
- Without a connection the game just starts with this device's card; the next save tries again.

Save states are synced as well, each slot on its own:

- A saved slot is uploaded right away, compressed (`<folder>/states/slot-N.state.deflate`, raw DEFLATE, about 9 MB
  instead of 46).
- When the Save state or Load state list opens, the app asks the server about each slot. A slot whose newest copy is on
  the server (saved on another device) shows "(on your server)" with its date; loading it downloads it first. For each
  slot the newer copy wins, by date, so saving in a slot replaces the server's copy too.
- A slot that couldn't be uploaded (no connection) is sent the next time a list opens. If the server doesn't answer
  within a few seconds, the lists show this device's states.
- `before-load.state` (the undo) stays on the device.

## Smoothness

The iOS build is tuned for the phone rather than sharing the 3DS settings:

- Its own translation (`generated/p3fes-ios`, made by `build.sh`): full 64-bit registers, no loop locals, checked
  stack accesses and no hot copies. The 3DS cuts trade accuracy for speed on a slow ARM11; an iPhone does not need them.
- The GPU picture goes straight to a Metal layer. Nothing is read back from the GPU to the CPU each frame.
- A steady 59.94 Hz clock (`mach_wait_until`), nudged up to 5% faster or slower to keep about 70 ms of sound queued, so
  frames neither bunch up nor drift.
- The game and render threads run at the highest thread priority (user-interactive), which iOS favours when it hands out
  the fast cores.
- A 64 MB game stack, so the stack unwinds far less often.
- The screen refreshes at 60 Hz, also on 120 Hz screens.
- No GPU work while the app is in the background.

## What works, what doesn't (2026-11-11)

Tested in the iOS Simulator (iPhone 18 Pro, iOS 27.2):

- GPU build (wgpu), presenting through Metal at a steady 60 fps: boot, disc reading, intro movies with sound, the 3D
  title screen at full speed (the sound queue stays full, so the game isn't behind), "Press any button" with the
  on-screen START, the main menu (The Journey / The Answer), the cursor moved with the on-screen D-pad
- CPU build (no wgpu): same up to the title screen, but the 3D title scene runs at about 5 frames a second, even on
  a Mac's CPU. Use the GPU build.
- not tried yet: a real iPhone or iPad (speed on a phone is unknown), iPad layout, the on-screen stick in the game,
  gameplay past the main menu, saving from inside the game, a real controller's buttons (one was detected and hid the
  on-screen controls as it should)
- not in the iOS front end (desktop window features): subtitles overlay, timed autosave states,
  rewind capture, key remapping, rumble, speed readout (the fps goes to `log.txt` every 5 seconds)
- the app pauses when it goes to the background (iOS suspends it) and goes on when you come back; nothing is saved
  automatically, so save in the game (or save a state) before you leave it for long
- the game pauses while the menu is open and while the app is not in front, and does no GPU work in the background
- save states, tested in the intro movie and at the title menu: save, load (the game and the controls go on from the
  saved moment), undo the last load
- save sync, tested against `rclone serve webdav`: first upload, "up to date", download of a newer server card,
  the question when both changed (keeping either side), and the in-game question when another device uploaded
- state sync, same server: slots only on this device uploaded when the list opens, a new save uploaded (the server's
  copy unpacks to the same bytes), a slot replaced on the server shown as "(on your server)", downloaded and loaded, and
  not sent back afterwards
- screen shape: Full screen and Wide in the scenes listed under "Screen shape"; 4:3 unchanged
- resolution: 1x to 4x on the lockers scene (speeds under "Resolution"), 2x in battle, Show speed
- for testing: `SIMCTL_CHILD_P3FES_TOUCH=1 xcrun simctl launch <device> local.p3fes.recomp` keeps the on-screen
  controls when the Simulator reports a game controller; `SIMCTL_CHILD_P3FES_MUTE=1` starts with no sound at all;
  `SIMCTL_CHILD_P3FES_TEST_STATE=<path>` loads that save state 4 seconds after the game starts

## Files

- `CMakeLists.txt`: the runtime, the generated code and the app; `P3FES_WGPU_DIR` switches on GPU drawing (and the gpu.cpp copy)
- `ios_main.mm`: the UIKit front end (disc lookup, frames, pacing, sound, input, save states, screen shape, menu)
- `wide.cpp`: the screen shape in the renderer (narrows the 2D layer, gives `present()` the picture's shape)
- `flip.cpp`: faster drawing (a per-pixel check of the GPU shader done once per triangle; CMakeLists.txt takes it out of
  the iOS copy of `gs.wgsl`)
- `touch.mm`: the on-screen controls
- `webdav.mm`: save sync with a WebDAV server (memory card and save states)
- `Info.plist.in`: app settings (Files app sharing, landscape, controllers)
- `build.sh`: configures, builds and packs the `.ipa`
