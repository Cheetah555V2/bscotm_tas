"""
Save file management — SAFE version.

The game detects changes to save-slot EXISTENCE (files added, removed,
renamed) and treats them as corruption. We therefore NEVER delete or
rename GameData*.bin / SystemData.bin.

We only:
  - Copy files out (backup).
  - Copy files over existing names (restore).

Files present in the exe_dir but absent from a backup are left untouched.
"""

from __future__ import annotations

import shutil
import time
from contextlib import contextmanager
from pathlib import Path


SAVE_FILES = [f"GameData{i:02d}.bin" for i in range(8)] + ["SystemData.bin"]


class SaveManager:
    def __init__(self, exe_dir: str | Path,
                 backup_dir: str | Path = "save_backups"):
        self.exe_dir = Path(exe_dir)
        self.backup_dir = Path(backup_dir)
        if not self.exe_dir.exists():
            raise FileNotFoundError(self.exe_dir)

    def snapshot(self, label: str | None = None) -> Path:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        name = f"{stamp}_{label}" if label else stamp
        out = self.backup_dir / name
        out.mkdir(parents=True, exist_ok=True)
        n = 0
        for fn in SAVE_FILES:
            src = self.exe_dir / fn
            if src.exists():
                shutil.copy2(src, out / fn)
                n += 1
        (out / ".meta").write_text(
            f"files={n}\ntimestamp={stamp}\nlabel={label or ''}\n",
            encoding="utf-8",
        )
        return out

    def restore(self, backup_path: str | Path):
        """
        Copy files from a backup over the current ones.
        Never deletes anything. Missing files in the backup are left alone.
        """
        backup_path = Path(backup_path)
        if not backup_path.exists():
            raise FileNotFoundError(backup_path)
        for fn in SAVE_FILES:
            src = backup_path / fn
            if src.exists():
                shutil.copy2(src, self.exe_dir / fn)

    @contextmanager
    def snapshot_and_restore(self, label: str | None = None):
        snap = self.snapshot(label=label)
        try:
            yield snap
        finally:
            self.restore(snap)


def default_save_manager(exe_path: str | Path,
                         backup_dir: str | Path = "save_backups") -> SaveManager:
    return SaveManager(Path(exe_path).parent, backup_dir)