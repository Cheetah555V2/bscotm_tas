# bscotm-tas

A tool-assisted speedrun (TAS) tool for **Bloodstained: Curse of the Moon**
(Steam, Windows, v1.1.2), with a TAStudio-style frame editor.

Record frame-perfect keyboard input, edit it in a piano-roll grid, and play it
back. The whole tool is two small native files (no Python, no Frida):

| File | What it is |
|---|---|
| `bscotm_tas.exe` | The editor (plain Win32, no dependencies) |
| `bscotm_hook.dll` | Tiny hook injected into the game at launch |

## How it works (short)

- The game polls 81 virtual keys every frame through `user32!GetAsyncKeyState`.
  The hook DLL patches **COTM.exe's own import slot** for that function (the game
  checks its `.text` for tampering, but the import table is safe) and only acts
  on calls whose return address is the game's input poll (`COTM.exe+0x2A5960`).
- The editor and the hook share one memory block: the editor writes the frame
  schedule, the hook injects it (or records the real keyboard).
- **Frame 1 is the game's own first frame marker.** Playback and recording both
  start there, never after a wall-clock "boot finished" wait, so a slow (cold)
  first launch cannot shift the inputs.
- **Fast-forward.** While replaying to the cursor the hook scales the game's clocks
  (`QueryPerformanceCounter`, `timeGetTime`, `GetSystemTimeAsFileTime`, `Sleep`,
  `WaitForSingleObject`, again through the game's own import slots) and drops
  vsync when the D3D9 device is created. Both are needed: the game paces itself on
  QPC *and* vsync. Frames stay deterministic (same end state at 1x and 50x), and
  the hook returns to real time exactly on the frame the replay ends.
- **Frame advance.** The hook blocks the game thread inside the input poll at a
  frame marker (before the frame's input is read) and releases it one frame per step.
  The virtual clock is frozen during the hold so the limiter does not see the pause
  as elapsed time. The game survives long holds (tested: 20 s).
- Details and every finding: [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

## Build

### Download

Prebuilt zips (exe + hook DLL) are attached to each
[release](../../releases). Unzip anywhere and run `bscotm_tas.exe`; keep
`bscotm_hook.dll` next to it.

### Build it yourself

The game is 32-bit, so the build needs a **32-bit** MinGW-w64 toolchain. With
[MSYS2](https://www.msys2.org), in an MSYS2 shell:

```bash
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-make
```

Then, from the repo folder:

```bat
build.bat
```

`build.bat` finds the toolchain by itself: `%MINGW32%` (its `bin` folder), then
`g++` on `PATH`, then the usual MSYS2 folders (`C:\msys64`, ...). If yours lives
elsewhere, `set MINGW32=<path>\mingw32\bin` first. From an MSYS2 MINGW32 shell,
plain `make` works too. Output goes to `build\`.

### Publishing a release (maintainers)

Pushing a tag builds on GitHub Actions and attaches the zip to a new release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Using it

Run `build\bscotm_tas.exe`. It finds
`Bloodstained Curse of the Moon\exe\COTM.exe` by searching the folders above it;
otherwise use **File > Set game path**.

| Button / key | Action |
|---|---|
| **Record new** (F8) | Relaunch the game and record from its first frame |
| **Play** (F5) | Relaunch the game and play the whole movie in real time |
| **Rewind to cursor** (F6, or double-click a frame number) | Relaunch, fast-forward to the cursor row, then **freeze** the game there |
| **Frame advance** (`.`) | Run exactly one frame of the frozen game using the next grid row's inputs (hold `.` to auto-repeat) |
| **Resume live** (F11) | Unfreeze the game and take over with the keyboard (not recorded) |
| **Record from cursor** (F7) | Play up to the cursor row, then record live from the next frame |
| **Stop** (F9) | End the current run |

Frame advance works like TAStudio: the game is held at a frame boundary, before the
next frame reads its input, so you can still edit that row. Stepping past the end of
the movie appends blank frames. If you edit a row *before* the game's position, the
next advance first replays to the greenzone edge, then steps. Stepping forward only;
to go back, move the cursor and Rewind (it replays from frame 1 at speed).

Rewind and Record from cursor fast-forward at the speed chosen in
**Run > Fast-forward speed** (default Max, 50x: a 1461-frame prelude takes about
5 s instead of 26 s). Audio is garbled while fast-forwarding.

**Baseline** is a snapshot of the game's save files restored before every
launch (so runs start from the same state). Create one with
*File > Save current game saves as baseline* while the game is closed.
**Prelude** is the input that takes the game from launch to the first
controllable frame; it is played before the movie. Record one, then
*File > Save as prelude*. Existing `.bscotm` movies load unchanged, but
**preludes recorded with the old Python tool should be re-recorded**: they were
captured against a different start point.

### Editing

- Click or drag on key cells to toggle/paint; click a frame number to select
  rows (Shift or drag to extend). Rows past the end extend the movie.
- `Delete` clears inputs, `Insert` inserts blank frames, `Ctrl+Delete` removes
  frames, `Ctrl+C/X/V` copy/cut/paste (`Ctrl+Shift+V` pastes as insert),
  `Ctrl+Z/Y` undo/redo.
- Green frame numbers are the "greenzone": frames the live game has been advanced
  through. Editing a frame invalidates the greenzone after it.

Settings are stored in `bscotm_tas.ini` next to the exe. Save backups, baselines
and preludes live in `save_backups\` and `preludes\` next to the game folder.
Your saves are copied to `save_backups\last_user_state` before a baseline is
restored.

## Known limitations

- Rewind = restart the game and fast-forward (still replays from frame 1). No true
  savestates: the game's anti-tamper makes snapshot/restore unreliable.
- Keyboard only. No controller (DirectInput) support yet.
- RNG is not seeded.

## History

The original Python/Frida implementation is kept in [`archive/`](archive/).

## License

MIT
