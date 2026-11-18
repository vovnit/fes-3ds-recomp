# Engine update: PS2 Web Recomp after 16 October 2026

The game runs on [PS2 Web Recomp](https://github.com/snuri00/ps2-web-recomp) (snuri00, MIT) with our tool patches on top.
`p3fes_install.sh` has always used one fixed version of it (`PIN`, 16 October 2026), the one the opening, the town and the first
Tartarus floors were played on. Upstream has moved on by 117 commits since. This page says what the newer version brings, what
was changed here so our patches fit it, and how to try it.

## Status (18 November 2026)
| Check | Tested version (`PIN`, c628ae8, 16 Oct) | Newer version (`PIN_NEXT`, d8a0a45, 12 Nov) |
| --- | --- | --- |
| All 67 installer patches and the Windows patch fit | yes | yes |
| Translator, runtime and tools build (clang 18, Linux) | yes | yes |
| The engine's own synthetic tests (`ctest`, `scripts/test_vu_codegen.py`) | yes | yes (5 runtime tests, 10 native and 10 Wasm VU tests) |
| Played with a disc | yes | **not yet** |

So the default stays the tested version. To try the newer one, install into a second folder:

    bash p3fes_install.sh --engine next --dir ~/ps2-web-recomp-next

`--engine` takes `tested` (default), `next`, or any commit id. It only matters when a project folder is made; an existing folder
keeps its version. Please report what you see (the playtest checklist in [qa-guide.md](qa-guide.md) is a good order).

## Checking a version yourself
`tools/p3fes_engine_check.py` applies every patch to a clean copy of any version and says which ones do not fit; `--build` also
builds it and runs the engine's own tests (no game data needed):

    python3 tools/p3fes_engine_check.py ~/ps2-web-recomp tested next origin/main --build

Run `git fetch` in the clone first to see the newest upstream version. This is the first thing to run when upstream changes.

## What the newer engine brings
* **Fixes we had as patches are now upstream.** The PATH3 mask rule (`e615191`, held in the VU thread, one packet per mask pulse)
  and the VU branch VI delay (`7b6b604`, `18ad888`, `c243416`, for the interpreter, the JIT and the compiled programs; it covers every
  instruction that writes an integer register except the flag reads, a little more than our version did). On such an engine
  `ps2run_fix_path3_mask.py` and the three `*_vubranch_patch.py` print "not needed" and change nothing.
* **Code the translation missed no longer stops the game:** an EE interpreter runs it (`686d9aa`), and the page offers a copyable
  report when the game halts (`c8a0f88`).
* **Timing and sound:** IOP timers raise their interrupt (`309e03f`), SPU2 STATX, scratchpad DMA interleave, SIF DMA in whole
  quadwords, system calls take EE cycles, VU0 macro flags, CD seeks complete later with their callback.
* **Picture:** only the lines the video mode shows are drawn, cropped by how far a game moves the picture (`a75fcdd`, `fe11596`);
  worth checking against our movie bars and edge fixes.
* **Its own 16:9 setting and PCSX2 patch files** (`c0f476e`, `6526f48`): `--widescreen` / `--patches` in the PC window, an aspect
  menu in the page. We keep our own widescreen (`PS2_ASPECT`, the mod build); see below.
* **Tools for testing:** `scripts/regress.sh` replays recorded runs and names the ones whose pictures changed, GS frame capture of
  chosen frames, save-state export and import, quick saves in the page.
* Also: a macOS Metal path for the native window, a player 2 invite in the page, touch controls that scale.

## What was changed here so the patches fit both versions
Every patch still accepts the older text; where upstream changed the text, the patch also accepts the newer one.
* `ps2run_controls_patch.py`: upstream already moved the stick off the arrow keys (to I J K L); ours moves it to W A S D as before.
* `ps2run_sifquiet_patch.py`: the "no handler" message now names the EE buffer; the once-only check keeps those words.
* `ps2run_autosave_patch.py`, `ps2recomp_modswitch_patch.py`, `ps2port_modswitch_patch.py`: longer usage and option lists.
  `ps2recomp_modswitch_patch.py` now writes its four files only when every edit fits (before, a source it did not know could be
  left half patched).
* `ps2run_widescreen_patch.py`: on the newer engine `--widescreen` starts the picture at 16:9 and `PS2_ASPECT` still decides; the
  code it leaves is the same as on the older engine, so `ps2run_fillwindow_patch.py` and the browser patch fit unchanged.
* `ps2run_poke_patch.py`: the program loader has a write-watch line between the copy and the clear.
* Browser page (`ps2web_cardbackup`, `speedrun`, `gamebeat`): buttons moved into a Menu panel (`host-only`), the engine script is
  started from a template, so our scripts go at the end of the page.
* `ps2run_windows_patch.py`: the window and surface kinds gained `Metal`, more tools are built with `-Wall`.
* **Browser aspect:** the newer page starts at 16:9 and tells the game the TV is wide. The launcher now always asks for `aspect=4:3`
  (older pages ignore it), so the picture shape still follows Settings > Widescreen.

## What to look at when playing the newer engine
1. Start-up, the opening movie and the FES intro (interpreter fallback, display crop).
2. The cut-in portraits during battle (the VU branch fix is upstream's version now).
3. Menus and text over textures (PATH3 rule is upstream's version now).
4. Widescreen on and off, in the PC window and the browser.
5. Sound effects and music over a long session (IOP timer and SPU2 changes).
6. Essential Mods with switches (the mod-switch translator patch on the newer translator).
If all of that holds, `PIN` can move to `PIN_NEXT`.
