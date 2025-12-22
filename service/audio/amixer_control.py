"""Volume and mute helpers using amixer."""
from __future__ import annotations

import asyncio
import logging
import shutil

LOGGER = logging.getLogger(__name__)


class AmixerController:
    def __init__(self, card: str, control: str) -> None:
        self.card = card
        self.control = control

    async def set_volume(self, percent: int) -> None:
        percent = max(0, min(percent, 100))
        if not shutil.which("amixer"):
            LOGGER.warning("amixer binary not found; skipping volume change")
            return
        cmd = ["amixer", "-c", self.card, "set", self.control, f"{percent}%"]
        await self._run(cmd)

    async def set_mute(self, mute: bool) -> None:
        if not shutil.which("amixer"):
            LOGGER.warning("amixer binary not found; skipping mute change")
            return
        cmd = ["amixer", "-c", self.card, "set", self.control, "mute" if mute else "unmute"]
        await self._run(cmd)

    async def _run(self, cmd: list[str]) -> None:
        LOGGER.info("Executing %s", " ".join(cmd))
        process = await asyncio.create_subprocess_exec(*cmd)
        await process.wait()
        if process.returncode != 0:
            LOGGER.error("Command failed with exit code %s", process.returncode)
