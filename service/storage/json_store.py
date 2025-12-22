"""Lightweight JSON persistence helpers for runtime state and IDs."""
from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from typing import Any, Dict


class JsonStateStore:
    """Durable JSON store with basic namespacing semantics."""

    def __init__(self, path: Path) -> None:
        self._path = Path(path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._cache: Dict[str, Any] | None = None

    @property
    def path(self) -> Path:
        return self._path

    def read_all(self) -> Dict[str, Any]:
        with self._lock:
            if self._cache is None:
                self._cache = self._load_from_disk()
            # Return a shallow copy to avoid accidental mutation of the cache
            return dict(self._cache)

    def _load_from_disk(self) -> Dict[str, Any]:
        if not self._path.exists():
            return {}
        try:
            with self._path.open("r", encoding="utf-8") as infile:
                return json.load(infile)
        except json.JSONDecodeError:
            # Corrupted file → move aside and start clean to avoid crashing the node
            corrupt_path = self._path.with_suffix(".corrupt")
            self._path.rename(corrupt_path)
            return {}

    def write_all(self, data: Dict[str, Any]) -> None:
        with self._lock:
            tmp_path = self._path.with_suffix(".tmp")
            with tmp_path.open("w", encoding="utf-8") as outfile:
                json.dump(data, outfile, indent=2, sort_keys=True)
                outfile.flush()
                os.fsync(outfile.fileno())
            tmp_path.replace(self._path)
            self._cache = dict(data)

    def read_namespace(self, name: str) -> Dict[str, Any]:
        data = self.read_all()
        return dict(data.get(name, {}))

    def write_namespace(self, name: str, payload: Dict[str, Any]) -> None:
        data = self.read_all()
        data[name] = payload
        self.write_all(data)

    def get_or_create_uuid(self, name: str) -> str:
        from uuid import uuid4

        with self._lock:
            data = self.read_all()
            identity = data.setdefault("identity", {})
            if name not in identity:
                identity[name] = str(uuid4())
                self.write_all(data)
            return identity[name]
