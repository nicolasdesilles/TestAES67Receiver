"""Manage alsaloop to pipe aes67 capture into the headphone output."""
from __future__ import annotations

import asyncio
import logging
import shutil
from typing import Sequence

LOGGER = logging.getLogger(__name__)


class AlsaLoopController:
    """Start/stop an alsaloop process and monitor its lifecycle."""

    def __init__(
        self,
        capture_device: str,
        playback_device: str,
        buffer_ms: int = 50,
        extra_args: Sequence[str] | None = None,
    ) -> None:
        self.capture_device = capture_device
        self.playback_device = playback_device
        self.buffer_ms = buffer_ms
        self.extra_args = list(extra_args or [])
        self._process: asyncio.subprocess.Process | None = None

    async def ensure_running(self) -> None:
        if not shutil.which("alsaloop"):
            LOGGER.warning("alsaloop binary not found; audio will not bridge capture/playback")
            return
        if self._process and self._process.returncode is None:
            return
        cmd = [
            "alsaloop",
            "-C",
            self.capture_device,
            "-P",
            self.playback_device,
            "-t",
            str(self.buffer_ms),
        ]
        cmd.extend(self.extra_args)
        LOGGER.info("Starting alsaloop: %s", " ".join(cmd))
        self._process = await asyncio.create_subprocess_exec(*cmd)

    async def stop(self) -> None:
        if not self._process:
            return
        LOGGER.info("Stopping alsaloop")
        self._process.terminate()
        try:
            await asyncio.wait_for(self._process.wait(), timeout=3)
        except asyncio.TimeoutError:
            LOGGER.warning("alsaloop did not exit cleanly; killing")
            self._process.kill()
            await self._process.wait()
        self._process = None
