"""
Orchestration layer.

Ties together:
  - COTMHook (input recording / injection)
  - Memory reading (for determinism checks and live status)
  - Movie file I/O
  - Game process lifecycle (launch / kill / restart)

The rewind strategy for v0.1 is "restart and replay":
    rewind_to(N):
        kill COTM.exe
        launch COTM.exe
        wait for it to be ready
        play movie[1..N] at max speed (Sleep hook enabled)
        pause (stop play)
    After this, the game is at frame N and the user can edit the movie.
"""

from __future__ import annotations

import ctypes
import os
import struct
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path
from typing import Callable

import frida

from .constants import (
    TARGET_PROCESS,
    PLAYER_CHAIN_ROOT, PLAYER_CHAIN_OFFSETS,
    PLAYER_OFF_HEALTH, PLAYER_OFF_X, PLAYER_OFF_Y,
    PLAYER_OFF_XVEL, PLAYER_OFF_YVEL,
    REPLAY_SETTLE_FRAMES,
)
from .frida_hook import COTMHook
from .movie import Movie, _now_utc
from .saves import default_save_manager


# ---------------------------------------------------------------------------
# Process helpers (Win32)
# ---------------------------------------------------------------------------

_k32  = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi",   use_last_error=True)

_PROCESS_VM_READ           = 0x0010
_PROCESS_QUERY_INFORMATION = 0x0400

class _PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize",             wintypes.DWORD),
        ("cntUsage",           wintypes.DWORD),
        ("th32ProcessID",      wintypes.DWORD),
        ("th32DefaultHeapID",  ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID",       wintypes.DWORD),
        ("cntThreads",         wintypes.DWORD),
        ("th32ParentProcessID",wintypes.DWORD),
        ("pcPriClassBase",     ctypes.c_long),
        ("dwFlags",            wintypes.DWORD),
        ("szExeFile",          ctypes.c_char * 260),
    ]

class _MODULEINFO(ctypes.Structure):
    _fields_ = [
        ("lpBaseOfDll", ctypes.c_void_p),
        ("SizeOfImage", wintypes.DWORD),
        ("EntryPoint",  ctypes.c_void_p),
    ]


def find_pid(name: str = TARGET_PROCESS) -> int | None:
    snap = _k32.CreateToolhelp32Snapshot(0x2, 0)
    e = _PROCESSENTRY32(); e.dwSize = ctypes.sizeof(_PROCESSENTRY32)
    try:
        if not _k32.Process32First(snap, ctypes.byref(e)):
            return None
        while True:
            if e.szExeFile.decode(errors="replace").lower() == name.lower():
                return e.th32ProcessID
            if not _k32.Process32Next(snap, ctypes.byref(e)):
                return None
    finally:
        _k32.CloseHandle(snap)


def get_module_base(pid: int, mod: str = TARGET_PROCESS) -> int | None:
    h = _k32.OpenProcess(_PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ, False, pid)
    if not h:
        return None
    try:
        mods = (wintypes.HMODULE * 1024)()
        need = wintypes.DWORD()
        if not _psapi.EnumProcessModulesEx(h, ctypes.byref(mods),
                                           ctypes.sizeof(mods),
                                           ctypes.byref(need), 0x03):
            return None
        n = need.value // ctypes.sizeof(wintypes.HMODULE)
        for i in range(n):
            buf = ctypes.create_string_buffer(260)
            if _psapi.GetModuleBaseNameA(h, mods[i], buf, 260):
                if buf.value.decode(errors="replace").lower() == mod.lower():
                    info = _MODULEINFO()
                    _psapi.GetModuleInformation(h, mods[i], ctypes.byref(info),
                                                ctypes.sizeof(info))
                    return info.lpBaseOfDll
    finally:
        _k32.CloseHandle(h)
    return None


# ---------------------------------------------------------------------------
# Minimal memory reader for status / determinism checks
# ---------------------------------------------------------------------------

class Mem:
    def __init__(self, pid: int):
        self.pid = pid
        self.h = _k32.OpenProcess(0x1F0FFF, False, pid)
        if not self.h:
            raise RuntimeError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")

    def close(self):
        if self.h:
            _k32.CloseHandle(self.h)
            self.h = None

    def _read(self, addr: int, size: int) -> bytes | None:
        if addr < 0 or addr > 0xFFFFFFFF:
            return None
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        ok = _k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr),
                                    buf, size, ctypes.byref(got))
        if not ok or got.value != size:
            return None
        return buf.raw

    def u32(self, addr): 
        d = self._read(addr, 4); return None if d is None else struct.unpack("<I", d)[0]
    def u8(self, addr):
        d = self._read(addr, 1); return None if d is None else d[0]
    def f32(self, addr):
        d = self._read(addr, 4); return None if d is None else struct.unpack("<f", d)[0]

    def resolve_player_struct(self, module_base: int) -> int | None:
        p = self.u32(module_base + PLAYER_CHAIN_ROOT)
        if not p: return None
        rev = list(reversed(PLAYER_CHAIN_OFFSETS))
        for off in rev[:-1]:
            p = self.u32((p + off) & 0xFFFFFFFF)
            if not p: return None
        return p            # <-- was: (p + rev[-1]) & 0xFFFFFFFF

    def snapshot(self, module_base: int) -> dict:
        """Read a compact set of state fields for comparison / status."""
        out = {"module_base": module_base}
        ps = self.resolve_player_struct(module_base)
        out["player_struct"] = ps
        if ps:
            out["health"] = self.u8(ps + PLAYER_OFF_HEALTH)
            out["x"]      = self.f32(ps + PLAYER_OFF_X)
            out["y"]      = self.f32(ps + PLAYER_OFF_Y)
            out["xvel"]   = self.f32(ps + PLAYER_OFF_XVEL)
            out["yvel"]   = self.f32(ps + PLAYER_OFF_YVEL)
        return out


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------

class Engine:
    def __init__(self, exe_path: str | Path, backup_dir: str | Path = "save_backups"):
        self.exe_path = Path(exe_path)
        self.saves = default_save_manager(self.exe_path, backup_dir)
        self.hook: COTMHook | None = None
        self.mem:  Mem | None = None
        self.pid:  int | None = None
        self.module_base: int | None = None
        self._spawn_pid = None

    # ---- lifecycle --------------------------------------------------------
    def is_game_running(self) -> bool:
        return find_pid() is not None

    def launch_game(self, wait_seconds: float = 30.0):
        if self.is_game_running():
            raise RuntimeError("COTM.exe is already running.")
        if not self.exe_path.exists():
            raise FileNotFoundError(self.exe_path)
        cwd = self.exe_path.parent
        try:
            self._spawn_pid = frida.spawn([str(self.exe_path)], cwd=str(cwd))
            print(f"[+] Spawned COTM.exe (paused) pid={self._spawn_pid}")
        except Exception as e:
            print(f"[!] frida.spawn failed ({e}); falling back to Popen")
            self._spawn_pid = None
            subprocess.Popen([str(self.exe_path)], cwd=str(cwd))
            deadline = time.time() + wait_seconds
            while time.time() < deadline:
                if find_pid():
                    time.sleep(1.0)
                    return
                time.sleep(0.2)
            raise TimeoutError("COTM.exe did not appear within timeout.")

    def kill_game(self):
        # Politely ask then force.
        subprocess.run(["taskkill", "/IM", TARGET_PROCESS, "/T", "/F"],
                       capture_output=True)
        # Wait until the process is gone.
        for _ in range(50):
            if not self.is_game_running():
                return
            time.sleep(0.1)
        raise TimeoutError("Could not kill COTM.exe.")

    def attach(self):
        if self._spawn_pid is not None:
            pid = self._spawn_pid
            pending_resume = True
        else:
            pid = find_pid()
            pending_resume = False
        if pid is None:
            raise RuntimeError("COTM.exe is not running.")
        self.pid = pid
        self.hook = COTMHook(pid=pid)
        if not pending_resume:
            # Non-spawned: process is already running, set up memory now.
            self.module_base = get_module_base(pid)
            self.mem = Mem(pid)
        else:
            # Spawned: mem reader is set up in resume() after the game has
            # initialized and unpacked its code.
            self.mem = None
            self.module_base = None

    def detach(self):
        if self.hook:
            self.hook.detach(); self.hook = None
        if self.mem:
            self.mem.close();   self.mem = None
        self.pid = None

    # ---- recording --------------------------------------------------------
    def record(self,
               output_path: str | Path,
               should_stop: Callable[[], bool],
               on_progress: Callable[[int], None] | None = None):
        """
        Record until `should_stop()` returns True. Saves to output_path.
        """
        if not self.hook:
            raise RuntimeError("Not attached.")

        # Collect sparse frame dicts as they arrive.
        frames: dict[int, dict] = {}

        def _on_rec_frame(idx: int, keys: dict):
            if keys:
                frames[idx] = keys

        self.hook._on_record_frame = _on_rec_frame  # rebind
        self.hook.start_record()

        last_reported = -1
        while not should_stop():
            time.sleep(0.2)
            st = self.hook.stats()
            n = st.get("recorded", 0)
            if on_progress and n != last_reported and n % 60 == 0:
                last_reported = n
                on_progress(n)

        info = self.hook.stop_record()
        total = info["total"]

        # Build sparse list with monotonic frame indices (0 .. total-1).
        sparse = []
        prev = {}
        for i in range(total):
            here = frames.get(i, {})
            if here != prev:
                # Encode deltas: keys that turned off get value 0.
                delta = {}
                for k in prev:
                    if k not in here:
                        delta[k] = 0
                for k in here:
                    delta[k] = 1
                sparse.append(delta)
                prev = here
            else:
                sparse.append({})

        movie = Movie(
            author=os.environ.get("USERNAME", "unknown"),
            created_utc=_now_utc(),
            rng_seed=None,
            anchors=[],
            frames=sparse,
        )
        movie.save(output_path)
        return movie

    # ---- playback ---------------------------------------------------------
    def play(self,
             movie: Movie,
             prelude: Movie | None = None,
             speedup: bool = True,
             stop_frame: int | None = None,
             on_tick: Callable[[int], None] | None = None,
             should_stop: Callable[[], bool] | None = None):
        if not self.hook:
            raise RuntimeError("Not attached.")

        combined: list[dict] = []
        prelude_len = 0
        if prelude and prelude.total_frames > 0:
            combined.extend(prelude.expand())
            prelude_len = len(combined)
        combined.extend(movie.expand())
        total = len(combined)

        schedule = {i + 1: _keys_to_vk_dec(f) for i, f in enumerate(combined)}

        combined_stop = total
        if stop_frame is not None:
            combined_stop = prelude_len + stop_frame

        self.hook.start_play(schedule, total, stop_at=combined_stop)
        if speedup:
            self.hook.set_speedup(True)

        last = -1
        while True:
            if should_stop and should_stop():
                break
            if self.hook._play_end_event.is_set():
                break
            st = self.hook.stats()
            f = st.get("frame", 0)
            movie_f = f - prelude_len
            if on_tick and f != last and movie_f >= 1:
                last = f
                on_tick(movie_f)
            if st.get("mode") != "play":
                # JS-side stop, or something went wrong.
                break
            time.sleep(0.03)

        self.hook.stop_play()
        return self.hook.get_last_snapshot() or {}

    # ---- rewind -----------------------------------------------------------
    def rewind_to(self,
                  movie: Movie,
                  target_frame: int,
                  prelude: Movie | None = None,
                  known_save_state: str | Path | None = None,
                  on_progress: Callable[[int, int], None] | None = None):
        if self.is_game_running():
            self.detach()
            self.kill_game()

        if known_save_state is not None:
            self.saves.restore(known_save_state)
        prelude_to_use = prelude

        with self.saves.snapshot_and_restore(label="pre_rewind"):
            self.launch_game()
            self.attach()
            self.resume()
            try:
                snapshot = self.play(
                    movie,
                    prelude=prelude_to_use,
                    speedup=True,
                    stop_frame=target_frame,
                    on_tick=(lambda f: on_progress(f, target_frame))
                            if on_progress else None,
                )
            finally:
                try: self.detach()
                except Exception: pass
                try: self.kill_game()
                except Exception: pass
        return snapshot or {}

    def resume(self):
        """Resume a spawned-and-paused game. No-op if launched via Popen."""
        if self._spawn_pid is None:
            return
        frida.resume(self._spawn_pid)
        # Wait for the exe module and d3d9 to load.
        time.sleep(1.5)
        self.module_base = get_module_base(self.pid)
        self.mem = Mem(self.pid)

    def resume_record(self,
                      movie: Movie,
                      prelude: Movie | None,
                      cursor_frame: int,
                      on_progress: Callable[[int], None] | None = None,
                      should_stop: Callable[[], bool] | None = None,
                      on_record_frame: Callable[[int, dict], None] | None = None):
        """
        Restore baseline, launch, play prelude + movie up to cursor_frame,
        then seamlessly transition into record mode.

        Returns the final recorded frame count.
        """
        if self.is_game_running():
            self.detach()
            self.kill_game()

        # Prepare combined schedule.
        combined: list[dict] = []
        prelude_len = 0
        if prelude and prelude.total_frames > 0:
            combined.extend(prelude.expand())
            prelude_len = len(combined)
        combined.extend(movie.expand())
        total = len(combined)
        schedule = {i + 1: _keys_to_vk_dec(f) for i, f in enumerate(combined)}
        combined_stop = prelude_len + cursor_frame

        self.launch_game()
        self.attach()
        self.resume()

        # Install record callback *before* transitioning.
        if on_record_frame:
            self.hook._on_record_frame = on_record_frame

        self.hook.start_play_then_record(
            schedule, total, stop_at=combined_stop,
            resume_from=cursor_frame,
        )
        self.hook.set_speedup(True)

        # Wait for the transition to record mode, then let the user play.
        self.hook._resume_event.wait(timeout=120.0)

        # Now recording. Poll progress until user stops.
        last_n = cursor_frame
        while True:
            if should_stop and should_stop():
                break
            st = self.hook.stats()
            n = st.get("recorded", 0)
            if on_progress and n != last_n:
                last_n = n
                on_progress(n)
            time.sleep(0.1)

        info = self.hook.stop_record()
        return info

    def snapshot_status(self) -> dict:
        """Read a small set of game state fields for comparison and display."""
        if not self.mem or self.module_base is None:
            return {}
        return self.mem.snapshot(self.module_base)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _keys_to_vk_dec(frame: dict) -> dict[int, int]:
    """Frames from Movie.expand() are already {vk_decimal(int): 1}."""
    return {int(k): 1 for k, v in frame.items() if v}