# Reverse-Engineering Notes

Everything below was discovered by inspecting the Steam PC build of
Bloodstained: Curse of the Moon v1.1.2 (Steam AppID 838310) through
Frida, Cheat Engine, and static analysis of `COTM.exe`.

## Process facts

- 32-bit (Win32) executable, ASLR enabled (loads at a random base each
  run — never hardcode `0x400000`).
- Custom in-house engine ("Ice" task system, class names prefixed
  `IceCo*`, `IceTask`, `IceContext`, `cBs03`).
- Steamworks integration via `steam_api.dll` (Steam overlay injects
  `gameoverlayrenderer.dll` into the process — beware, it also calls
  into user32/GetAsyncKeyState and pollutes naive hooks).

## Anti-tamper

`COTM.exe` **verifies its own `.text` section integrity.** Any hook placed
inside the exe's code (`Interceptor.attach` on an address in `COTM.exe`,
or CE's `INT3` breakpoints without ScyllaHide) causes an immediate crash.
Hardware breakpoints and IAT patches (writing to a `.data`/`.idata`
pointer) are safe.

## Input system

- The game polls **81 virtual keys** every frame via
  `user32!GetAsyncKeyState`. All polls come from a single call site,
  `COTM.exe+0x2a5960`. Filter hooks by this return address.
- The VK list is a fixed 81-entry DWORD table at
  `module_base + 0x3d1760`.
- The game maintains two parallel 81-byte arrays:
  - `STATE_CUR[i] = GetAsyncKeyState(VK_TABLE[i]) >> 15` (this frame)
  - `STATE_PREV[i] = previous frame's STATE_CUR[i]` (for edge detection)
  - `STATE_CUR` base is at some heap address `S`; `STATE_PREV` is at
    `S - 0x80`.
- **The game has no separate "action flag" byte.** It reads the state
  array directly from game logic each frame. This is why hooking at the
  `GetAsyncKeyState` return works so cleanly — there's no intermediate
  buffer to chase.

## Player struct

Pointer chain (offsets are **xml-order, deepest first**, and must be
applied **reversed**, with the last offset added rather than
dereferenced):

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
| `+0x1AC` | f32 | X position |
| `+0x1B0` | f32 | Y position |
| `+0x3DC` | u8  | Health |
| `+0x53C` | f32 | Invisibility |
| `+0x5B8` | f32 | Render X (snaps to ±8 of physics X) |
| `+0x5BC` | f32 | Render Y |

## Frame pacing (unsolved)

The game holds 60 fps through a mechanism we could not identify. We
verified experimentally that it is **not**:

- `Sleep` / `SleepEx` / `NtDelayExecution` (zeroing these has no effect)
- `QueryPerformanceCounter` (scaling it 5× speeds up audio but not logic)
- `IDirect3DDevice9::Present` (not called by COTM — the swap chain is
  created via `Direct3DCreate9Ex` and we never observed a Present)
- `IDXGISwapChain::Present` (D3D11 never initialised by COTM)
- `NtSetTimer2` (only ~3 calls per run, all one-shot 100 ms timers)
- `SetEvent` on either of the two known INFINITE waiters (they fire
  60×/s but the 16 ms gap is upstream)

The 16.7 ms gap is consumed inside the game's own code between the
`SetEvent` signaller and the `WaitForSingleObject` waiter. We did not
pursue this further because hooking `COTM.exe` code trips the
anti-tamper check.

## Save file layout

`exe/GameData00.bin` … `GameData07.bin` = save slots 1–8.
`exe/SystemData.bin` = mode unlocks + keybinds.

The game treats save-slot file *existence* as authoritative:
deleting or renaming a `GameData*.bin` while the game is closed causes
a corruption prompt on next launch. **Always modify save files by
overwriting (same filename) or leave them alone.**

The game reads all save files once at boot, before any user interaction.
Loading a save in the menu is purely an in-memory selection.

## Resource formats

- `Data/` files are named with the **MD5 of the original filename**
  (`hashlib.md5(b"Data/Title.ttb").hexdigest()`).
- Encryption tool: https://github.com/Giza/inti-Creates-encdec-tool
- Filetype → extension mapping:
  - `.ttb`, `.tb2` → `txt` / `txt2` (TTB text resources)
  - `.osb` → `obj` (objects)
  - `.bfb` → `bft` (BMP fonts)
  - `.scb` → `scroll`
  - `.stb` → `set`
  - `.bisar` → `snd`
  - `.json` / `.json2` → `json` / `json2`

## What didn't work

- Hooking `CreateFileW` on save files at runtime: no reads happen
  outside of boot.
- Full-process memory snapshots (Hourglass-style): restoring 130 MB of
  heap + D3D9 + XAudio2 + DirectInput state into the live process
  reliably deadlocks. Save/restore ran in <100 ms but the game froze
  within seconds.
- Autosave-based checkpoints: the game only writes save files at level
  transitions; there is no mid-level save to anchor on.