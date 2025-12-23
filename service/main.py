"""Entrypoint wiring FastAPI, IS-04 worker, and IS-05 API."""
from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager, suppress
from pathlib import Path
from typing import Any

from fastapi import FastAPI

from service.audio.alsaloop import AlsaLoopController
from service.audio.amixer_control import AmixerController
from service.config import AppConfig, ensure_identity, load_config
from service.daemon.aes67d_client import AES67DaemonClient
from service.nmos.is04_registration import IS04RegistrationWorker
from service.nmos.is04_node_api import build_node_api_router
from service.nmos.is05_connection_api import build_router
from service.storage.json_store import JsonStateStore

LOGGER = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO)


def create_app(config_path: str | Path | None = None) -> FastAPI:
    config: AppConfig = load_config(config_path)
    LOGGER.info(
        "Config loaded: daemon.base_url=%s sink_id=%s poll=%ss interface=%s http_port=%s",
        config.daemon.base_url,
        config.daemon.sink_id,
        config.daemon.status_poll_interval,
        config.interface_name,
        config.http_port,
    )
    state_store = JsonStateStore(config.state_file)
    identity = ensure_identity(state_store)

    daemon_client = AES67DaemonClient(str(config.daemon.base_url), config.daemon.sink_id)
    alsaloop = AlsaLoopController(
        capture_device=config.audio.capture_device,
        playback_device=config.audio.playback_device,
        buffer_ms=config.audio.alsaloop_buffer_ms,
    )
    amixer = AmixerController(config.audio.amixer_card, config.audio.amixer_controls)

    router = build_router(config, state_store, daemon_client, alsaloop, amixer)
    node_router = build_node_api_router(config, identity, state_store, daemon_client)
    is04_worker = IS04RegistrationWorker(config, identity, state_store)

    async def daemon_monitor() -> None:
        last_flags: dict[str, object] | None = None
        last_ptp: dict[str, object] | None = None
        sink_present: bool | None = None
        consecutive_failures = 0
        while True:
            try:
                sinks = await daemon_client.list_sinks()
                sink_ids = {
                    int(item.get("id"))
                    for item in sinks.get("sinks", [])
                    if isinstance(item, dict) and isinstance(item.get("id"), int)
                }
                now_present = daemon_client.sink_id in sink_ids
                if now_present != sink_present:
                    sink_present = now_present
                    if sink_present:
                        LOGGER.info("aes67-linux-daemon sink %s is present", daemon_client.sink_id)
                    else:
                        LOGGER.info("aes67-linux-daemon sink %s is not configured yet", daemon_client.sink_id)
                        last_flags = None

                if sink_present:
                    status = await daemon_client.fetch_sink_status()
                    flags = (status or {}).get("sink_flags", {}) if isinstance(status, dict) or status is None else {}
                    if flags != last_flags:
                        LOGGER.info("aes67-linux-daemon sink_flags changed: %s", flags)
                        last_flags = dict(flags) if isinstance(flags, dict) else {"raw": flags}

                ptp = await daemon_client.fetch_ptp_status()
                if ptp != last_ptp:
                    LOGGER.info("aes67-linux-daemon PTP status changed: %s", ptp)
                    last_ptp = dict(ptp) if isinstance(ptp, dict) else {"raw": ptp}

                if consecutive_failures:
                    LOGGER.info("aes67-linux-daemon status poll recovered")
                    consecutive_failures = 0
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                consecutive_failures += 1
                if consecutive_failures in (1, 5, 20):
                    LOGGER.warning(
                        "aes67-linux-daemon status poll failed (%s) for base_url=%s: %s",
                        consecutive_failures,
                        daemon_client.base_url,
                        exc,
                    )
            await asyncio.sleep(config.daemon.status_poll_interval)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        worker_task = asyncio.create_task(is04_worker.run(), name="is04-registration")
        daemon_task = asyncio.create_task(daemon_monitor(), name="aes67d-monitor")
        try:
            yield {"config": config, "identity": identity}
        finally:
            await is04_worker.stop()
            worker_task.cancel()
            daemon_task.cancel()
            with suppress(asyncio.CancelledError):
                await worker_task
            with suppress(asyncio.CancelledError):
                await daemon_task
            await daemon_client.close()

    app = FastAPI(
        title="AES67 NMOS Wrapper",
        version="0.1.0",
        lifespan=lifespan,
    )
    app.include_router(router)
    app.include_router(node_router)

    @app.get("/health/live")
    async def liveness() -> dict[str, Any]:
        return {"ok": True}

    @app.get("/health/ready")
    async def readiness() -> dict[str, Any]:
        # Future: surface registry and daemon readiness signals
        return {"ok": True}

    return app


app = create_app()
