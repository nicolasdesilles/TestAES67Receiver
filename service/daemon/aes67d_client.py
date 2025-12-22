"""HTTP client for aes67-linux-daemon interactions."""
from __future__ import annotations

import asyncio
import logging
from typing import Any, Dict

import httpx

LOGGER = logging.getLogger(__name__)


class AES67DaemonClient:
    """Small wrapper around the daemon REST API."""

    def __init__(self, base_url: str, sink_id: int, timeout: float = 5.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.sink_id = sink_id
        self._client = httpx.AsyncClient(timeout=timeout)
        self._lock = asyncio.Lock()

    async def upsert_sink(self, payload: Dict[str, Any]) -> None:
        url = f"{self.base_url}/api/sink/{self.sink_id}"
        LOGGER.info(
            "Configuring aes67 sink %s via %s (use_sdp=%s map=%s)",
            self.sink_id,
            url,
            payload.get("use_sdp"),
            payload.get("map"),
        )
        async with self._lock:
            response = await self._client.put(url, json=payload)
            response.raise_for_status()

    async def delete_sink(self) -> None:
        url = f"{self.base_url}/api/sink/{self.sink_id}"
        LOGGER.info("Deleting aes67 sink %s via %s", self.sink_id, url)
        async with self._lock:
            response = await self._client.delete(url)
            if response.status_code not in (200, 204, 404):
                response.raise_for_status()

    async def list_sinks(self) -> dict[str, Any]:
        url = f"{self.base_url}/api/sinks"
        async with self._lock:
            response = await self._client.get(url)
            response.raise_for_status()
            return response.json()

    async def fetch_sink_status(self) -> Dict[str, Any] | None:
        url = f"{self.base_url}/api/sink/status/{self.sink_id}"
        async with self._lock:
            response = await self._client.get(url)
            if response.status_code in (400, 404):
                # Common before any activation: sink not configured yet.
                return None
            response.raise_for_status()
            return response.json()

    async def fetch_config(self) -> dict[str, Any]:
        url = f"{self.base_url}/api/config"
        async with self._lock:
            response = await self._client.get(url)
            response.raise_for_status()
            return response.json()

    async def fetch_ptp_status(self) -> dict[str, Any]:
        url = f"{self.base_url}/api/ptp/status"
        async with self._lock:
            response = await self._client.get(url)
            response.raise_for_status()
            return response.json()

    async def close(self) -> None:
        await self._client.aclose()
