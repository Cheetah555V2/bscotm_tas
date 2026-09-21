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
