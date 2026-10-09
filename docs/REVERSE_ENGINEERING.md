# Reverse-Engineering Notes

**Game:** Bloodstained: Curse of the Moon (Steam, AppID 838310, v1.1.2)
**Platform:** Windows x86 (32-bit, WoW64)
**Tools used:** Frida, Cheat Engine, x32dbg + ScyllaHide, Ghidra (static)

---

## Legal notice

This document contains **factual observations** about a commercially released
game, gathered for the purpose of interoperability — specifically, to enable a
Tool-Assisted Speedrun (TAS) tool that the game does not natively support.

- **No copyrighted code, assets, or data are redistributed here.** Only
  discovered offsets, addresses, and behavioural observations are documented.
- **No DRM is circumvented.** The game's Steam DRM is untouched; this work
  focuses on the game's own anti-debug/anti-tamper logic, which is not a
  copyright protection measure.
- **Reverse engineering for interoperability is broadly permitted** in many
  jurisdictions (US: DMCA §1201(f), *Sega v. Accolade*; EU: Software Directive
  Art. 6). EULAs may restrict this contractually — review yours.
- **This is a TAS tool, not a cheat or piracy tool.** It works only with a
  legally purchased copy and does not unlock content or bypass purchase.

If you are the rights holder and object to any part of this document, please
open an issue and it will be amended or removed.

---

## Process facts

- 32-bit (Win32) executable. ASLR is enabled — the image loads at a random
  base every run. Never hardcode `0x400000`. All addresses in this document
  are **RVAs relative to the module base**, written as `cotm.<RVA>`.
- Custom in-house engine, called "Ice" internally. Class names visible in
  strings: `IceCoLocalImpl`, `IceTask`, `IceContext`, `IceTSingleton`,
  `cBs03`, `cGameData`, `cSceneGameOver`, `cSubMenuKeyConfig`.
- Steamworks integration via `steam_api.dll`. Steam's overlay injects
  `gameoverlayrenderer.dll` into the process, which **also calls
  `user32!GetAsyncKeyState`** — naive hooks will catch overlay noise. Filter
  by return address.
- Audio: XAudio2 (`XAudio2_7.dll`). Graphics: Direct3D 9
  (`d3d9.dll` + `d3dx9_43.dll`). DirectInput8 for gamepad
  (`DINPUT8.dll`).

---

## Anti-tamper

This is the single biggest obstacle to advanced tooling.

### What is known

- **`COTM.exe` verifies its own `.text` integrity.** Any software breakpoint
  (INT3 or UD2) placed inside the code triggers a crash within ~2 seconds.
  Even UD2 — which is not INT3 — is detected.
- **Hardware breakpoints do not work reliably.** `Thread.setHardwareBreakpoint`
  in Frida:
  - At a low-frequency address (e.g. `cotm.31CCC5`, ~3 Hz): no hits, no crash —
    the breakpoint appears to be a no-op or silently cleared.
  - At two breakpoints on the same thread: **immediate access-violation crash**
    (`ExceptionCode 0xC0000005` at `EIP=0x0`).
  - At `user32!GetAsyncKeyState` (a system DLL, thousands of calls/second):
    immediate access-violation crash the moment threads are resumed.
- **The DR-register read path is not the detector.** Hooking
  `ntdll!NtGetContextThread` records zero calls across 15 seconds of normal
  play — the game never reads debug registers.
- **No VEH, no UCP handler, no UEF.** Hooking `AddVectoredExceptionHandler`,
  `AddVectoredContinueHandler`, and `SetUnhandledExceptionFilter` shows no
  registration by the game.
- **`KiUserExceptionDispatcher` sees no exceptions** during normal play —
  the game does not use exceptions as control flow, and our breakpoint
  exceptions never reach user-mode dispatching (the crash is faster).

### What this implies

The game likely has one of:

1. A kernel-mode or driver-level component that watches for debug register
   writes at the hardware level.
2. A timing-based watchdog: any thread that fails to advance for N ms causes
   the process to be terminated. This explains why low-frequency software
   breakpoints at `cotm.31CCC5` survive (only 3 pauses/second) but
   per-frame breakpoints at 60 Hz kill the process within seconds — the
   watchdog fires when the pacer stalls.
3. A checksum loop scanning specific `.text` pages with tight timing, that
   detects any modification — including UD2 — within a fraction of a second.

The most likely explanation given the symptoms is **(2)**, the watchdog. This
would also explain the earlier Memory snapshot restoration failure: the
restore operation is slow enough (100 ms+) that the watchdog terminates the
process.

### What is safe

- **Hooks in system DLLs (`user32.dll`, `kernel32.dll`) are safe.** Our
  entire input hook lives here and has never crashed the game.
- **IAT patches are safe.** We patch the Sleep IAT slot at `cotm.2bddac` and
  restore it later; the game never detects this.
- **Reading memory via `ReadProcessMemory` is safe.** Our status reader and
  pointer chains work fine.
- **Writing to the game's own data section** (not `.text`) is safe.

### What has been ruled out (tested and failed)

| Technique | Result |
|---|---|
| `Interceptor.attach` in `COTM.exe` | Crash within 2 s |
| x32dbg INT3 breakpoints | Crash |
| x32dbg UD2 breakpoints at 60 Hz addresses | Crash |
| x32dbg UD2 breakpoints at 3 Hz addresses | Survive (80 hits observed) |
| Frida `setHardwareBreakpoint` on 1 thread | No effect, no crash |
| Frida `setHardwareBreakpoint` on 4+ threads | Immediate crash |
| `NtSuspendProcess` + full memory dump + `WriteProcessMemory` restore | Deadlocks 2/15 threads |
| QueryPerformanceCounter scaling (5×) | Speeds audio only, not logic |
| Sleep/SleepEx/NtDelayExecution zeroing | No effect on logic |

---

## Input system

- The game polls **81 virtual keys** every frame via
  `user32!GetAsyncKeyState`. All polls from the game's own loop come from a
  **single return address: `cotm.2a5960`**. Filter hooks by this address to
  ignore the Steam overlay's and DirectX's own calls to the same function.
- The VK list is a **fixed 81-entry DWORD table** at
  `module_base + 0x3d1760`. Index 0 is always `0x0000` (used as a frame
  boundary marker by our hook).
- The game maintains two parallel **81-byte arrays**:
  - `STATE_CUR[i] = GetAsyncKeyState(VK_TABLE[i]) >> 15` — this frame
  - `STATE_PREV[i] = previous frame's STATE_CUR[i]` — for edge detection
  - `STATE_CUR` base is some heap address `S`; `STATE_PREV` is at `S - 0x80`.
- **There is no separate "action flag" byte.** The game reads the state
  array directly from game logic each frame. This is why hooking at the
  `GetAsyncKeyState` return works so cleanly — there's no intermediate
  buffer to chase.

### Tracked VK map

| Name | VK | Decimal | Purpose |
|---|---|---|---|
| A | 0x41 | 65 | Move left |
| D | 0x44 | 68 | Move right |
| W | 0x57 | 87 | Look up |
| S | 0x53 | 83 | Crouch |
| SPACE | 0x20 | 32 | Jump |
| LMB | 0x01 | 1 | Attack |
| RMB | 0x02 | 2 | Sub-weapon |
| Q | 0x51 | 81 | Prev character |
| E | 0x45 | 69 | Next character |
| P | 0x50 | 80 | Pause / skip cutscene |
| ENTER | 0x0D | 13 | Menu confirm |
| ESC | 0x1B | 27 | Menu cancel |

---

## Player struct

Pointer chain (CE XML order — deepest offset last in XML, **apply in
reverse**: deref first, add last):

```
p0 = *(module_base + 0x48365C)
p1 = *(p0 + 0x08)
p2 = *(p1 + 0x6C)
p3 = *(p2 + 0x20)
p4 = *(p3 + 0x20)
p5 = *(p4 + 0x08)
p6 = *(p5 + 0x84) <- player struct base
```


Fields (offsets from `p6`):

| Offset | Type | Field |
|---|---|---|
| `+0x1A0` | f32 | X velocity |
| `+0x1A4` | f32 | Y velocity |
| `+0x1AC` | f32 | X position (physics) |
| `+0x1B0` | f32 | Y position (physics) |
| `+0x3DC` | u8  | Health |
| `+0x53C` | f32 | Invisibility timer |
| `+0x5B8` | f32 | Render X (snaps to ±8 of physics X) |
| `+0x5BC` | f32 | Render Y |

The X/Y velocity values are plain floats. Walking speed is `±1.3333`.
The `+0x1A0` float reads `-1`, `0`, or `+1` — this is a direction
indicator, not the actual velocity (which is `1.3333`). The render
coordinates at `+0x5B8` snap back toward the physics coordinates with
a spring-like offset of ~8 units per frame.

---

## Frame pacing

The game holds 60 fps through a mechanism we could **not** definitively
identify. We were able to enumerate the relevant threads and their
behaviour:

### Pacer thread (`cotm.3dcc6e` entry)

The pacer thread's top-level function is at `cotm.31ccc5`. It is a
**task dispatcher** that:
- Fires ~60 times per second during normal play
- Fires 1–5 times/second during transitions (menu, stage load)
- Holds no locks at the loop top (`cotm.2a48a0`)

The loop body:

```
cotm.2a48a0 <-- loop top
...
cotm.2a4933 comiss xmm1, xmm0
cotm.2a4936 jbe cotm.2a495c ; not enough time elapsed
cotm.2a4938 cmp byte [esi], 1 ; <-- frame boundary
cotm.2a493b jne cotm.2a494d
cotm.2a493d ...
cotm.2a4946 call cotm.293070 ; per-frame work
cotm.2a494b jmp cotm.2a4964
cotm.2a494d ...
cotm.2a4955 call cotm.293070 ; per-frame work (alternate)
cotm.2a495a jmp cotm.2a4964
cotm.2a495c push 0
cotm.2a495e call Sleep ; Sleep(0) — yields CPU
cotm.2a4964 call cotm.2a4630 ; exit check
cotm.2a496d test al, al
cotm.2a496f je cotm.2a48a0 ; loop
```


`Sleep(0)` returns almost immediately, so the loop spins at high frequency
checking the clock until the timer target expires. When the target expires,
it runs the frame work once.

### Per-frame work (`cotm.293070`)

This function is called once per frame. It:
- Reads timing via `QueryPerformanceCounter`/`QueryPerformanceFrequency`
- Calls `cotm.352c00` to update the timestamp
- Reads/writes various engine objects
- Ends with `EnterCriticalSection`/`LeaveCriticalSection` pairs

Putting a software breakpoint here causes an immediate crash. Putting a
hardware breakpoint here also crashes when multiple threads are targeted.

### Logic thread

A second thread signals `SetEvent` 60 times/second from `cotm.29df7e`.
Its stack includes `USER32!GetMessage` / `win32u!NtUserGetMessage`,
suggesting it's driven by the Windows message pump. The full stack shows
`user32!DispatchMessageW` calling into `cotm.2a342f` which calls
`cotm.29df7e`.

### Third thread

A third thread signals `SetEvent` 60 times/second from `cotm.2932c5`. Its
immediate caller is `cotm.2a494b`, so it lives on the pacer thread.

### What consumes the 16.7 ms gap

None of `Sleep`, `SleepEx`, `NtDelayExecution`, `QueryPerformanceCounter`,
`WaitForSingleObject` at INFINITE, `NtSetTimer2`, or any DirectX Present
call accounts for the 16.7 ms per frame. The gap is inside COTM.exe code
between the two `SetEvent` sites. We could not pursue this further because
every hook we attempted in that region crashed the process.

**This is the biggest open question for anyone continuing this work.**

---

## Save file layout

`exe/GameData00.bin` … `exe/GameData07.bin` = save slots 1–8.
`exe/SystemData.bin` = mode unlocks + keybinds.

### Critical facts

- The game treats save-slot **file existence** as authoritative. Deleting
  or renaming a `GameData*.bin` while the game is closed causes a
  corruption prompt on next launch. **Always modify save files by
  overwriting (same filename) or leave them alone.**
- The game reads all save files **once at boot**, before any user
  interaction. Loading a save in the menu is purely an in-memory selection.
- The game writes save files on **clean exit** and possibly on level
  transitions. There is no mid-level autosave. A save file does **not**
  encode the player's current position within a stage — only level
  progress, upgrades, and character unlocks.
- Because of the above, **save-file snapshots cannot be used as TAS
  checkpoints.** Re-launching with a "stage 1 start" save puts you at the
  title screen; the game only enters Stage 1 when the player selects it
  from the menu.

### Corrupting a save

If you accidentally delete a save file while the game is closed, the fix
is to close the game and restore `GameData*.bin` from a backup. Do not
launch the game with a missing file — the corruption prompt resets all
data.

---

## Resource formats

`Data/` files are named with the **MD5 hash of the original filename**:
```
hashlib.md5(b"Data/Title.ttb").hexdigest()
-> f0ec225d271b4b35ad770f1c387f4102
```

### Decompiler

The community tool at
[github.com/Giza/inti-Creates-encdec-tool](https://github.com/Giza/inti-Creates-encdec-tool)
decrypts all `Data/` files. Usage:

```
python inti_encdec.py d <filetype> <input_file> <output_file>
```

### Filetype → extension map

| Type | Extension | Content |
|---|---|---|
| `txt` | `.ttb`, `.tb2` | Text resource (zlib-compressed after 4-byte header) |
| `bft` | `.bfb` | BMP font (no `BM` magic — starts with u32 header fields) |
| `obj` | `.osb` | Objects / sprite data |
| `scroll` | `.scb` | Stage background |
| `set` | `.stb` | Stage setup |
| `snd` | `.bisar` | Sound index |
| `json`, `json2` | (config) | Configuration |
| `save1`, `save2`, `save3` | (save data) | `save3` requires SteamID |

### Filename mapping

After decryption, the original filename is found by hashing candidate
names from `COTM.exe` strings. Known matches:

- `f0ec225d271b4b35ad770f1c387f4102` = `Data/Title.ttb`
- `000f0f5e14965b9995cbf6351a2aab3c` = `Data/GraphicText02_en.osb`
- `122d8ea252533a501d9999e2301b2506` = `Data/GameOver.ttb`

Many files (87 out of 383) could not be matched to a string in the
executable — these are likely referenced by numeric ID or built from
concatenated parts at runtime.

---

## Save/replay flow

The rewind mechanism works by:

1. Kill COTM.exe
2. Launch a fresh copy (via `frida.spawn` so we can attach before boot)
3. Wait for the input poll rate to stabilise (see below)
4. Play the prelude (from title screen to first controllable frame)
5. Play the movie up to the target frame
6. Snapshot the state

### Boot detection

The input poll rate is used as a proxy for "the game is ready":

- During the INTI logo and loading screens: 0 polls/second
- Once the title screen is fully loaded: ~5000–8000 polls/second
- During gameplay: same rate

We wait for two consecutive seconds of `poll_rate > 2000` before starting
the prelude. This is implemented via `getPollRate()` / `resetPollCounter()`
RPC calls and `Engine.wait_for_input_ready()`.

Without this wait, the prelude's first inputs (Enter presses) fire during
the INTI logo screen and are consumed by nothing, causing the entire
prelude to desync. Whether this happens depends on disk cache state —
cold cache (slow) usually works, hot cache (fast) usually doesn't.

---

## What didn't work (summary)

In addition to the anti-tamper findings above:

- **Hooking `CreateFileW` on save files at runtime:** no reads happen
  outside of boot. The game reads all saves once and caches them.
- **Full-process memory snapshots (Hourglass-style):** 132 MB heap +
  D3D9 + XAudio2 + DirectInput state written back into the live process
  deadlocks 2 out of 15 threads within seconds. Snapshot/restore itself
  takes <100 ms.
- **Autosave-based checkpoints:** game only writes save files at level
  transitions; there is no mid-level save to anchor on.
- **QueryPerformanceCounter scaling (5×):** speeds up audio playback but
  not game logic — the audio subsystem and game logic use separate time
  bases, and QPC is only the audio one.
- **Sleep/SleepEx/NtDelayExecution zeroing:** the game's 16.7 ms wait is
  not in any Windows API — it's inside COTM.exe code we can't hook.

---

## Open questions for future work

1. **What actually consumes the 16.7 ms per frame?** The pacer thread's
   loop spins on `Sleep(0)` + QPC check. When the time target expires, it
   calls `cotm.293070`. Between `cotm.293070` returning and the next loop
   iteration, the elapsed time should be tiny. Yet each frame takes
   16.7 ms. The time must be consumed inside `cotm.293070` or its
   callees — but every attempt to break there crashes.

2. **How does the game detect debug registers?** No VEH, no UEF, no
   `NtGetContextThread` calls. The detection must happen below the API
   level — a driver, a hardware feature, or a timing check we haven't
   identified.

3. **Can the pacer's timer check be spoofed?** The loop compares QPC
   against a stored target. If we could read/write that target from
   outside the process (via `ReadProcessMemory`/`WriteProcessMemory` on
   a known static address), we might be able to shorten it. We didn't
   locate a suitable static address during this work.

4. **Where is the RNG state?** We never investigated. Likely an LCG or
   Mersenne Twister in the game's data section, seeded from `timeGetTime`
   or `QueryPerformanceCounter` at boot. Finding it would enable seeded
   runs (useful for practice and for verifying determinism against a
   known reference).

5. **Is DirectInput the true input path for gamepads?** The game uses
   `DirectInput8Create` (confirmed by import scan). We never hooked
   `IDirectInputDevice8::GetDeviceState` because keyboard TAS was the
   priority. A future controller-support effort would start there.

---

## Contributing

If you have experience with Windows anti-tamper, kernel-mode debugging,
or game engine internals and want to attack the frame-pacing or
anti-tamper problems:

- Open an issue describing your approach
- The most valuable contribution would be identifying the 16.7 ms
  consumer inside `cotm.293070`
- Second-most valuable: explaining why hardware breakpoints trigger an
  immediate access-violation at `EIP=0x0` when set on the pacer thread

All findings should be reproducible from a fresh install of COTM v1.1.2
and the tools listed at the top of this document.
---

## Update: frame pacing solved (fast-forward)

The "what consumes the 16.7 ms" question above has an answer: **two things
together pace the game, and each alone is enough to hold 60 fps.**

1. The game's own limiter reads `QueryPerformanceCounter` (about 1000 calls/s
   from COTM.exe) and spins on `Sleep(0)` until a frame is due. It never calls
   `timeGetTime` or `GetSystemTimeAsFileTime`.
2. The D3D9 device is created with vsync (`Present` blocks until vblank).

Scaling only QPC (the earlier experiment) changes nothing because vsync still
holds 60 fps; removing only vsync changes nothing because the QPC limiter holds
60 fps. Do both and the game runs as fast as one frame of work takes (~440
frames/s on the dev machine).

How (all in `src/hook.cpp`, nothing touches COTM.exe's `.text`):
- IAT-patch QPC / `timeGetTime` / `GetSystemTimeAsFileTime` / `Sleep` /
  `WaitForSingleObject(Ex)` in COTM.exe so only the game sees scaled time
  (virtual = base + (real - base) * speed, re-based on every speed change).
- IAT-patch `Direct3DCreate9`, then swap `IDirect3D9::CreateDevice` (slot 16 of
  d3d9.dll's vtable) for a wrapper that sets `PresentationInterval` to
  `D3DPRESENT_INTERVAL_IMMEDIATE`.

Determinism check: the 1461-frame prelude ends at the same player state
(HP 12, X 496, Y 1712) at 1x, 4x, 8x, 16x and 50x. The hook returns to 1x on the
exact frame replay ends, so recording from the cursor is always real time.

---

## Update: window focus gates input

The game ignores keyboard input while its window is not the active window, but keeps
running logic and graphics. COTM.exe imports no focus query (`GetForegroundWindow`,
`GetFocus`, `GetActiveWindow` are all absent from USER32), so the state comes from
window messages (`WM_ACTIVATEAPP`, `WM_ACTIVATE`, `WM_KILLFOCUS`, ...) handled in its
window procedure. This also affects injected input: with another window focused, the
scripted `GetAsyncKeyState` results had no effect (player stayed at X 496 instead of
moving to 549.33).

Fix (in `src/hook.cpp`): IAT-patch `CreateWindowExA`, subclass the game's top-level
window with `SetWindowLongA(GWL_WNDPROC)`, force `WM_ACTIVATEAPP`/`WM_NCACTIVATE` to
TRUE and `WM_ACTIVATE` to `WA_ACTIVE`, drop `WM_KILLFOCUS`, and post one synthetic
activation at creation. The game then behaves as always focused, so the hook gates live
(non-injected) keys itself: they are only reported while `GetForegroundWindow()` is the
game window. Verified: with Notepad focused the stepped result is identical (X 549.33,
Y 1748.56), and keys typed into Notepad are not recorded.

---

## RNG

Method: the game's `.text` is decrypted at runtime, so the code was dumped from the running
process and searched for PRNG constants; hardware execute breakpoints (debug registers set
from the host, no code modified) logged calls; the hook logged who reads each clock.

### The gameplay generator: xorshift128

- State: four `uint32` at `*(COTM.exe + 0x48365C) + 0x2F4 / 0x2F8 / 0x2FC / 0x300` (x, y, z, w).
  That global is the same game-manager object the player pointer chain starts from.
- Next number: `exe+0x80280` (`ecx` = manager, arg = range, returns `w % range`); float
  version `exe+0x802E0`. About 100 other sites step the same four words inline.
  `t = x ^ (x << 11); x = y; y = z; z = w; w = w ^ (((w >> 11) ^ t) >> 8) ^ t`.
- Seeding (a script command, `exe+0x2695EC..0x269622`): one 32-bit seed `s` gives
  `x = s * 0x075BCD15, y = s * 0x0165EC15, z = s * 0x0034BF15, w = s * 0x0006F855` (invert
  the multipliers mod 2^32 to recover `s` from a state). The constructor default is
  Marsaglia's 123456789 / 362436069 / 521288629 / 88675123.
- **The seed is the Unix time in seconds at launch**, read through the C runtime's `time()`.
  Seeds recovered from consecutive launches were 1791366638, then +10, +9, +9, +9, +9 (the
  seconds between launches). The runtime looks `GetSystemTimeAsFileTime` and
  `GetSystemTimePreciseAsFileTime` up with `GetProcAddress` at run time, so patching the
  import slots does not reach it; `GetProcAddress` itself is in the import table.
- Item drops: the candle's break handler (`exe+0x7CB10`) looks up a drop table entry and
  calls `exe+0x26CB80`, which builds cumulative weights and calls `exe+0x80280(total)`.
  The result picks the item kind. Pickup (`exe+0x1FDE40..`) adds weapon points through
  `exe+0x1FCCE0`. Weapon points ("Ammo") = byte at `*(*(exe+0x483660)+8)+0x1E` (max at +0x1D).
- Measured: `test3.bscotm` to frame ~3715 gave 18 or 19 weapon points in different launches
  (about 1 launch in 4), depending only on the launch second. Not the cause: wall clock via
  `QueryPerformanceCounter`, process id, heap address, worker-thread overlap, game speed.

### Seeding it from the tool

The movie's `rng_seed` (a Unix time in seconds, or `null`) is written to the shared block
(`Shm::rng_on`, `rng_time`) before the game starts. The hook then:
- patches the exe's `GetProcAddress` import; when the game asks for
  `GetSystemTimeAsFileTime` / `GetSystemTimePreciseAsFileTime` it gets a function that
  returns the frozen seed time, and
- makes the imported `GetSystemTimeAsFileTime` return the same time.

With a seed set, the generator state at frame 1 is identical on every launch and the
outcome repeats (seed 1701088878: 19 weapon points in 4/4 launches; 1701011101: 18 in 4/4).
Time seeds one second apart behave almost identically, so use seeds far apart when searching.

Other ways to interfere without patching code: write the four state words from the hook at a
frame marker (the state is untouched until about frame 700 here), or put a hardware
breakpoint on `exe+0x80280` to log every draw.

### A second engine: Mersenne Twister (unused in stage 1)

- Seed routine `exe+0x2C77B0` (`this` = engine, arg = seed): `state[0]=seed;
  state[i] = 1812433253 * (state[i-1] ^ (state[i-1] >> 30)) + i`.
- Next `exe+0x2C7800`: MT twist with parameters read from the object, tempering
  `y ^= y>>11; y ^= (y<<7) & 0xFF3A58AD; y ^= (y<<15) & 0xFFFFDF8C; y ^= y>>18`. If never
  seeded it seeds itself with 5489.
- Object layout: `+0 n`, `+4 m`, `+8 matrix constant`, `+0xC upper mask`, `+0x10 lower mask`,
  `+0x14 state`, `+0x20 index`; the engine lives at `owner+0xB0`.
- Seed command `exe+0x2C7F00`: value `v <= 31` selects `table[v]` (32 values, 485 .. 31043,
  at `exe+0x39AE38`), otherwise `v` is the seed.
- 19 call sites (`exe+0x2C79xx .. 0x2C8Bxx`, random ranges/colours in script handlers).
  **Zero calls** were observed in all of `test3.bscotm` (4838 frames) and 12000 frames of
  `test.bscotm`, so it does not decide stage 1; later stages were not checked.

## Render-command queue (fast-forward speed-up)

Profiling the fast-forward (sampling the busiest game thread) showed the main thread is
CPU-bound (about 90% of one core, the other threads are idle) and that after the D3D calls were
skipped, about half of its time was still the game preparing its drawing: big `memcpy`s and
per-frame allocations of about 1.6 MB (a 1 MB block every frame, ~6 blocks of 62 KB, ...).

- The game queues drawing commands during a frame and runs them from a loop at `exe+0x29DF10`
  (called from `exe+0x293070`, which is called from the main loop at `exe+0x2A4946`). Each command
  has a type at `cmd+4`; the loop does `push cmd; mov ecx, this; call [exe+0x380D88 + type*4]`
  (36 entries, thiscall, handlers return with `ret 4`) and then calls the command's own cleanup.
- The table is in the data section (pointers into the exe), so pointing its entries at a stub is a
  data write, not a code patch. The hook does this while frames are being skipped
  (`SPEED_NORENDER`, same condition as skipping the D3D draws) and puts the originals back
  for the last 2 frames before the stopping point.
- Not every type can go: type 10 is needed to get past the boot (the game never starts without it);
  types 6, 7, 14, 19, 20, 27 are used rarely (8 - 150 times in 9000 frames) and load resources or
  set persistent device state, skipping them leaves a black or wrong picture at the stopping
  point (14 loads a named resource through the device and is the most expensive). The default skip
  set is the 23 types used about once per frame or more that change neither the game state nor
  the final picture: 1-4, 9, 11, 13, 15-18, 22-26, 28, 30-35 (`CMD_SKIP_DEFAULT` in `hook.cpp`;
  the host can override it with `Shm::cmd_skip_lo/hi`). Types 0, 5, 8 were never used in the
  test movie and 12, 21, 29 only a few dozen times, so they are left alone.
- Result on `test3.bscotm` (9000 frames, max speed): about 1350 -> 1780 frames per second
  (1.3x). The RNG state, health, weapon points and position were identical at 7 checkpoints
  and the picture at the end was pixel-identical. Skipping every type except 10 is faster
  still (about 2000 fps) but the final picture is wrong.
- Limits of the check: only `test3.bscotm` has gameplay (stage 1); other stages may use
  command types this movie never produced.

## RNG draw log

Every draw writes all four state words, so a hardware **write** breakpoint (DR0, 4 bytes, on
`*(COTM.exe+0x48365C)+0x300`, set on every thread of the game from a helper thread, re-set when the
manager object moves and on new threads) traps each draw, including the ~100 inlined copies that a
breakpoint on the draw function would miss. The vectored handler logs frame marker, `eip` (the
instruction after the write), the return address and the new `w` into `Shm::rng_log_buf`; for the draw
function the range is read from `[ebp+8]` and the caller from `[ebp+4]` (write site `exe+0x802CF`), for
the float version from `[esp+4]` (`exe+0x80328`), for inlined copies the first call-preceded address on
the stack is used. It is armed only while the host wants it and within 300 frames of `draw_from`.

Seen on `test3.bscotm` (no baseline, no seed change): draws are rare, a few dozen on a handful of
frames in the first 3700; the item-drop roll is `range 100` called from `exe+0x26CC79`; the draws
around it come from `exe+0xEC...` (spawn/behaviour code, ranges 2-4) and inlined copies at
`exe+0xEC585/0xEC757/0xEC925/0xEC3C9`.

## Launch time: the 3 second join

A rewind restarts the game, so the time to the first frame is paid every time. Measured (`test3.bscotm`,
speed 50x, with a baseline): hook loaded at 0.15-0.4 s, game window at 0.8-1.1 s, first input poll at
1.3-1.6 s and the **first frame marker at 4.3-4.6 s**. The 3 s gap was ours: at every frame marker the hook
joins the worker threads the game started since the previous one (so their loading finishes inside the
frame, see "worker threads" above), with a 3000 ms bound. Before the first marker the game starts 9 threads
(all through the CRT start stub `exe+0x31CC6E`, real entry `exe+0x293340`); 8 finish within about 0.4 s, the
first one never does: it loops on `Sleep(8)` (called from `exe+0x2BDDA4`) for the life of the process, so
the join always ran into its 3000 ms timeout.

Fix: `H_Sleep` counts the sleeps of each thread; the join skips a thread that is still running and has
called `Sleep` at least 3 times (a polling thread: idle, not busy). The thread handles are duplicated with
`THREAD_QUERY_LIMITED_INFORMATION` so the thread id can be read. First frame marker now at 1.3-1.7 s; a
rewind to frame 1000 through the editor went from 6-9 s to 3.5 s. The game state at frames 700, 1500, 3000,
3715, 7000 and 9000 (RNG words, health, weapon points, position) is identical to the earlier measurements,
and *Verify fast-forward* still matches at all 20 checkpoints.

Note: with the old join, in a later session the same harness (baseline restored, the 40-frame stepping of
`comp`, or a direct `RunJob` to frame N) often never got out of the menus (health 0 at frame 3000) while the
new code reached the same states as before every time; the reason for the old flakiness was not found.

---

## Update: savestates that work (branch `savestates`)

The prototype's "running on after a load is unreliable" had a handful of concrete causes, each found with
`tools/sstest.cpp` (save at frame N, run, load, run the same frames again, compare the per-frame values) and
the hook's crash log (`%TEMP%\bscotm_crash.txt`: registers, stack with module names, code bytes at the fault).

1. **The XAudio2 voice table overflowed.** The hook tracks every source voice so a restore can bring back the
   ones the game destroyed after the save. It kept *every* destroyed voice until the next save; a few hundred
   frames of sound effects filled the 1024 entries, and from then on voices were handed out untracked, with the
   game's own callback pointer. After a restore the audio thread called those callbacks into rolled-back memory
   (`XAudio2_7+2CC06`, `OnVoiceProcessingPassStart`). Now only voices that existed at a save are kept.
2. **XAudio2 calls the game's callbacks on its own thread, at any time.** Each voice is given a hook-owned shim
   callback object instead (it forwards while the voice is alive and no restore is under way; `DestroyVoice`
   waits for calls in flight). The restore closes a gate, drains, and only then touches voices.
3. **The game's worker threads were still running when voices were destroyed** for the restore (a stand-in
   `Stop` on a dead voice). They are now parked first.
4. **D3D9 resources were released twice.** The game releases textures/buffers after the save point; the restored
   memory still points at them, and the game releases them again (the crashing code is a destructor calling
   `[obj->vtable+8]`). With savestates on, `Release` of every D3D9 class the game creates is patched: the last
   reference is kept and the game is told the object is gone. The cost is video memory that is not given back.
5. **The game's long-lived thread** (`COTM.exe+31CC6E`) has its registers and stack recorded at a save and put back
   at a load, but only if it is stopped at the same eip/esp (otherwise it is counted, not touched).
   The other threads in the process (Windows thread pool, COM, the AMD driver, `inputhost`) are not game state.

Result on the stage 1 test movie: a save takes about 90 ms, a load 60-80 ms; saves anywhere in the movie
(frames 100 to 11000), gaps of up to 6000 frames, 15 loads in a row from one session: all identical to the
reference run frame by frame. Loading and then playing different inputs survives too (8/8, 1500 frames each).

Memory: a state is a table of 4 KB page indices; pages unchanged since the previous state are shared (the first state is
~81 MB, later ones copy 0.25-7 MB). Over a 74,000-frame movie (the test movie repeated 6 times) with a state every
2000 frames in 8 rotating slots the game stays at about 350 MB and the pool at under 100 MB; before sharing, the 9th
state failed (32-bit address space). GPU dedicated memory stayed at 48 MB on that run.

Limits: the packed `COTM.exe` cannot be disassembled from disk (read the code bytes the crash log dumps); XAudio2
voices' play position and queued buffers are not restored (sound only); D3D memory contents are not restored
(picture only); nothing survives closing the game.

## The stage 1 boss (Memory window: Boss HP)

Goal: show the boss's health. The player values come from fixed pointer chains; the boss does not have one,
so it was found by searching the game's memory during the boss fight of `test3.bscotm` (boss stage from
frame 7601, the TAS drains the weapon points with six sub-weapon casts from frame 8556).

Method (all with `ReadProcessMemory` and one hardware write breakpoint, no game code touched):
1. *Decreasing-value scan.* Sample every aligned dword of the game's private read/write memory (about 150 MB)
   every 5-15 frames from 8545 to 8780 and keep the ones that never increase. One candidate series went
   `120 113 106 99 92 85 78`: six drops of 7 at the six casts; another, `19 16 13 10 7 4 1`, was the weapon
   points. So the boss has 120 HP and a sub-weapon hit costs it 7.
2. *Who writes it.* A write breakpoint on that address (the hook's debug-register code with four slots) fired
   at `exe+0x7D7A9` with `ecx` = the boss object: a setter that stores the new HP. The field is at **+0x3DC of
   the object, max HP at +0x3E0** (120), the same layout as the player (HP at +0x3DC of the player struct).
   The boss object's vtable is `exe+0x39B99C`.
3. The boss's position is not at +0x1AC/+0x1B0 (those floats are 0): the boss is made of several objects
   and its position lives elsewhere. A static pointer chain was not found: the object is referenced from about
   25 places in the heap (list/map nodes), none from the exe's data.

So the editor finds it by search: a background thread scans the heap for a dword equal to a known boss vtable
(`BOSS_VTABLES` in `main.cpp`) whose HP / max HP fields are sane, keeps the address while the object is valid
and scans again when it is gone. A scan takes about 0.2 s. Other bosses have other vtables; to add one, run the
same procedure (step 1 to find the HP series, step 2 to get the object and its vtable) and add the RVA.

Not everything with HP at +0x3DC is an enemy: a generic scan for "vtable object with HP <= max HP at
+0x3DC/+0x3E0" finds hundreds of unrelated objects (UI and effect classes), so a known vtable is required.

### Saving while the game is loading (found after v0.9.0)

A save made while the game is busy (stage assets loading) was restored wrongly in about one run in three at frame 100.
Every failing run had a long "park the game's threads" phase (140-310 ms); every passing run had 0. The hook now
refuses such a save (stopping the threads took more than ~20 ms), the host retries a few times 250 ms apart, and a
thread only counts as stopped when it is in its idle `Sleep(8)` loop (`exe+2BDDAC`), not in an arbitrary wait.
Remaining: about 1 in 24 early-frame (100) saves still restored badly; saves at frames 400 to 11000 passed in all runs
made after the change. Per-phase timings of the last save are in `Shm::snap_time` (shown by `tools/sstest`).

### Replay nondeterminism after the stage 1 boss dies (test3.bscotm, around frame 10550-10600)

Not caused by savestates: continuous fast replays disagree with each other, with savestates off, and with a real-time (1x)
replay. The player is standing still after the boss kill and the stage-clear health refill (9 -> 12) happens at
different frames. In the same 140 frames of two identical runs the game made 16 vs 20 `SubmitSourceBuffer` calls and
received 12 vs 16 buffer callbacks: the game's music streaming is driven by XAudio2 callbacks, which arrive on the audio
thread in real time, so anything in the game that waits for audio events lands on a different frame each run. The
counters are in `Shm::px_cnt` / `Shm::sh_cnt` (`tools/sstest --cntfrom A --cntto B`). A fix would have to deliver the
callbacks on a frame schedule instead of by the audio thread (a virtual audio clock).
