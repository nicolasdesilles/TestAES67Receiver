"""IS-04 registration + heartbeat worker."""
from __future__ import annotations

import asyncio
import datetime as dt
import logging
import socket
from contextlib import suppress
from dataclasses import dataclass
from typing import Any, Dict, Optional

import httpx

from service.config import AppConfig, NodeIdentity
from service.storage.json_store import JsonStateStore

LOGGER = logging.getLogger(__name__)
SERVICE_TYPE = "_nmos-registration._tcp.local."


@dataclass
class RegistryEndpoint:
    url: str
    priority: int = 0


class IS04RegistrationWorker:
    def __init__(
        self,
        config: AppConfig,
        identity: NodeIdentity,
        state_store: JsonStateStore,
    ) -> None:
        self._config = config
        self._identity = identity
        self._state_store = state_store
        self._client = httpx.AsyncClient(timeout=5.0)
        self._stop = asyncio.Event()
        self._registry: RegistryEndpoint | None = None
        self._registered = False

    async def run(self) -> None:
        LOGGER.info("IS-04 worker started (%s)", self._config.describe_discovery())
        while not self._stop.is_set():
            try:
                await self._tick()
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # pragma: no cover - best-effort logging
                LOGGER.warning("IS-04 worker error: %s", exc)
            try:
                await asyncio.wait_for(
                    asyncio.shield(self._stop.wait()),
                    timeout=self._config.registry.heartbeat_interval,
                )
            except asyncio.TimeoutError:
                continue

    async def _tick(self) -> None:
        if not self._registry:
            self._registry = await self._discover_registry()
            if not self._registry:
                LOGGER.debug("No registry discovered yet")
                return
            LOGGER.info("Using registry %s", self._registry.url)
            self._registered = False
        if not self._registered:
            await self._register_resources()
            return
        await self._send_heartbeat()

    async def _discover_registry(self) -> RegistryEndpoint | None:
        if self._config.registry.mode == "static" and self._config.registry.static_urls:
            return RegistryEndpoint(url=str(self._config.registry.static_urls[0]))

        endpoint = await self._discover_via_dnssd()
        if endpoint:
            return endpoint

        if self._config.registry.static_urls:
            LOGGER.info("Falling back to static registry list")
            return RegistryEndpoint(url=str(self._config.registry.static_urls[0]))
        return None

    async def _discover_via_dnssd(self) -> RegistryEndpoint | None:
        try:
            from zeroconf import ServiceBrowser, ServiceInfo, ServiceListener, Zeroconf
        except ImportError:
            LOGGER.warning("zeroconf not installed; DNS-SD discovery unavailable")
            return None

        loop = asyncio.get_running_loop()
        zeroconf = Zeroconf()
        future: asyncio.Future[RegistryEndpoint | None] = loop.create_future()

        class _Listener(ServiceListener):
            def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:  # noqa: D401
                return

            def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:  # noqa: D401
                return

            def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
                if future.done():
                    return
                info = zc.get_service_info(type_, name)
                if not info:
                    return
                endpoint = _service_info_to_endpoint(info)
                future.set_result(endpoint)

        browser = ServiceBrowser(zeroconf, SERVICE_TYPE, listener=_Listener())
        try:
            return await asyncio.wait_for(future, timeout=self._config.registry.dns_sd_timeout)
        except asyncio.TimeoutError:
            return None
        finally:
            browser.cancel()
            zeroconf.close()

    async def _register_resources(self) -> None:
        if not self._registry:
            return
        version = _utc_version()
        node = self._build_node_resource(version)
        device = self._build_device_resource(version)
        receiver = self._build_receiver_resource(version)

        try:
            await self._upsert_resource("nodes", self._identity.node_id, node)
            await self._upsert_resource("devices", self._identity.device_id, device)
            await self._upsert_resource("receivers", self._identity.receiver_id, receiver)
        except httpx.HTTPError as exc:
            LOGGER.warning("Failed to register resources: %s", exc)
            return
        self._registered = True
        LOGGER.info(
            "Registered Node %s / Device %s / Receiver %s",
            self._identity.node_id,
            self._identity.device_id,
            self._identity.receiver_id,
        )

    async def _upsert_resource(self, collection: str, resource_id: str, payload: Dict[str, Any]) -> None:
        if not self._registry:
            return
        url = f"{self._registry.url}/resource/{collection}"
        envelope = {
            "type": collection[:-1],
            "id": resource_id,
            "data": payload,
        }
        response = await self._client.post(url, json=envelope)
        if response.status_code in (200, 201, 202):
            return
        if response.status_code == 409:
            LOGGER.info("Resource %s already registered, updating", resource_id)
            url = f"{self._registry.url}/resource/{collection}/{resource_id}"
            response = await self._client.put(url, json=envelope)
            response.raise_for_status()
            return
        response.raise_for_status()

    def _build_node_resource(self, version: str) -> Dict[str, Any]:
        hostname = _hostname()
        node = {
            "id": self._identity.node_id,
            "version": version,
            "label": self._config.node_friendly_name,
            "description": f"AES67 receiver on {hostname}",
            "tags": {},
            "services": [],
            "controls": [],
            "caps": {},
            "interfaces": [
                {
                    "name": "eth0",
                    "chassis_id": hostname,
                    "port_id": "eth0",
                    "vlan_ids": [],
                }
            ],
            "hostname": hostname,
        }
        return node

    def _build_device_resource(self, version: str) -> Dict[str, Any]:
        return {
            "id": self._identity.device_id,
            "version": version,
            "label": self._config.device_friendly_name,
            "description": "AES67 mono receiver device",
            "type": "urn:x-nmos:device:generic",
            "node_id": self._identity.node_id,
            "controls": [],
            "receivers": [self._identity.receiver_id],
            "senders": [],
            "tags": {},
        }

    def _build_receiver_resource(self, version: str) -> Dict[str, Any]:
        return {
            "id": self._identity.receiver_id,
            "version": version,
            "label": self._config.receiver_friendly_name,
            "description": "Mono AES67 RTP receiver",
            "format": "urn:x-nmos:format:audio",
            "transport": "urn:x-nmos:transport:rtp",
            "device_id": self._identity.device_id,
            "caps": {},
            "subscription": {"sender_id": None, "active": False},
            "interface_bindings": ["eth0"],
            "tags": {},
        }

    async def _send_heartbeat(self) -> None:
        if not self._registry:
            return
        url = f"{self._registry.url}/health/nodes/{self._identity.node_id}"
        LOGGER.debug("Sending IS-04 heartbeat to %s", url)
        response = await self._client.post(url, json={})
        if response.status_code == 404:
            LOGGER.info("Registry lost our Node; re-registering soon")
            self._registry = None
            self._registered = False
            return
        response.raise_for_status()

    async def stop(self) -> None:
        self._stop.set()
        with suppress(asyncio.CancelledError):
            await self._client.aclose()


def _service_info_to_endpoint(info: Any) -> RegistryEndpoint:
    addresses = info.parsed_addresses()
    if not addresses:
        raise RuntimeError("Registry advertised without an address")
    address = addresses[0]
    port = info.port
    version = "v1.3"  # default assumption; controllers may redirect if needed
    url = f"http://{address}:{port}/x-nmos/registration/{version}"
    return RegistryEndpoint(url=url)


def _utc_version() -> str:
    return dt.datetime.utcnow().isoformat() + "Z"


def _hostname() -> str:
    try:
        return socket.gethostname()
    except Exception:  # pragma: no cover - fallback
        return "aes67-node"
