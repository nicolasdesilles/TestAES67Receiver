"""Volume and mute helpers using amixer."""
from __future__ import annotations

import asyncio
import logging
import shutil
from typing import Sequence

LOGGER = logging.getLogger(__name__)


class AmixerController:
    def __init__(self, card: str, controls: Sequence[str]) -> None:
        self.card = card
        self.controls = [control for control in controls if control]
        if not self.controls:
            LOGGER.warning("No amixer controls provided; volume operations will be skipped")

    async def set_volume(self, percent: int) -> None:
        percent = max(0, min(percent, 100))
        if not shutil.which("amixer"):
            LOGGER.warning("amixer binary not found; skipping volume change")
            return
        for control in self.controls:
            cmd = ["amixer", "-c", self.card, "set", control, f"{percent}%"]
            await self._run(cmd)

    async def set_mute(self, mute: bool) -> None:
        if not shutil.which("amixer"):
            LOGGER.warning("amixer binary not found; skipping mute change")
            return
        action = "mute" if mute else "unmute"
        for control in self.controls:
            cmd = ["amixer", "-c", self.card, "set", control, action]
            await self._run(cmd)

    async def _run(self, cmd: list[str]) -> None:
        LOGGER.info("Executing %s", " ".join(cmd))
        process = await asyncio.create_subprocess_exec(*cmd)
        await process.wait()
        if process.returncode != 0:
            LOGGER.error("Command failed with exit code %s", process.returncode)
