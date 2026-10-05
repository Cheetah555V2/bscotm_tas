"""
Prelude support.

A "prelude" is a short input sequence that takes the game from a cold
boot to the exact state a movie begins from — typically the first frame
where the player has control in Stage 1.

Preludes are stored as .bscotm files in a chosen directory (default:
./preludes). A movie can embed a prelude inline (self-contained) or
reference one by ID (small file, requires the prelude to be present).
"""

from __future__ import annotations

from pathlib import Path

from .movie import Movie

DEFAULT_PRELUDE_DIR = Path("preludes")


def prelude_path(prelude_id: str,
                 base_dir: Path = DEFAULT_PRELUDE_DIR) -> Path:
    return base_dir / f"{prelude_id}.bscotm"


def load_prelude(prelude_id: str,
                 base_dir: Path = DEFAULT_PRELUDE_DIR) -> Movie:
    p = prelude_path(prelude_id, base_dir)
    if not p.exists():
        raise FileNotFoundError(f"Prelude not found: {p}")
    return Movie.load(p)


def save_prelude(prelude: Movie,
                 prelude_id: str,
                 base_dir: Path = DEFAULT_PRELUDE_DIR) -> Path:
    base_dir.mkdir(parents=True, exist_ok=True)
    p = prelude_path(prelude_id, base_dir)
    prelude.save(p)
    return p