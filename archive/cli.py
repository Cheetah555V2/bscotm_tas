"""
Command-line interface for bscotm-tas.

Usage:
    python -m bscotm_tas record <movie.bscotm>
    python -m bscotm_tas play   <movie.bscotm> [--no-speedup]
    python -m bscotm_tas rewind <movie.bscotm> --frame N
    python -m bscotm_tas status [--watch]
    python -m bscotm_tas info   <movie.bscotm>
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from pathlib import Path

from .constants import TARGET_PROCESS
from .engine import Engine, find_pid
from .movie import Movie
from .saves import SAVE_FILES


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _default_exe_path() -> Path:
    """Best-guess path to COTM.exe relative to this repo."""
    here = Path(__file__).resolve().parent.parent
    return here / "Bloodstained Curse of the Moon" / "exe" / "COTM.exe"


def _fmt_seconds(frames: int, fps: float = 60.0) -> str:
    s = frames / fps
    return f"{int(s // 60)}m{s % 60:05.2f}s"


def _print_status_line(st: dict):
    if not st or st.get("player_struct") is None:
        print("    (player struct not resolved — is the game in a level?)")
        return
    print(f"    HP={st.get('health')}  "
          f"X={_f(st.get('x'))}  Y={_f(st.get('y'))}  "
          f"XV={_f(st.get('xvel'), 5)}  YV={_f(st.get('yvel'), 5)}")


def _f(v, prec: int = 3) -> str:
    return "?" if v is None else f"{v:.{prec}f}"


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_record(args):
    movie_path = Path(args.movie)
    if movie_path.exists() and not args.force:
        print(f"[!] {movie_path} already exists. Use --force to overwrite.")
        return 1

    if not find_pid():
        print(f"[!] {TARGET_PROCESS} is not running. Launch the game first.")
        return 1

    engine = Engine(_default_exe_path())
    engine.attach()
    print("[+] Attached to game.")
    print(f"[*] Recording. Save will be written to: {movie_path}")
    print("[*] Press Ctrl+C in this terminal when you're done.")
    print()

    stop = {"flag": False}
    def _sigint(*_):
        stop["flag"] = True
    signal.signal(signal.SIGINT, _sigint)

    def on_progress(n):
        print(f"    {n} frames  ({_fmt_seconds(n)})")

    try:
        movie = engine.record(movie_path, should_stop=lambda: stop["flag"],
                              on_progress=on_progress)
    finally:
        engine.detach()

    print()
    print(f"[+] Recorded {movie.total_frames} frames "
          f"({_fmt_seconds(movie.total_frames)})")
    print(f"    keys used: {movie.keys_used()}")
    print(f"    saved to:  {movie_path}")
    return 0


def cmd_play(args):
    movie_path = Path(args.movie)
    if not movie_path.exists():
        print(f"[!] Movie not found: {movie_path}")
        return 1
    movie = Movie.load(movie_path)
    print(f"[+] Loaded {movie.total_frames} frames "
          f"({_fmt_seconds(movie.total_frames)}) "
          f"from {movie_path.name}")
    print(f"    author:  {movie.author}")
    print(f"    created: {movie.created_utc}")
    print(f"    keys:    {movie.keys_used()}")

    # --- Resolve checkpoint ---
    checkpoint_path = _checkpoint_path(getattr(args, "checkpoint", None))
    if checkpoint_path is False:
        return 1

    # --- Resolve prelude (only if no checkpoint) ---
    prelude = None
    if checkpoint_path is None:
        if movie.prelude_frames:
            prelude = Movie(frames=movie.prelude_frames,
                            prelude_id=movie.prelude_id,
                            prelude_description=movie.prelude_description)
        elif getattr(args, "prelude", None):
            from .prelude import load_prelude
            prelude = load_prelude(args.prelude)

    if checkpoint_path is not None:
        print(f"[+] Using checkpoint '{args.checkpoint}' (skipping prelude)")
    elif prelude:
        print(f"[+] Using prelude ({prelude.total_frames} frames)")

    engine = Engine(_default_exe_path())

    stop = {"flag": False}
    def _sigint(*_):
        stop["flag"] = True
    signal.signal(signal.SIGINT, _sigint)

    if checkpoint_path is not None or prelude is not None:
        # We can reach the movie's starting point automatically.
        # Let rewind_to handle kill → restore → launch → play.
        print("[*] Playing (this will relaunch the game).")

        last_tick = [-1]
        def on_progress(f, t):
            if f // 60 != last_tick[0] // 60:
                last_tick[0] = f
                print(f"    frame {f}/{t}  "
                      f"({_fmt_seconds(f)} / {_fmt_seconds(t)})")

        result = None
        try:
            result = engine.rewind_to(
                movie,
                target_frame=movie.total_frames,
                prelude=prelude,
                on_progress=on_progress,
            )
        finally:
            try:
                engine.detach()
            except Exception:
                pass

        print()
        print("[+] Playback finished.")
        if result:
            print("[*] Final game state:")
            _print_status_line(result)
        return 0

    # --- No checkpoint and no prelude: attach to a running game ---
    if not find_pid():
        print(f"[!] {TARGET_PROCESS} is not running, and no checkpoint or")
        print("    prelude was provided. Either:")
        print("      - launch the game manually and get to the movie's")
        print("        starting state, then re-run this command, or")
        print("      - pass --checkpoint <id> / --prelude <id> so the tool")
        print("        can set up the state for you.")
        return 1

    engine.attach()
    print("[+] Attached to running game.")

    last_tick = [-1]
    def on_tick(f):
        if f // 60 != last_tick[0] // 60:
            last_tick[0] = f
            print(f"    frame {f}/{movie.total_frames}  "
                  f"({_fmt_seconds(f)} / {_fmt_seconds(movie.total_frames)})")

    try:
        engine.play(movie, prelude=None, speedup=not args.no_speedup,
                    on_tick=on_tick, should_stop=lambda: stop["flag"])
    finally:
        try:
            engine.detach()
        except Exception:
            pass

    print("\n[+] Playback finished.")
    return 0


def cmd_rewind(args):
    movie_path = Path(args.movie)
    if not movie_path.exists():
        print(f"[!] Movie not found: {movie_path}")
        return 1
    movie = Movie.load(movie_path)
    target = args.frame
    if target < 1 or target > movie.total_frames:
        print(f"[!] --frame must be between 1 and {movie.total_frames}.")
        return 1

    print(f"[+] Loaded movie: {movie_path.name} ({movie.total_frames} frames)")
    print(f"[*] Rewind target: frame {target} ({_fmt_seconds(target)})")

    # --- Resolve checkpoint ---
    checkpoint_path = _checkpoint_path(getattr(args, "checkpoint", None))
    if checkpoint_path is False:
        return 1

    # --- Prelude: only needed if no checkpoint ---
    prelude = None
    if checkpoint_path is not None:
        print(f"[+] Using checkpoint '{args.checkpoint}' (skipping prelude)")
    else:
        if movie.prelude_frames:
            prelude = Movie(frames=movie.prelude_frames,
                            prelude_id=movie.prelude_id,
                            prelude_description=movie.prelude_description)
            print(f"[+] Using embedded prelude ({prelude.total_frames} frames)")
        elif args.prelude:
            from .prelude import load_prelude
            prelude = load_prelude(args.prelude)
            print(f"[+] Using prelude '{args.prelude}' "
                  f"({prelude.total_frames} frames)")
        else:
            print("[!] No prelude and no checkpoint; rewind will desync at "
                  "the movie start.")

    # --- Baseline: only needed if no checkpoint ---
    baseline_path = None
    if checkpoint_path is None:
        if args.baseline:
            from .saves import SaveManager
            sm = SaveManager(_default_exe_path().parent,
                             backup_dir="save_backups/baselines")
            baseline_path = sm.backup_dir / args.baseline
            if not baseline_path.exists():
                print(f"[!] Baseline '{args.baseline}' not found at "
                      f"{baseline_path}")
                return 1
            print(f"[+] Baseline: {baseline_path}")
        else:
            print("[!] No --baseline given; the sandbox save state will be "
                  "used as-is.")
            print("    For deterministic rewinds, pass --baseline <id>.")

    engine = Engine(_default_exe_path())

    if engine.is_game_running():
        print(f"[*] {TARGET_PROCESS} is running; will be killed and relaunched.")
    else:
        print(f"[*] {TARGET_PROCESS} is not running; will launch it.")

    def on_progress(f, t):
        if f % 500 == 0 or f == t:
            print(f"    replaying frame {f}/{t}")

    status = None
    try:
        status = engine.rewind_to(movie,
                                  target_frame=target,
                                  prelude=prelude,
                                  known_save_state=baseline_path,
                                  on_progress=on_progress)
    finally:
        try:
            engine.detach()
        except Exception:
            pass

    print()
    print(f"[+] Rewound to frame {target}.")
    print("[*] Final game state:")
    _print_status_line(status)
    return 0


def cmd_status(args):
    if not find_pid():
        print(f"[!] {TARGET_PROCESS} is not running.")
        return 1

    engine = Engine(_default_exe_path())
    engine.attach()
    print("[+] Attached. Reading state...")
    try:
        if args.watch:
            print("[*] Ctrl+C to stop.")
            try:
                while True:
                    st = engine.snapshot_status()
                    os.system("cls")
                    print(f"bscotm-tas status — {TARGET_PROCESS} "
                          f"pid={engine.pid}")
                    _print_status_line(st)
                    time.sleep(0.25)
            except KeyboardInterrupt:
                pass
        else:
            st = engine.snapshot_status()
            _print_status_line(st)
    finally:
        engine.detach()
    return 0


def cmd_info(args):
    movie_path = Path(args.movie)
    if not movie_path.exists():
        print(f"[!] Movie not found: {movie_path}")
        return 1
    movie = Movie.load(movie_path)
    print(f"Movie:          {movie_path}")
    print(f"Format:         bscotm-tas v1")
    print(f"Author:         {movie.author}")
    print(f"Created:        {movie.created_utc}")
    print(f"RNG seed:       {movie.rng_seed}")
    print(f"Anchors:        {len(movie.anchors)}")
    for a in movie.anchors:
        print(f"  - frame {a.frame}: {a.type} {a.label}")
    print(f"Prelude id:     {movie.prelude_id}")
    if movie.prelude_frames:
        print(f"Prelude frames: {len(movie.prelude_frames)}")
    print(f"Total frames:   {movie.total_frames}  "
          f"({_fmt_seconds(movie.total_frames)})")
    print(f"Keys used:      {movie.keys_used()}")

    n_shown = 0
    print()
    print("First few non-empty frames:")
    for i, f in enumerate(movie.frames[:300]):
        if f:
            print(f"  frame {i:>5}  {f}")
            n_shown += 1
            if n_shown >= 10:
                break
    if n_shown == 0:
        print("  (none in the first 300 frames)")
    return 0


def cmd_prelude_record(args):
    from .prelude import save_prelude

    if not find_pid():
        print(f"[!] {TARGET_PROCESS} is not running. Launch the game first.")
        return 1

    engine = Engine(_default_exe_path())
    engine.attach()
    print("[+] Attached to game.")
    print(f"[*] Recording prelude '{args.id}'.")
    print("[*] Start from a fresh boot (title screen), press the keys to get")
    print("    into Stage 1, then press Ctrl+C in this terminal.")
    print()

    stop = {"flag": False}
    def _sigint(*_):
        stop["flag"] = True
    signal.signal(signal.SIGINT, _sigint)

    def on_progress(n):
        print(f"    {n} frames")

    try:
        movie = engine.record(Path(f"_prelude_tmp.bscotm"),
                              should_stop=lambda: stop["flag"],
                              on_progress=on_progress)
    finally:
        engine.detach()

    try:
        Path("_prelude_tmp.bscotm").unlink()
    except Exception:
        pass

    movie.prelude_id = args.id
    movie.prelude_description = args.description or ""

    out_path = save_prelude(movie, args.id)
    print()
    print(f"[+] Prelude saved: {out_path}")
    print(f"    {movie.total_frames} frames  "
          f"({_fmt_seconds(movie.total_frames)})")
    print(f"    keys used: {movie.keys_used()}")
    return 0


def cmd_determinism(args):
    from .prelude import load_prelude
    from .saves import SaveManager

    movie_path = Path(args.movie)
    if not movie_path.exists():
        print(f"[!] Movie not found: {movie_path}")
        return 1
    movie = Movie.load(movie_path)

    # --- Resolve checkpoint ---
    checkpoint_path = _checkpoint_path(getattr(args, "checkpoint", None))
    if checkpoint_path is False:
        return 1

    # --- Prelude: required only when no checkpoint ---
    prelude = None
    if checkpoint_path is None:
        if movie.prelude_frames:
            prelude = Movie(frames=movie.prelude_frames,
                            prelude_id=movie.prelude_id,
                            prelude_description=movie.prelude_description)
        elif args.prelude:
            prelude = load_prelude(args.prelude)

        if not prelude:
            print("[!] No prelude available and no --checkpoint given.")
            print("    Pass --prelude <id>, embed a prelude in the movie, or")
            print("    pass --checkpoint <id> to skip the prelude entirely.")
            return 1

    # --- Baseline: only required when no checkpoint ---
    baseline_path = None
    if checkpoint_path is None:
        if args.baseline:
            exe_dir = _default_exe_path().parent
            sm = SaveManager(exe_dir, backup_dir="save_backups/baselines")
            candidate = sm.backup_dir / args.baseline
            if not candidate.exists():
                print(f"[!] Baseline '{args.baseline}' not found at {candidate}")
                return 1
            baseline_path = candidate
            print(f"[+] Baseline to restore: {candidate}")
        else:
            print("[!] No --baseline given. Determinism cannot be guaranteed")
            print("    because the sandbox save state will differ across runs.")
            print("    Re-run with --baseline <id>, or take one now with:")
            print("      python -m bscotm_tas baseline save <id>")
            return 1

    checkpoints = sorted({int(x) for x in args.checkpoints.split(",")})
    print(f"[+] Movie:       {movie_path.name} ({movie.total_frames} frames)")
    if checkpoint_path is not None:
        print(f"[+] Checkpoint:  {args.checkpoint} (prelude skipped)")
    elif prelude:
        print(f"[+] Prelude:     {prelude.total_frames} frames")
    print(f"[+] Checkpoints: {checkpoints}")
    print(f"[+] Runs:        {args.runs}")
    print()

    all_runs = []
    for run_idx in range(args.runs):
        print(f"=== Run {run_idx + 1}/{args.runs} ===")
        engine = Engine(_default_exe_path())
        t0 = time.time()
        try:
            st = engine.rewind_to(movie,
                                  target_frame=max(checkpoints),
                                  prelude=prelude,
                                  known_save_state=baseline_path)
        except Exception as e:
            print(f"[!] Run {run_idx + 1} failed: {e}")
            try:
                engine.detach()
            except Exception:
                pass
            continue
        elapsed = time.time() - t0

        all_runs.append(st)
        print(f"    state: HP={st.get('health')} X={st.get('x')} "
              f"Y={st.get('y')} XV={st.get('xvel')} YV={st.get('yvel')}"
              f"   [{elapsed:.2f}s]")
        try:
            engine.detach()
        except Exception:
            pass

    print()
    if len(all_runs) < 2:
        print("[!] Need at least 2 successful runs to compare.")
        return 1

    keys = ["health", "x", "y", "xvel", "yvel"]
    first = all_runs[0]
    consistent = True
    for i, st in enumerate(all_runs[1:], start=2):
        diff = {k: (first.get(k), st.get(k)) for k in keys
                if st.get(k) != first.get(k)}
        if diff:
            consistent = False
            print(f"[!] Run {i} differs from Run 1:")
            for k, (a, b) in diff.items():
                print(f"      {k}: {a} vs {b}")
        else:
            print(f"[+] Run {i} matches Run 1.")

    print()
    if consistent:
        print("[+] DETERMINISM: PASS")
        return 0
    print("[!] DETERMINISM: FAIL")
    return 2


def cmd_baseline(args):
    from .saves import SaveManager

    exe_dir = _default_exe_path().parent
    sm = SaveManager(exe_dir, backup_dir="save_backups/baselines")

    if args.action == "list":
        base = sm.backup_dir
        if not base.exists():
            print("[*] No baselines yet.")
            return 0
        items = sorted(p for p in base.iterdir() if p.is_dir())
        if not items:
            print("[*] No baselines yet.")
            return 0
        print(f"[+] Baselines in {base.resolve()}:\n")
        for p in items:
            files = sorted(f.name for f in p.iterdir() if f.name != ".meta")
            print(f"  {p.name}  ({len(files)} files)")
            for f in files:
                size = (p / f).stat().st_size
                print(f"      {f}  ({size} bytes)")
        return 0

    if args.action == "show":
        p = sm.backup_dir / args.id
        if not p.exists():
            print(f"[!] Baseline not found: {p}")
            return 1
        files = sorted(f.name for f in p.iterdir() if f.name != ".meta")
        print(f"[+] Baseline '{args.id}'  ({len(files)} files)")
        for f in files:
            size = (p / f).stat().st_size
            print(f"    {f}  ({size} bytes)")
        meta = p / ".meta"
        if meta.exists():
            print(f"\n    meta: {meta.read_text().strip()}")
        return 0

    if args.action == "delete":
        p = sm.backup_dir / args.id
        if not p.exists():
            print(f"[!] Baseline not found: {p}")
            return 1
        import shutil as _sh
        _sh.rmtree(p)
        print(f"[+] Deleted baseline '{args.id}'.")
        return 0

    if args.action == "save":
        if not args.id:
            print("[!] 'save' requires a baseline id.")
            return 1
        if find_pid():
            print(f"[!] Close {TARGET_PROCESS} first. Baselines must be taken")
            print("    while the game is not running, so the files on disk")
            print("    are the authoritative state.")
            return 1

        target = sm.backup_dir / args.id
        if target.exists() and not args.force:
            print(f"[!] Baseline '{args.id}' already exists. "
                  f"Use --force to overwrite.")
            return 1
        if target.exists():
            import shutil as _sh
            _sh.rmtree(target)

        target.mkdir(parents=True, exist_ok=True)
        import shutil as _sh
        n = 0
        for fn in SAVE_FILES:
            src = exe_dir / fn
            if src.exists():
                _sh.copy2(src, target / fn)
                n += 1
        (target / ".meta").write_text(
            f"id={args.id}\nfiles={n}\n", encoding="utf-8"
        )
        print(f"[+] Baseline '{args.id}' saved: {target}")
        print(f"    {n} files")
        for f in sorted(target.iterdir()):
            if f.name == ".meta":
                continue
            print(f"      {f.name}  ({f.stat().st_size} bytes)")
        return 0

    print(f"[!] Unknown action: {args.action}")
    return 1


def cmd_gui(args):
    from .gui_tk import run
    return run()


# ---------------------------------------------------------------------------
# Argparse wiring
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="bscotm_tas",
        description="bscotm-tas — TAS tool for "
                    "Bloodstained: Curse of the Moon",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    # --- record ---
    pr = sub.add_parser("record",
                        help="Record a new movie from a live session")
    pr.add_argument("movie", help="Output .bscotm file")
    pr.add_argument("--force", action="store_true",
                    help="Overwrite if exists")
    pr.set_defaults(func=cmd_record)

    # --- play ---
    pp = sub.add_parser("play", help="Play a movie")
    pp.add_argument("movie", help="Path to .bscotm file")
    pp.add_argument("--no-speedup", action="store_true",
                    help="Do not use the Sleep bypass during playback")
    pp.add_argument("--prelude", default=None,
                    help="Prelude ID to use (if not embedded)")
    pp.add_argument("--baseline", default=None,
                    help="Baseline save-state ID")
    pp.set_defaults(func=cmd_play)

    # --- rewind ---
    pw = sub.add_parser("rewind",
                        help="Restart the game and replay to a frame")
    pw.add_argument("movie", help="Path to .bscotm file")
    pw.add_argument("--frame", type=int, required=True,
                    help="Target frame (1-based)")
    pw.add_argument("--prelude", default=None,
                    help="Prelude ID to use (if not embedded)")
    pw.add_argument("--baseline", default=None,
                    help="Baseline save-state ID (recommended for "
                         "determinism)")
    pw.set_defaults(func=cmd_rewind)

    # --- status ---
    ps = sub.add_parser("status", help="Print live game state")
    ps.add_argument("--watch", action="store_true",
                    help="Continuously update (Ctrl+C to stop)")
    ps.set_defaults(func=cmd_status)

    # --- info ---
    pi = sub.add_parser("info", help="Show metadata of a movie file")
    pi.add_argument("movie", help="Path to .bscotm file")
    pi.set_defaults(func=cmd_info)

    # --- prelude-record ---
    prd = sub.add_parser("prelude-record",
                         help="Record a new prelude "
                              "(title → Stage 1 start)")
    prd.add_argument("id", help="Short ID, e.g. fresh_empty_stage1")
    prd.add_argument("--description", default="",
                     help="Human-readable description")
    prd.set_defaults(func=cmd_prelude_record)

    # --- determinism ---
    pd = sub.add_parser("determinism",
                        help="Run a movie N times and compare end state")
    pd.add_argument("movie", help="Path to .bscotm file")
    pd.add_argument("--prelude", default=None,
                    help="Prelude ID (if not embedded in the movie)")
    pd.add_argument("--baseline", default=None,
                    help="Baseline save-state id")
    pd.add_argument("--runs", type=int, default=3,
                    help="How many times to run (default 3)")
    pd.set_defaults(func=cmd_determinism)

    # --- baseline ---
    pb = sub.add_parser("baseline",
                        help="Manage baseline save states")
    pb.add_argument("action", choices=["save", "list", "show", "delete"])
    pb.add_argument("id", nargs="?", default=None,
                    help="Baseline id (required for save/show/delete)")
    pb.add_argument("--force", action="store_true",
                    help="Overwrite existing baseline")
    pb.set_defaults(func=cmd_baseline)


    # --- gui ---
    pg = sub.add_parser("gui", help="Launch the graphical interface")
    pg.set_defaults(func=cmd_gui)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())