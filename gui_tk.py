"""
Tkinter GUI for bscotm-tas.

Single-file implementation. Uses ttk.Treeview for the frame grid.
Threading is done with queue.Queue + root.after polling (the standard
tkinter pattern — tkinter itself is single-threaded).

Layout:
  [toolbar]  Open / Save / Record / Play / Stop / Rewind | Baseline | Prelude | Frame: N/M
  [grid]     # | L R U Dn | A D W S | Jmp Atk Sub | Q E | P Ent Esc
  [status]   message                              HP X Y XV YV
"""

import queue
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from .constants import TRACKED_KEYS, VK_DEC_TO_NAME
from .engine import Engine
from .movie import Movie
from .prelude import load_prelude


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEFAULT_EXE           = Path("Bloodstained Curse of the Moon/exe/COTM.exe")
DEFAULT_BASELINE_DIR  = Path("save_backups/baselines")
DEFAULT_PRELUDE_DIR   = Path("preludes")

# UI column order (actions). Must be a subset of TRACKED_KEYS.
COLUMN_ORDER = [
    "LEFT", "RIGHT", "UP", "DOWN",
    "A", "D", "W", "S",
    "SPACE", "LMB", "RMB",
    "Q", "E",
    "P", "ENTER", "ESC",
]

HEADER_LABELS = {
    "LEFT": "L", "RIGHT": "R", "UP": "U", "DOWN": "Dn",
    "A": "A", "D": "D", "W": "W", "S": "S",
    "SPACE": "Jmp", "LMB": "Atk", "RMB": "Sub",
    "Q": "Q", "E": "E",
    "P": "P", "ENTER": "Ent", "ESC": "Esc",
}


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("bscotm-tas — Bloodstained: Curse of the Moon")
        root.geometry("1200x720")

        # Model state
        self.movie: Movie | None = None
        self.movie_path: Path | None = None
        self.frames_dense: list[dict] = []   # each entry: {action_name: 1}
        self.current_frame: int = 0
        self.baseline_id: str | None = None
        self.prelude_id: str | None = None
        self.busy: bool = False
        self._stop_flag: dict = {"flag": False}

        # Thread → UI messaging
        self.msg_queue: queue.Queue = queue.Queue()

        self._build_ui()
        self._populate_combos()
        self._update_buttons()

        self.root.after(100, self._poll_queue)

    # ---- UI construction ---------------------------------------------------
    def _build_ui(self):
        # --- toolbar ---
        tb = tk.Frame(self.root, bd=1, relief=tk.RAISED)
        tb.pack(side=tk.TOP, fill=tk.X)

        def add_btn(text, cmd):
            b = tk.Button(tb, text=text, command=cmd, padx=6)
            b.pack(side=tk.LEFT, padx=1, pady=2)
            return b

        add_btn("Open", self.open_movie)
        add_btn("Save", self.save_movie)
        add_btn("Save As…", self.save_movie_as)

        ttk.Separator(tb, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y,
                                                    padx=4, pady=2)
        self.btn_record = add_btn("● Record", self.record_movie)
        self.btn_play   = add_btn("▶ Play",   self.play_movie)
        self.btn_stop   = add_btn("■ Stop",   self.stop_current)
        self.btn_rewind = add_btn("⏪ Rewind to cursor", self.rewind_to_cursor)
        self.btn_resume = add_btn("⏺ Record from cursor", self.resume_record_from_cursor)

        ttk.Separator(tb, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y,
                                                    padx=4, pady=2)
        tk.Label(tb, text="Baseline:").pack(side=tk.LEFT)
        self.cmb_baseline = ttk.Combobox(tb, state="readonly", width=16)
        self.cmb_baseline.pack(side=tk.LEFT, padx=2)
        self.cmb_baseline.bind("<<ComboboxSelected>>", self._on_baseline_changed)

        tk.Label(tb, text="Prelude:").pack(side=tk.LEFT, padx=(6, 0))
        self.cmb_prelude = ttk.Combobox(tb, state="readonly", width=20)
        self.cmb_prelude.pack(side=tk.LEFT, padx=2)
        self.cmb_prelude.bind("<<ComboboxSelected>>", self._on_prelude_changed)

        self.lbl_frame = tk.Label(tb, text="Frame: 0 / 0", padx=8)
        self.lbl_frame.pack(side=tk.RIGHT, padx=6)

        # --- grid ---
        mid = tk.Frame(self.root)
        mid.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        vsb = ttk.Scrollbar(mid, orient="vertical")
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        hsb = ttk.Scrollbar(mid, orient="horizontal")
        hsb.pack(side=tk.BOTTOM, fill=tk.X)

        cols = ["frame"] + COLUMN_ORDER
        self.tree = ttk.Treeview(
            mid, columns=cols, show="headings",
            yscrollcommand=vsb.set, xscrollcommand=hsb.set,
            selectmode="browse",
        )
        vsb.config(command=self.tree.yview)
        hsb.config(command=self.tree.xview)

        self.tree.heading("frame", text="#")
        self.tree.column("frame", width=55, anchor="e", stretch=False)
        for name in COLUMN_ORDER:
            self.tree.heading(name, text=HEADER_LABELS.get(name, name))
            self.tree.column(name, width=38, anchor="center", stretch=False)

        # Style for current-frame row
        self.tree.tag_configure("current",
                                background="#d0f0d0", foreground="#000000")
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.tree.bind("<Button-1>", self._on_tree_click)
        self.tree.bind("<Up>",   lambda e: self._move_cursor(-1) or "break")
        self.tree.bind("<Down>", lambda e: self._move_cursor(+1) or "break")

        # --- status bar ---
        sb = tk.Frame(self.root)
        sb.pack(side=tk.BOTTOM, fill=tk.X)
        self.lbl_status = tk.Label(sb, text="Ready.", anchor="w", padx=6)
        self.lbl_status.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.lbl_game_state = tk.Label(sb, text="", anchor="e",
                                       font=("Consolas", 9), padx=6)
        self.lbl_game_state.pack(side=tk.RIGHT)

    def _populate_combos(self):
        self.cmb_baseline["values"] = ["(none)"] + self._list_dirs(DEFAULT_BASELINE_DIR)
        self.cmb_baseline.current(0)
        self.cmb_prelude["values"] = ["(none)"] + self._list_dirs(DEFAULT_PRELUDE_DIR)
        self.cmb_prelude.current(0)

    def _list_dirs(self, p: Path):
        if not p.exists():
            return []
        return sorted(d.name for d in p.iterdir() if d.is_dir())

    def _on_baseline_changed(self, *_):
        v = self.cmb_baseline.get()
        self.baseline_id = None if v == "(none)" else v

    def _on_prelude_changed(self, *_):
        v = self.cmb_prelude.get()
        self.prelude_id = None if v == "(none)" else v

    # ---- Grid rendering ----------------------------------------------------
    def _render_grid(self):
        self.tree.delete(*self.tree.get_children())
        for i, frame in enumerate(self.frames_dense):
            values = [str(i + 1)]
            for name in COLUMN_ORDER:
                values.append("●" if frame.get(name) else "")
            self.tree.insert("", "end", iid=str(i), values=values)
        self._highlight_current()
        self._update_frame_label()

    def _update_cell_display(self, row: int):
        iid = str(row)
        if not self.tree.exists(iid):
            return
        frame = self.frames_dense[row]
        cur = list(self.tree.item(iid, "values"))
        for i, name in enumerate(COLUMN_ORDER):
            cur[i + 1] = "●" if frame.get(name) else ""
        self.tree.item(iid, values=cur)

    def _update_frame_label(self):
        n = len(self.frames_dense)
        cur = (self.current_frame + 1) if n else 0
        self.lbl_frame.config(text=f"Frame: {cur} / {n}")

    def _highlight_current(self):
        # Remove tag from all rows, add it to the current one.
        for iid in self.tree.get_children():
            if "current" in self.tree.item(iid, "tags"):
                self.tree.item(iid, tags=())
        iid = str(self.current_frame)
        if self.tree.exists(iid):
            self.tree.item(iid, tags=("current",))
            self.tree.see(iid)

    def _move_cursor(self, delta: int):
        if not self.frames_dense:
            return
        self.current_frame = max(0, min(len(self.frames_dense) - 1,
                                        self.current_frame + delta))
        self._highlight_current()
        self._update_frame_label()

    # ---- Grid interaction --------------------------------------------------
    def _on_tree_click(self, ev):
        region = self.tree.identify_region(ev.x, ev.y)
        if region != "cell":
            return
        iid = self.tree.identify_row(ev.y)
        col_id = self.tree.identify_column(ev.x)
        if not iid or not col_id:
            return
        try:
            row = int(iid)
            col_idx = int(col_id[1:]) - 1   # #1 -> 0
        except (ValueError, IndexError):
            return

        if col_idx == 0:
            # Frame-number column: just move the cursor.
            self.current_frame = row
            self._highlight_current()
            self._update_frame_label()
            return

        # Action column: toggle the cell, and move cursor to this row.
        action = COLUMN_ORDER[col_idx - 1]
        frame = self.frames_dense[row]
        if frame.get(action):
            del frame[action]
        else:
            frame[action] = 1
        self._update_cell_display(row)
        self.current_frame = row
        self._highlight_current()
        self._update_frame_label()

    # ---- File actions ------------------------------------------------------
    def open_movie(self):
        path = filedialog.askopenfilename(
            title="Open movie",
            filetypes=[("bscotm-tas movie", "*.bscotm"), ("All files", "*.*")],
        )
        if not path:
            return
        try:
            movie = Movie.load(path)
        except Exception as e:
            messagebox.showerror("Open failed", str(e)); return
        self.movie = movie
        self.movie_path = Path(path)
        self.frames_dense = self._to_named_frames(movie.expand())
        self.current_frame = 0
        self._render_grid()
        self.root.title(f"bscotm-tas — {Path(path).name}")
        if movie.prelude_id:
            i = self.cmb_prelude["values"].index(movie.prelude_id) \
                if movie.prelude_id in self.cmb_prelude["values"] else -1
            if i >= 0:
                self.cmb_prelude.current(i)
                self.prelude_id = movie.prelude_id
        self._update_buttons()

    def save_movie(self):
        if not self.movie:
            return
        if not self.movie_path:
            return self.save_movie_as()
        self._write_movie(self.movie_path)

    def save_movie_as(self):
        if not self.movie:
            messagebox.showinfo("No movie", "Open or record a movie first.")
            return
        path = filedialog.asksaveasfilename(
            title="Save movie", defaultextension=".bscotm",
            filetypes=[("bscotm-tas movie", "*.bscotm")],
        )
        if not path:
            return
        self.movie_path = Path(path)
        self._write_movie(self.movie_path)
        self.root.title(f"bscotm-tas — {self.movie_path.name}")

    def _write_movie(self, path: Path):
        sparse = self._to_sparse_frames()
        self.movie.frames = sparse
        self.movie.prelude_id = self.prelude_id
        try:
            self.movie.save(path)
        except Exception as e:
            messagebox.showerror("Save failed", str(e)); return
        self.lbl_status.config(text=f"Saved {path}")

    # ---- Frame key conversion ---------------------------------------------
    def _to_named_frames(self, dense: list[dict]) -> list[dict]:
        """{vk_decimal(str):1} -> {action_name:1}"""
        out = []
        for f in dense:
            nf = {}
            for k, v in f.items():
                if v:
                    name = VK_DEC_TO_NAME.get(str(k))
                    if name:
                        nf[name] = 1
            out.append(nf)
        return out

    def _to_sparse_frames(self) -> list[dict]:
        """{action_name:1} -> sparse deltas with vk_decimal(str) keys."""
        dense = []
        for f in self.frames_dense:
            nf = {}
            for name in f:
                info = TRACKED_KEYS.get(name)
                if info:
                    _vk, dec = info
                    nf[dec] = 1
            dense.append(nf)
        sparse = []
        prev = {}
        for f in dense:
            delta = {}
            for k in prev:
                if k not in f:
                    delta[k] = 0
            for k in f:
                if k not in prev:
                    delta[k] = 1
            sparse.append(delta)
            prev = f
        return sparse

    # ---- Async operation dispatch -----------------------------------------
    def _run_async(self, name: str, fn, *args):
        if self.busy:
            messagebox.showinfo("Busy", "An operation is already running.")
            return
        self.busy = True
        self._stop_flag = {"flag": False}
        self._update_buttons()
        self.lbl_status.config(text=f"{name}: starting…")

        def worker():
            try:
                result = fn(*args)
                self.msg_queue.put(("done", name, result))
            except Exception as e:
                import traceback
                self.msg_queue.put(("error", name,
                                    f"{e}\n\n{traceback.format_exc()}"))

        threading.Thread(target=worker, daemon=True).start()

    # ---- Recording ---------------------------------------------------------
    def record_movie(self):
        path = filedialog.asksaveasfilename(
            title="Record to", defaultextension=".bscotm",
            filetypes=[("bscotm-tas movie", "*.bscotm")],
        )
        if not path:
            return
        self._run_async("record", self._do_record, Path(path))

    def _do_record(self, path: Path):
        engine = Engine(DEFAULT_EXE)
        if engine.is_game_running():
            engine.detach()
            engine.kill_game()
        if self.baseline_id:
            engine.saves.restore(DEFAULT_BASELINE_DIR / self.baseline_id)
        engine.launch_game()
        engine.attach()
        try:
            def on_progress(n):
                self.msg_queue.put(("progress", None,
                                    f"Recording: {n} frames"))
            movie = engine.record(path,
                                  should_stop=lambda: self._stop_flag["flag"],
                                  on_progress=on_progress)
            return {"saved_to": str(path), "frames": movie.total_frames}
        finally:
            engine.detach()
            engine.kill_game()

    # ---- Playback ----------------------------------------------------------
    def _prepare_movie_for_playback(self):
        """Push current grid edits into self.movie, without saving to disk."""
        if not self.movie:
            return
        self.movie.frames = self._to_sparse_frames()
        self.movie.prelude_id = self.prelude_id
        self.movie.prelude_frames = None  # use prelude_id reference
    
    def play_movie(self):
        if not self.movie:
            messagebox.showinfo("No movie", "Open a movie first."); return
        self._run_async("play", self._do_play)

    def _do_play(self):
        self._prepare_movie_for_playback()
        prelude = self._resolve_prelude()
        engine = Engine(DEFAULT_EXE)
        if engine.is_game_running():
            engine.detach()
            engine.kill_game()
        if self.baseline_id:
            engine.saves.restore(DEFAULT_BASELINE_DIR / self.baseline_id)
        engine.launch_game()
        engine.attach()
        try:
            def on_tick(f):
                st = engine.snapshot_status()
                st["frame"] = f
                self.msg_queue.put(("status", None, st))
            snapshot = engine.play(self.movie, prelude=prelude,
                                   speedup=True,
                                   stop_frame=len(self.frames_dense),
                                   on_tick=on_tick,
                                   should_stop=lambda: self._stop_flag["flag"])
            return snapshot or {}
        finally:
            engine.detach()
            engine.kill_game()

    # ---- Rewind ------------------------------------------------------------
    def rewind_to_cursor(self):
        if not self.movie:
            messagebox.showinfo("No movie", "Open a movie first."); return
        target = self.current_frame + 1   # 1-based
        self._run_async("rewind", self._do_rewind, target)

    def _do_rewind(self, target: int):
        self._prepare_movie_for_playback()
        prelude = self._resolve_prelude()
        prelude_len = prelude.total_frames if prelude else 0
        baseline_path = (DEFAULT_BASELINE_DIR / self.baseline_id
                         if self.baseline_id else None)
        engine = Engine(DEFAULT_EXE)

        def on_progress(f, total):
            # f is 1-based, measured against the movie (not including prelude).
            self.msg_queue.put(("progress", None,
                                f"Rewinding: frame {f}/{total}"))

        snapshot = engine.rewind_to(self.movie, target_frame=target,
                                    prelude=prelude,
                                    known_save_state=baseline_path,
                                    on_progress=on_progress)
        return snapshot or {}

    # ---- Stop --------------------------------------------------------------
    def stop_current(self):
        self._stop_flag["flag"] = True
        self.lbl_status.config(text="Stopping…")

    # ---- Helpers -----------------------------------------------------------
    def _resolve_prelude(self):
        # Prefer the movie's embedded prelude if present.
        if self.movie and self.movie.prelude_frames:
            return Movie(
                frames=self.movie.prelude_frames,
                prelude_id=self.movie.prelude_id,
                prelude_description=self.movie.prelude_description,
            )
        if not self.prelude_id:
            return None
        try:
            return load_prelude(self.prelude_id, DEFAULT_PRELUDE_DIR)
        except Exception as e:
            messagebox.showwarning("Prelude error", str(e))
            return None

    # ---- Queue polling / dispatch -----------------------------------------
    def _poll_queue(self):
        try:
            while True:
                kind, name, payload = self.msg_queue.get_nowait()
                if kind == "progress":
                    self.lbl_status.config(text=str(payload))
                elif kind == "status":
                    self._update_game_state(payload)
                elif kind == "done":
                    self.busy = False
                    self._update_buttons()
                    self.lbl_status.config(text=f"{name}: finished.")
                    self._on_operation_done(name, payload)
                elif kind == "error":
                    self.busy = False
                    self._update_buttons()
                    messagebox.showerror("Error", str(payload))
                    self.lbl_status.config(text=f"{name}: failed.")
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)

    def _update_game_state(self, st: dict):
        f = st.get("frame")
        if f is not None:
            self.current_frame = max(0, min(len(self.frames_dense) - 1, f - 1))
            self._highlight_current()
            self._update_frame_label()
        def fmt(v, p=2):
            return "?" if v is None else f"{v:.{p}f}"
        self.lbl_game_state.config(
            text=f"HP={st.get('health')} "
                 f"X={fmt(st.get('x'))} Y={fmt(st.get('y'))} "
                 f"XV={fmt(st.get('xvel'), 3)} YV={fmt(st.get('yvel'), 3)}"
        )

    def _on_operation_done(self, name: str, result):
        if name == "record" and result and "saved_to" in result:
            try:
                movie = Movie.load(result["saved_to"])
                self.movie = movie
                self.movie_path = Path(result["saved_to"])
                self.frames_dense = self._to_named_frames(movie.expand())
                self.current_frame = 0
                self._render_grid()
                self.root.title(f"bscotm-tas — {Path(result['saved_to']).name}")
                self.lbl_status.config(
                    text=f"Recorded {result.get('frames')} frames → "
                         f"{result['saved_to']}")
            except Exception as e:
                messagebox.showerror("Load recorded failed", str(e))
        elif name in ("play", "rewind") and result:
            self._update_game_state(result)
        elif name == "resume_record" and result:
            new_frames = result.get("new_frames", {})
            cursor = result.get("cursor", 0)
            if not new_frames:
                self.lbl_status.config(text="No new frames recorded.")
                return

            # Extend frames_dense as needed.
            max_idx = max(new_frames)
            while len(self.frames_dense) <= max_idx:
                self.frames_dense.append({})

            # Overwrite frames starting at cursor with the newly recorded ones.
            # (Only from cursor+1 onwards; cursor itself is the boundary.)
            for idx, frame in new_frames.items():
                if idx <= cursor:
                    continue
                # Merge: keep any pre-existing keys not re-recorded,
                # replace everything else.
                self.frames_dense[idx] = dict(frame)

            # Any frames between old_length and new_length that weren't
            # recorded get cleared so leftover old data doesn't linger.
            # (This is the "start fresh from cursor" behavior.)
            for i in range(cursor + 1, max_idx + 1):
                if i not in new_frames:
                    self.frames_dense[i] = {}

            self._render_grid()
            self.lbl_status.config(
                text=f"Recorded {len(new_frames)} frames from cursor "
                     f"{cursor + 1} (total now {len(self.frames_dense)})")

    def _update_buttons(self):
        if self.busy:
            # While busy: only Stop is enabled.
            self.btn_record.config(state=tk.DISABLED)
            self.btn_play.config(state=tk.DISABLED)
            self.btn_rewind.config(state=tk.DISABLED)
            self.btn_resume.config(state=tk.DISABLED)
            self.btn_stop.config(state=tk.NORMAL)
        else:
            has_movie = self.movie is not None
            self.btn_record.config(state=tk.NORMAL)
            self.btn_play.config(state=tk.NORMAL if has_movie else tk.DISABLED)
            self.btn_rewind.config(state=tk.NORMAL if has_movie else tk.DISABLED)
            self.btn_resume.config(state=tk.NORMAL if has_movie else tk.DISABLED)
            self.btn_stop.config(state=tk.DISABLED)
    
    def resume_record_from_cursor(self):
        if not self.movie:
            messagebox.showinfo("No movie", "Open a movie first."); return
        self._prepare_movie_for_playback()
        target = self.current_frame + 1
        if not messagebox.askyesno(
                "Record from cursor",
                f"Record new inputs starting after frame {target}?\n\n"
                "The game will restart, play up to the cursor at high speed, "
                "then start recording live. Press Stop when finished."):
            return
        self._run_async("resume_record", self._do_resume_record, target)

    def _do_resume_record(self, target: int):
        prelude = self._resolve_prelude()
        baseline_path = (DEFAULT_BASELINE_DIR / self.baseline_id
                         if self.baseline_id else None)
        engine = Engine(DEFAULT_EXE)

        # Collect newly recorded frames here, keyed by movie-frame-index.
        new_frames: dict[int, dict] = {}
        NAME_FROM_DEC = VK_DEC_TO_NAME

        def on_record_frame(idx: int, keys: dict):
            # keys: {vk_decimal(int): 1}
            named = {}
            for vk_dec, v in keys.items():
                name = NAME_FROM_DEC.get(str(vk_dec))
                if name and v:
                    named[name] = 1
            if named:
                new_frames[idx] = named

        def on_progress(n: int):
            self.msg_queue.put(("progress", None,
                                f"Recording from frame {target}: at {n}"))

        if baseline_path:
            engine.saves.restore(baseline_path)

        try:
            engine.resume_record(
                self.movie, prelude, cursor_frame=target,
                should_stop=lambda: self._stop_flag["flag"],
                on_progress=on_progress,
                on_record_frame=on_record_frame,
            )
        finally:
            try: engine.detach()
            except Exception: pass
            # Note: leave the game running. The user can close it manually
            # or use Play/Rewind to restart it.

        return {"new_frames": new_frames, "cursor": target}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def run():
    root = tk.Tk()
    MainWindow(root)
    root.mainloop()