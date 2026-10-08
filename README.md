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
  the hook returns to real time exactly on the frame the replay ends. While
  fast-forwarding it also skips drawing (D3D `Present`/`Clear`/`Draw*` return at once,
  except for the last 2 frames so the frozen picture is real) and answers the game's
  key polls without asking Windows: about 1000 frames per second instead of 340. It also
  stops the game's own render-command queue from running the drawing commands that are
  safe to skip (about 1.3x faster again; see the docs).
- **Frame advance.** The hook blocks the game thread inside the input poll at a
  frame marker (before the frame's input is read) and releases it one frame per step.
  The virtual clock is frozen during the hold so the limiter does not see the pause
  as elapsed time. The game survives long holds (tested: 20 s).
- **Focus.** The game only accepts input while its window is active, which it learns
  from window messages (it imports no focus query). The hook wraps its window
  procedure so it never sees a deactivation, so scripted input and frame advance work
  even while the editor has focus. Live keys (recording, resume) are gated on the
  game window really being in the foreground, so typing in the editor never leaks in.
- Details and every finding: [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

## How the game's RNG works

Drops, enemy behaviour and the like come from one random number generator. Knowing how
it is fed explains why a replay could differ from run to run, and what **RNG seed** does.

1. **The generator.** It is a small xorshift128 generator: four 32-bit numbers (its
   state) that are scrambled a little each time the game asks for a random number
   (`w % range` for "pick one of N"). The same four numbers always produce the same
   sequence of results. The state lives in the game's manager object; the Memory window
   shows it as *RNG state x / y / z / w*.
2. **The seed.** When the game starts, one 32-bit number (the seed) is spread over the
   four state words, and the state is then only advanced by random draws.
3. **Where the seed comes from.** The game uses the current time in seconds (Unix time) at
   launch. Launch the same movie one second later and the state is different, so a drop
   (for example 18 or 19 weapon points from a candle) can come out differently.
4. **What the tool does.** With an RNG seed set (*Run > RNG seed...*), the hook gives the
   game a clock frozen at that number. The game "launches" at that time every run, gets the
   same seed, and so the same state at frame 1.
5. **Why the state sometimes does not move.** Each draw advances the state, but a draw only
   happens when the game needs a random result. Menus, boot and a standing player can go
   for hundreds of frames without any, and the Memory window will show the same numbers
   until something random happens.
6. **What the seed cannot do.** It fixes the start only. If you change your inputs, the
   game may draw a different number of random values before an event, and that event's
   result changes too. Seed 0 makes all four state words 0, which xorshift never leaves, so
   the RNG would never change; the real game seeds from the clock and cannot produce 0, so
   that value is not a real case and is left as is.

How it was found (addresses, constants, the code path of a drop):
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md#rng).

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
| **Run to cursor** (F4, or right-click a row > *Run game to frame N*) | Advance the frozen game through many frames at once, up to the cursor row, using the grid's inputs (past the end of the movie it appends blank frames). Runs at the fast-forward speed and gives the same result as stepping one by one (500 frames in about 1 s instead of 15 s). **Stop** (F9) cancels and freezes at the next frame. If the game is already at or past the cursor, use Rewind |
| **Record here** (F12) | Unfreeze the game and record your live keyboard from the frozen frame (click the game window and play). **Stop** (F9) freezes the game again right after the last recorded frame, so you can keep stepping or record again |
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
- **Frame notes.** Right-click a row (or use the Edit menu) to add, edit or remove a
  note for that frame. One note per frame; it shows in the **Note** column and as an
  orange flag beside the frame number. **Notes list...** opens a window with every note;
  double-click (or Enter) jumps the cursor to the frame, `Delete` removes the note.
  Notes move with their frames on insert, delete, cut and paste, are covered by
  undo/redo, and are saved in the `.bscotm` file (an optional `notes` field, so older
  files still load). Painting or clearing inputs leaves notes alone; **Record new**
  starts a movie without notes.
- **Input patterns.** `Ctrl+R` (*Edit > Repeat selection*) copies the selected frames N more
  times right after the selection, as inserted frames. `Ctrl+Shift+R` (*Edit > Fill selection with
  a pattern*) fills the selected frames with a repeating press of one key: type `KEY period on
  [offset]`, e.g. `Jmp 12 2` presses jump for 2 frames out of every 12, `R 1 1` holds right on
  every frame. The key is released on the other selected frames; `offset` frames are skipped first.
  Both are one undo step.
- **RNG seed.** *Run > RNG seed...* sets a number (0 - 4294967295) that is saved in the
  movie (`rng_seed`; empty = none). The game seeds its random generator from the Unix
  time in seconds at launch, which is why a replay could give different drops from run
  to run. With a seed set, the game sees a frozen clock at that time, so every launch
  gets the same random numbers. The status bar shows the seed. Changing it restarts the
  game on the next Rewind / frame advance. Details: `docs/REVERSE_ENGINEERING.md`.
- **Find.** `Ctrl+F` (*Edit > Find*) moves the cursor to the next frame where a key is pressed:
  type the key (`Jmp`), add `released` for the next release (`Atk released`), or `*` for any change
  in any key. `F3` / `Shift+F3` find the next / previous match. `Alt+Down` / `Alt+Up` jump to the
  next / previous frame that has a note or bookmark.
- **Bookmarks and jumping.** `Ctrl+B` (or right-click a row > *Add bookmark here...*) names the
  cursor frame as a bookmark (shown with a purple flag and a star). `F2` / `Shift+F2` jump to the
  next / previous bookmark, `Ctrl+G` jumps to any frame number, and *Edit > Bookmarks list...*
  (or double-click an entry there) jumps to one. **A jump to a frame the frozen game has not reached
  yet just runs the game forward at the fast-forward speed (no restart); a jump back restarts the
  game and fast-forwards to the frame.** Bookmarks are frame notes with a flag: they move with
  their frames on insert / delete / paste, are covered by undo, and are saved in the `.bscotm`
  file (`"bookmark":true` on a note, so older files and the Python tool still load).
- **Verify fast-forward.** *Run > Verify fast-forward...* replays the whole movie twice at the
  fastest speed, once with all the speed-ups and once with only the clock speed-up and normal
  drawing, and compares the game's values (the Memory window's list: health, weapon points,
  speeds, positions, RNG state, ...) at about 20 evenly spaced frames. Use it on your own movie,
  especially one that goes through other stages, before trusting a long fast-forward. It reports
  the first checkpoint and value that differ, or that all matched. Checkpoints while the game is
  still loading (no player yet) are skipped because loading runs on real time. The picture is
  not compared. About 40 seconds for a 10,000-frame movie.
- **RNG log.** The **RNG log** button in the menu bar opens a window that lists every random
  number the game draws while it runs the frames near where you stop (the last 300 frames of a
  Rewind, Run to cursor or Jump, and every frame you advance by hand). Tick *Log the game's random
  draws*, then step or jump as usual. Each row is one draw: the movie frame, the code that made it
  (`exe+...`; for the game's draw function that is the call site), the range it was asked for
  (e.g. 100 for an item-drop roll), the result (`state w % range`) and the generator state after
  it. A frame with no rows made no draws. Use it to see which frame decides a drop and which input
  change would move it; *Save CSV...* exports the list. It uses a hardware write breakpoint on the
  RNG state (no game code is changed), so logging slows the frames it covers a little.
- **Value history.** In the Memory window, tick **Record history**, then step, run or jump as
  usual: the hook samples health, weapon points, score, X/Y speed, X/Y position and invisibility
  at every frame (also during fast-forward). **Graph...** draws the selected values over the
  frames, each scaled to the full height (the legend shows its range; hover to read one frame);
  **Save CSV...** writes every recorded frame. A new game launch starts a new history.
- **Boss HP.** While the stage 1 boss exists, the Memory window also shows **Boss HP** and
  **Boss max HP** (120 for that boss; the TAS in `test3.bscotm` takes 7 per sub-weapon hit). The boss
  is found by a background search of the game's memory (about a second after it spawns), because it has no
  fixed pointer. Other bosses are not known yet; how it was found and how to add another is in
  `docs/REVERSE_ENGINEERING.md`.
- **Help.** The **Help** menu (or `F1`) opens an in-app guide: getting started, recording and
  playing, frame advance, editing, bookmarks and jumping, the RNG seed and log, the Memory
  window, Verify fast-forward, keyboard shortcuts and troubleshooting. It is part of the program,
  so it always matches the version you run.
- **Game memory window.** The **Memory** button in the menu bar opens a window that shows
  the running game's health, weapon points (and max), score, X/Y speed, X/Y position
  (physics and render copies), invisibility, difficulty, style, the four characters and
  the four RNG state words. It updates 10 times a second for any COTM.exe that is running
  (frozen or live) and shows `-` where the game has no value yet (menus, loading).
  Read-only; the pointer chains come from the community cheat table.
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
- The RNG seed fixes the random numbers at launch only; changed inputs can still change later random results (see "How the game's RNG works").

## History

The original Python/Frida implementation is kept in [`archive/`](archive/).

## License

MIT
