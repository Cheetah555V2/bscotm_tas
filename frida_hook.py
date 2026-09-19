"""
Frida hook layer.

Attaches to COTM.exe, installs a hook on user32!GetAsyncKeyState, and
exposes an RPC interface for record / play / idle, plus optional Sleep
speedup for fast replay.

We NEVER hook addresses inside COTM.exe — the game has an anti-tamper
check that crashes when we do. All hooks live in system DLLs.
"""

from __future__ import annotations

import json
import time
from typing import Callable
import threading

import frida

from .constants import (
    TARGET_PROCESS,
    POLL_RET_OFFSET,
    VK_TABLE_OFFSET,
    TRACKED_KEYS,
)

VERBOSE = False

# ---------------------------------------------------------------------------
# JS payload (loaded into the game process)
# ---------------------------------------------------------------------------

_FRIDA_JS = r"""
var base     = Process.getModuleByName("COTM.exe").base;
var POLL_RET = base.add(POLL_RET_OFFSET_PLACEHOLDER);
var gks      = Process.getModuleByName("user32.dll")
                      .getExportByName("GetAsyncKeyState");

var TRACKED = TRACKED_VKS_PLACEHOLDER;

var mode      = "idle";
var speedup   = false;
var frame     = 0;
var cur_rec   = {};
var schedule  = {};
var movie     = {};
var rec_count = 0;
var play_total = 0;
var stop_at    = 0;
var play_end_sent = false;
var inject_count = 0;
var record_after_play = false;
var resume_from = 0;

// ---------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------
function u32(a) { try { return a.readU32(); } catch (e) { return null; } }

function read_player_state() {
    try {
        var p = u32(base.add(0x0048365C));
        if (!p) return null;
        var offs = [0x08, 0x6C, 0x20, 0x20, 0x08, 0x84];
        for (var i = 0; i < offs.length; i++) {
            p = u32(ptr(p).add(offs[i]));
            if (!p) return null;
        }
        var ps = ptr(p);
        return {
            player_struct: ps.toInt32(),
            health: ps.add(0x3DC).readU8(),
            x:      ps.add(0x1AC).readFloat(),
            y:      ps.add(0x1B0).readFloat(),
            xvel:   ps.add(0x1A0).readFloat(),
            yvel:   ps.add(0x1A4).readFloat(),
        };
    } catch (e) { return null; }
}

// ---------------------------------------------------------------------
// Optional: bypass Sleep(8) inside the game's pacer via the IAT slot.
// Not required for correctness. Slightly reduces CPU usage during
// playback but does not speed up wall-clock frame rate.
// ---------------------------------------------------------------------
var sleep_iat_slot = null;
var sleep_real_fn  = null;
var sleep_stub     = null;
var sleep_iat_patched = false;

(function prepareStub() {
    sleep_stub = Memory.alloc(16);
    Memory.protect(sleep_stub, 16, "rwx");
    sleep_stub.writeByteArray([0xC2, 0x04, 0x00]);   // stdcall ret 4
})();

function locateSleepIatNow() {
    if (sleep_iat_slot) return true;
    var retAddr = base.add(0x2bddac);
    for (var back = 6; back <= 16; back++) {
        var p = retAddr.sub(back);
        var b0, b1;
        try { b0 = p.readU8(); b1 = p.add(1).readU8(); }
        catch (e) { continue; }
        if (b0 !== 0xFF || b1 !== 0x15) continue;
        var slot_abs;
        try { slot_abs = p.add(2).readU32(); } catch (e) { continue; }
        var slot = ptr(slot_abs);
        var real_fn;
        try { real_fn = slot.readPointer(); } catch (e) { continue; }
        if (!real_fn || real_fn.isNull()) continue;
        var m = Process.findModuleByAddress(real_fn);
        if (!m) continue;
        var mn = m.name.toLowerCase();
        if (mn.indexOf("kernel") < 0 && mn.indexOf("ntdll") < 0) continue;
        sleep_iat_slot = slot;
        sleep_real_fn  = real_fn;
        if (VERBOSE) send({type: "log", msg: "[iat] Sleep slot=" + slot +
              " real=" + real_fn + " stub=" + sleep_stub});
        return true;
    }
    return false;
}

function setSleepIatPatched(on) {
    if (on && !sleep_iat_slot) locateSleepIatNow();
    if (!sleep_iat_slot) return false;
    try {
        Memory.protect(sleep_iat_slot, 4, "rwx");
        if (on && !sleep_iat_patched) {
            sleep_iat_slot.writePointer(sleep_stub);
            sleep_iat_patched = true;
            if (VERBOSE) send({type: "log", msg: "[iat] Sleep -> stub (ON)"});
        } else if (!on && sleep_iat_patched) {
            sleep_iat_slot.writePointer(sleep_real_fn);
            sleep_iat_patched = false;
            if (VERBOSE) send({type: "log", msg: "[iat] Sleep -> real (OFF)"});
        }
        return true;
    } catch (e) { return false; }
}

// ---------------------------------------------------------------------
// Input hook
// ---------------------------------------------------------------------
Interceptor.attach(gks, {
    onEnter: function (args) {
        this.vk = args[0].toInt32();
        try { this.ret = this.context.esp.readPointer(); }
        catch (e) { this.ret = null; }
    },
    onLeave: function (retval) {
        if (!this.ret || !this.ret.equals(POLL_RET)) return;

        if (this.vk === 0) {
            if (mode === "record") {
                if (Object.keys(cur_rec).length > 0) {
                    send({ type: "rec_frame", frame: rec_count, keys: cur_rec });
                    rec_count++;
                } else {
                    if (rec_count % 60 === 0) {
                        send({ type: "rec_blank", frame: rec_count });
                    }
                    rec_count++;
                }
                cur_rec = {};
            }
            frame++;

            if (mode === "play") {
                if (frame > stop_at) {
                    if (record_after_play) {
                        mode = "record";
                        rec_count = resume_from + 1;
                        cur_rec = {};
                        record_after_play = false;
                        speedup = false;
                        setSleepIatPatched(false);
                        send({ type: "resume_recording",
                               frame: frame, rec_count: rec_count });
                        return;
                    }
                    if (!play_end_sent) {
                        play_end_sent = true;
                        var snap = read_player_state();
                        send({ type: "play_end", frame: frame, snapshot: snap });
                        mode = "idle";
                        speedup = false;
                        setSleepIatPatched(false);
                    }
                    return;
                }
                schedule = movie[frame] || {};
            }
            return;
        }

        var vk_dec = this.vk.toString();
        if (TRACKED.indexOf(vk_dec) < 0) return;

        if (mode === "record") {
            var held = (retval.toInt32() & 0x8000) !== 0;
            if (held) cur_rec[vk_dec] = 1;
        } else if (mode === "play") {
            var want = schedule[vk_dec] ? 0x8000 : 0x0000;
            retval.replace(want);
            inject_count++;
        }
    }
});

// ---------------------------------------------------------------------
// RPC
// ---------------------------------------------------------------------
rpc.exports = {
    setmode: function (m) { mode = m; return mode; },

    startrecord: function () {
        movie = {}; cur_rec = {}; schedule = {};
        frame = 0; rec_count = 0; inject_count = 0;
        speedup = false; setSleepIatPatched(false);
        mode = "record";
        return "recording";
    },

    stoprecord: function () {
        if (Object.keys(cur_rec).length > 0) {
            send({ type: "rec_frame", frame: rec_count, keys: cur_rec });
            rec_count++;
            cur_rec = {};
        }
        mode = "idle";
        return { total: rec_count, injected: inject_count };
    },

    startplay: function (sched, total, stop_at_frame) {
        movie = sched; play_total = total;
        stop_at = (stop_at_frame == null) ? total : stop_at_frame;
        schedule = movie[1] || {};
        frame = 0; inject_count = 0;
        play_end_sent = false; record_after_play = false;
        speedup = true;
        setSleepIatPatched(true);
        mode = "play";
        return { total: total, stop_at: stop_at };
    },

    stopplay: function () {
        mode = "idle"; speedup = false;
        setSleepIatPatched(false);
        return { injected: inject_count };
    },

    startPlayThenRecord: function (sched, total, stop_at_frame, resume_frame) {
        movie = sched; play_total = total;
        stop_at = (stop_at_frame == null) ? total : stop_at_frame;
        resume_from = resume_frame || 0;
        schedule = movie[1] || {};
        frame = 0; inject_count = 0; rec_count = 0; cur_rec = {};
        play_end_sent = false; record_after_play = true;
        speedup = true;
        setSleepIatPatched(true);
        mode = "play";
        return { total: total, stop_at: stop_at, resume_from: resume_from };
    },

    setSpeedup: function (on) {
        speedup = !!on;
        setSleepIatPatched(on);
        return speedup;
    },

    stats: function () {
        return {
            mode: mode, frame: frame, speedup: speedup,
            recorded: rec_count, movie_frames: play_total,
            inject_count: inject_count, stop_at: stop_at,
            sleep_iat_patched: sleep_iat_patched,
        };
    },

    resolvePlayerStruct: function () {
        var p = u32(base.add(0x0048365C));
        if (!p) return null;
        var offs = [0x08, 0x6C, 0x20, 0x20, 0x08, 0x84];
        for (var i = 0; i < offs.length; i++) {
            p = u32(ptr(p).add(offs[i]));
            if (!p) return null;
        }
        return ptr(p);
    },
};

send({ type: "ready", base: base.toString(),
       poll_ret: POLL_RET.toString(),
       vk_table: base.add(VK_TABLE_OFFSET_PLACEHOLDER).toString() });
"""


def _build_js() -> str:
    tracked_decs = [dec for (_vk, dec) in TRACKED_KEYS.values()]
    return (
        _FRIDA_JS
        .replace("POLL_RET_OFFSET_PLACEHOLDER", hex(POLL_RET_OFFSET))
        .replace("VK_TABLE_OFFSET_PLACEHOLDER", hex(VK_TABLE_OFFSET))
        .replace("TRACKED_VKS_PLACEHOLDER", json.dumps(tracked_decs))
        .replace("VERBOSE_PLACEHOLDER", "true" if VERBOSE else "false")
    )


# ---------------------------------------------------------------------------
# Python wrapper
# ---------------------------------------------------------------------------

class COTMHook:
    """
    Manages the Frida session and the hook's RPC.

    Lifecycle:
        hook = COTMHook()               # attaches
        hook.start_record()             # begin recording
        frames = hook.stop_record()     # -> list[dict] (sparse)
        hook.start_play(schedule, n)    # begin playback
        hook.stop_play()
        hook.detach()
    """

    def __init__(self,
                 pid: int | None = None,
                 on_record_frame: Callable[[int, dict], None] | None = None,
                 on_play_end:     Callable[[int], None] | None = None):
        self._on_record_frame = on_record_frame
        self._on_play_end     = on_play_end
        self._rec_frames: list[dict] = []
        self._rec_last_frame_index = -1
        self._ready = False
        self._last_stats: dict = {}
        self._play_end_event = threading.Event()
        self._play_end_snapshot = None
        self._play_end_frame = None
        self._resume_event = threading.Event()

        try:
            if pid is not None:
                self.session = frida.attach(pid)
            else:
                self.session = frida.attach(TARGET_PROCESS)
        except frida.ProcessNotFoundError:
            raise RuntimeError(f"{TARGET_PROCESS} is not running.")

        self.script = self.session.create_script(_build_js())
        self.script.on("message", self._on_message)
        self.script.load()

        # Wait for the ready signal so we know hooks are installed.
        deadline = time.time() + 5.0
        while not self._ready and time.time() < deadline:
            time.sleep(0.05)
        if not self._ready:
            raise RuntimeError("Frida script did not report ready within 5s.")

    # ---- internal ---------------------------------------------------------
    def _on_message(self, msg, _data):
        # Frida "log" channel: console.log() from JS
        if msg["type"] == "log":
            print(f"[js] {msg.get('payload', '')}")
            return
        if msg["type"] == "error":
            print("[frida] JS error:", msg.get("description"))
            return
        if msg["type"] != "send":
            return

        p = msg["payload"]
        if not isinstance(p, dict):
            print(f"[js?] {p}")
            return

        t = p.get("type")

        if t == "log":
            print(f"[js] {p.get('msg', '')}")
            return

        if t == "ready":
            self._ready = True
        elif t == "rec_frame":
            idx = int(p["frame"])
            keys = {int(k): v for k, v in p["keys"].items()}
            self._rec_last_frame_index = idx
            if self._on_record_frame:
                self._on_record_frame(idx, keys)
        elif t == "rec_blank":
            self._rec_last_frame_index = int(p["frame"])
        elif t == "play_end":
            self._play_end_frame = int(p.get("frame", 0))
            self._play_end_snapshot = p.get("snapshot")
            self._play_end_event.set()
            if self._on_play_end:
                self._on_play_end(self._play_end_frame)
        elif t == "resume_recording":
            print(f"[frida] resume_recording at frame {p.get('frame')}, "
                  f"rec_count={p.get('rec_count')}")
            self._resume_event.set()

    def start_play_then_record(self, schedule: dict, total_frames: int,
                               stop_at: int, resume_from: int):
        self._resume_event.clear()
        self._play_end_event.clear()
        self._play_end_snapshot = None
        self._play_end_frame = None
        sched = {str(f): {str(k): 1 for k in v} for f, v in schedule.items()}
        return self.script.exports_sync.startPlayThenRecord(
            sched, total_frames, stop_at, resume_from)

    # ---- mode control -----------------------------------------------------
    def start_record(self):
        self._rec_frames = []
        self._rec_last_frame_index = -1
        return self.script.exports_sync.startrecord()

    def stop_record(self) -> dict:
        """Return sparse frames dict {frame_index: {vk_dec: 1}}."""
        info = self.script.exports_sync.stoprecord()
        return {"total": info["total"], "injected": info["injected"]}

    def start_play(self, schedule, total_frames, stop_at=None):
        self._play_end_event.clear()
        self._play_end_snapshot = None
        self._play_end_frame = None
        sched = {str(f): {str(k): 1 for k in v} for f, v in schedule.items()}
        return self.script.exports_sync.startplay(sched, total_frames, stop_at)

    def get_last_snapshot(self):
        return self._play_end_snapshot

    def stop_play(self):
        return self.script.exports_sync.stopplay()

    def set_speedup(self, on: bool):
        return self.script.exports_sync.setSpeedup(on)

    def stats(self) -> dict:
        self._last_stats = self.script.exports_sync.stats()
        return self._last_stats

    def resolve_player_struct(self):
        return self.script.exports_sync.resolvePlayerStruct()

    def detach(self):
        try:
            self.script.exports_sync.stopplay()
        except Exception:
            pass
        try:
            self.session.detach()
        except Exception:
            pass