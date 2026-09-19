"""
Movie file format.

Format (.bscotm, JSON):
{
  "format": "bscotm-tas",
  "format_version": 1,
  "game": "Bloodstained: Curse of the Moon",
  "game_version": "1.1.2",
  "platform": "steam-windows-x86",
  "author": "yourname",
  "created_utc": "2026-09-17T12:34:56Z",
  "rng_seed": null,
  "anchors": [
    {"frame": 0,    "type": "start"},
    {"frame": 340,  "type": "level_start", "label": "Stage 1"},
    ...
  ],
  "keys_used": ["D","SPACE","LMB"],
  "total_frames": 72000,
  "frames": [
    {},                          # frame 0: no keys
    {"D": 1},                    # frame 1: D held
    {"D": 1, "SPACE": 1},        # frame 2: D + Space
    ...
  ]
}

Frames are 0-indexed in the file (frame 0 is the first frame of input).
The "frames" list contains only entries for frames where something changed;
the loader reconstructs the full timeline by treating a missing entry as
"same as the previous entry".
"""

import json
import datetime
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from .constants import (
    MOVIE_FORMAT, MOVIE_FORMAT_VERSION, GAME_TITLE, GAME_VERSION, PLATFORM,
)

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class Anchor:
    frame: int
    type: str               # "start", "level_start", ...
    label: str = ""

    def to_dict(self):
        d = {"frame": self.frame, "type": self.type}
        if self.label:
            d["label"] = self.label
        return d

    @classmethod
    def from_dict(cls, d):
        return cls(frame=int(d["frame"]),
                   type=str(d.get("type", "unknown")),
                   label=str(d.get("label", "")))

@dataclass
class Movie:
    author: str = "unknown"
    created_utc: str = ""
    rng_seed: int | None = None
    anchors: list[Anchor] = field(default_factory=list)
    frames: list[dict] = field(default_factory=list)   # sparse: only changed frames

    # --- prelude fields (new) ---
    # A "movie" that begins at frame 1 of Stage 1 should carry a prelude so
    # that a cold boot can be replayed deterministically up to the movie's
    # starting point.
    prelude_id: str | None = None
    prelude_description: str = ""
    prelude_frames: list[dict] | None = None  # inline prelude (self-contained)

    # ---- helpers --------------------------------------------------------
    @property
    def total_frames(self) -> int:
        return len(self.frames)

    def has_prelude(self) -> bool:
        return bool(self.prelude_frames)

    def keys_used(self) -> list[str]:
        s = set()
        for f in self.frames:
            for k, v in f.items():
                if v:
                    s.add(k)
        return sorted(s)

    def frame_at(self, index: int) -> dict:
        """Return the effective key set at frame `index`."""
        return self.frames[index] if 0 <= index < len(self.frames) else {}

    def expand(self) -> list[dict]:
        """Return a dense list: one dict per frame."""
        out = []
        prev = {}
        for f in self.frames:
            merged = dict(prev)
            for k, v in f.items():
                if v:
                    merged[k] = 1
                else:
                    merged.pop(k, None)
            out.append(merged)
            prev = merged
        return out

    # ---- I/O ------------------------------------------------------------
    def save(self, path: str | Path):
        path = Path(path)
        doc = {
            "format": MOVIE_FORMAT,
            "format_version": MOVIE_FORMAT_VERSION,
            "game": GAME_TITLE,
            "game_version": GAME_VERSION,
            "platform": PLATFORM,
            "author": self.author,
            "created_utc": self.created_utc or _now_utc(),
            "rng_seed": self.rng_seed,
            "anchors": [a.to_dict() for a in self.anchors],

            "prelude_id": self.prelude_id,
            "prelude_description": self.prelude_description,
            "prelude_frames": self.prelude_frames,

            "keys_used": self.keys_used(),
            "total_frames": self.total_frames,
            "frames": self.frames,
        }
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, separators=(",", ":"))
        return path

    @classmethod
    def load(cls, path: str | Path) -> "Movie":
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        if doc.get("format") != MOVIE_FORMAT:
            raise ValueError(f"Not a {MOVIE_FORMAT} file: {doc.get('format')!r}")
        if doc.get("format_version") != MOVIE_FORMAT_VERSION:
            raise ValueError(f"Unsupported format version: "
                             f"{doc.get('format_version')}")
        return cls(
            author=doc.get("author", "unknown"),
            created_utc=doc.get("created_utc", ""),
            rng_seed=doc.get("rng_seed"),
            anchors=[Anchor.from_dict(a) for a in doc.get("anchors", [])],
            frames=list(doc.get("frames", [])),
            prelude_id=doc.get("prelude_id"),
            prelude_description=doc.get("prelude_description", ""),
            prelude_frames=doc.get("prelude_frames"),
        )

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _now_utc() -> str:
    return datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")