"""IS-04 registration + heartbeat worker."""
from __future__ import annotations

import asyncio
import datetime as dt
import logging
import os
import re
import socket
import time
from urllib.parse import urlparse
from contextlib import suppress
from dataclasses import dataclass
from typing import Any, Dict, Optional

import httpx

from service.config import AppConfig, NodeIdentity, SUPPORTED_CONNECTION_API_VERSIONS
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
        version = _tai_version()
        ptp_status = await self._fetch_ptp_status()
        node_clock = _ptp_clock_from_status(ptp_status)
        node = self._build_node_resource(version, node_clock)
        device = self._build_device_resource(version)
        receiver = self._build_receiver_resource(version)

        try:
            await self._upsert_resource("nodes", self._identity.node_id, node)
            await self._upsert_resource("devices", self._identity.device_id, device)
            await self._upsert_resource("receivers", self._identity.receiver_id, receiver)
        except httpx.HTTPStatusError as exc:
            details = exc.response.text
            LOGGER.warning("Failed to register resources: %s; response=%s", exc, details)
            return
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
        resource_type = collection[:-1]
        url = f"{self._registry.url}/resource"
        envelope = {
            "type": resource_type,
            "data": payload,
        }

        response = await self._client.post(url, json=envelope)
        if response.status_code in (200, 201, 202):
            return

        if response.status_code == 409:
            # Per IS-04 Registration API, resources are managed via POST /resource and DELETE/GET
            # on /resource/{resourceType}/{resourceId}. Some registries return 409 if the resource
            # already exists; delete+recreate is a safe prototype behaviour.
            delete_url = f"{self._registry.url}/resource/{resource_type}/{resource_id}"
            LOGGER.info("Resource %s already present; deleting %s then re-registering", resource_id, delete_url)
            delete_resp = await self._client.delete(delete_url)
            if delete_resp.status_code not in (200, 204, 404):
                delete_resp.raise_for_status()
            response = await self._client.post(url, json=envelope)
            response.raise_for_status()
            return

        response.raise_for_status()

    def _build_node_resource(self, version: str, clock: Dict[str, Any]) -> Dict[str, Any]:
        hostname = _hostname()
        interface_name = _detect_primary_interface(self._config.interface_name)
        port_id = _read_interface_mac(interface_name)
        chassis_id = None
        # Advertise the FastAPI host/port as a basic Node API endpoint.
        api_host = _detect_advertise_ip(self._registry.url) if self._registry else hostname
        api_port = int(os.environ.get("AES67_NMOS_HTTP_PORT", str(self._config.http_port)))
        node_api_versions = ["v1.3", "v1.2", "v1.1"]
        href = f"http://{api_host}:{api_port}/x-nmos/node/v1.3"
        node = {
            "id": self._identity.node_id,
            "version": version,
            "label": self._config.node_friendly_name,
            "description": f"AES67 receiver on {api_host}",
            "tags": {},
            "href": href,
            "api": {
                "versions": node_api_versions,
                "endpoints": [
                    {
                        "host": api_host,
                        "port": api_port,
                        "protocol": "http",
                        "authorization": False,
                    }
                ],
            },
            "services": [],
            "controls": [],
            "caps": {},
            "clocks": [clock],
            "interfaces": [
                {
                    "name": interface_name,
                    "chassis_id": chassis_id,
                    "port_id": port_id,
                }
            ],
            "hostname": api_host,
        }
        return node

    async def _fetch_ptp_status(self) -> dict[str, Any] | None:
        """Best-effort read of the daemon PTP status for IS-04 clock declaration."""

        url = f"{self._config.daemon.base_url}/api/ptp/status"
        try:
            response = await self._client.get(url)
            if response.status_code != 200:
                return None
            payload = response.json()
            if isinstance(payload, dict):
                return payload
            return None
        except Exception:
            return None

    def _build_device_resource(self, version: str) -> Dict[str, Any]:
        # Controllers discover the IS-05 Connection API via Device.controls
        # (type urn:x-nmos:control:sr-ctrl/{version}).
        api_host = _detect_advertise_ip(self._registry.url) if self._registry else _hostname()
        api_port = int(os.environ.get("AES67_NMOS_HTTP_PORT", str(self._config.http_port)))
        controls: list[Dict[str, Any]] = [
            {
                "href": f"http://{api_host}:{api_port}/x-nmos/connection/{conn_version}",
                "type": f"urn:x-nmos:control:sr-ctrl/{conn_version}",
            }
            for conn_version in SUPPORTED_CONNECTION_API_VERSIONS
        ]
        return {
            "id": self._identity.device_id,
            "version": version,
            "label": self._config.node_friendly_name,
            "description": "AES67 mono receiver device",
            "type": "urn:x-nmos:device:generic",
            "node_id": self._identity.node_id,
            "controls": controls,
            "receivers": [self._identity.receiver_id],
            "senders": [],
            "tags": {},
        }

    def _build_receiver_resource(self, version: str) -> Dict[str, Any]:
        interface_name = _detect_primary_interface(self._config.interface_name)
        return {
            "id": self._identity.receiver_id,
            "version": version,
            "label": self._config.node_friendly_name,
            "description": "Mono AES67 RTP receiver",
            "format": "urn:x-nmos:format:audio",
            "caps": {
                "media_types": ["audio/L24"],
            },
            "transport": "urn:x-nmos:transport:rtp.mcast",
            "device_id": self._identity.device_id,
            "subscription": {"sender_id": None, "active": False},
            "interface_bindings": [interface_name],
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
        if self._registered:
            with suppress(Exception):
                await self._delete_resources()
        with suppress(asyncio.CancelledError):
            await self._client.aclose()

    async def _delete_resources(self) -> None:
        if not self._registry:
            return

        await self._delete_resource("receivers", self._identity.receiver_id)
        await self._delete_resource("devices", self._identity.device_id)
        await self._delete_resource("nodes", self._identity.node_id)
        self._registered = False

    async def _delete_resource(self, collection: str, resource_id: str) -> None:
        if not self._registry:
            return
        resource_type = collection[:-1]
        url = f"{self._registry.url}/resource/{resource_type}/{resource_id}"
        response = await self._client.delete(url)
        if response.status_code not in (200, 204, 404):
            response.raise_for_status()


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


def _tai_version() -> str:
    """Return an IS-04 version string matching the required pattern ^[0-9]+:[0-9]+$.

    The spec describes this as a TAI timestamp. Many registries validate only the format,
    not absolute epoch correctness, so we use Unix time with nanoseconds for now.
    """

    ns = time.time_ns()
    seconds = ns // 1_000_000_000
    nanos = ns % 1_000_000_000
    return f"{seconds}:{nanos}"


def _hostname() -> str:
    try:
        return socket.gethostname()
    except Exception:  # pragma: no cover - fallback
        return "aes67-node"


def _detect_advertise_ip(registry_url: str) -> str:
    """Best-effort selection of an IP address to advertise in Node.api.endpoints.

    Strategy: open a UDP socket towards the registry host and use the chosen source IP.
    """

    try:
        parsed = urlparse(registry_url)
        host = parsed.hostname
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        if not host:
            return socket.gethostbyname(_hostname())
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect((host, port))
            return sock.getsockname()[0]
        finally:
            sock.close()
    except Exception:  # pragma: no cover
        return "127.0.0.1"


def _detect_primary_interface(preferred: str | None) -> str:
    """Choose an interface name to report in Node.interfaces.

    Prefer the first non-loopback interface that is 'up'. Fallback to 'eth0'.
    """

    if preferred:
        return preferred

    sys_net = "/sys/class/net"
    try:
        candidates = []
        for name in os.listdir(sys_net):
            if name == "lo":
                continue
            operstate_path = os.path.join(sys_net, name, "operstate")
            try:
                operstate = open(operstate_path, "r", encoding="utf-8").read().strip()
            except OSError:
                operstate = "unknown"
            candidates.append((0 if operstate == "up" else 1, name))
        candidates.sort()
        return candidates[0][1] if candidates else "eth0"
    except Exception:  # pragma: no cover
        return "eth0"


def _read_interface_mac(interface_name: str) -> str:
    """Return MAC address formatted with '-' separators (schema requirement)."""

    try:
        mac = open(f"/sys/class/net/{interface_name}/address", "r", encoding="utf-8").read().strip().lower()
        mac = mac.replace(":", "-")
        if re.match(r"^([0-9a-f]{2}-){5}([0-9a-f]{2})$", mac):
            return mac
    except OSError:
        pass
    # Fallback: schema requires MAC, so use a deterministic placeholder if unavailable
    return "00-00-00-00-00-00"


def _coerce_ptp_gmid(value: object) -> str:
    if not isinstance(value, str):
        return "00-00-00-00-00-00-00-00"
    candidate = value.strip().lower()
    if re.match(r"^([0-9a-f]{2}-){7}([0-9a-f]{2})$", candidate):
        return candidate
    return "00-00-00-00-00-00-00-00"


def _ptp_clock_from_status(ptp_status: dict[str, Any] | None) -> Dict[str, Any]:
    locked = bool(ptp_status and ptp_status.get("status") == "locked")
    gmid = _coerce_ptp_gmid(ptp_status.get("gmid") if ptp_status else None)
    return {
        "name": "clk0",
        "ref_type": "ptp",
        "traceable": locked,
        "version": "IEEE1588-2008",
        "gmid": gmid,
        "locked": locked,
    }
