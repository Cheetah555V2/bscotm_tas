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