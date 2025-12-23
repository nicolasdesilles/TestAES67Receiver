"""IS-04 Node API (read-only) for this single-receiver node.

This is the API that controllers use to query the Node directly after discovering it
via the IS-04 registry.

We implement a minimal, read-only subset sufficient for a single Node/Device/Receiver.
"""

from __future__ import annotations

import logging
import os
import socket
import time
from typing import Any, Dict
from urllib.parse import urlparse

from fastapi import APIRouter, Depends, Path, status
from fastapi.responses import JSONResponse

from service.config import AppConfig, NodeIdentity, SUPPORTED_NODE_API_VERSIONS, SUPPORTED_CONNECTION_API_VERSIONS
from service.daemon.aes67d_client import AES67DaemonClient
from service.storage.json_store import JsonStateStore

LOGGER = logging.getLogger(__name__)


def _error_response(code: int, error: str, debug: str | None = None) -> JSONResponse:
    # Match IS-04 error schema: {code, error, debug}
    return JSONResponse(status_code=code, content={"code": code, "error": error, "debug": debug})


def _tai_version() -> str:
    ns = time.time_ns()
    seconds = ns // 1_000_000_000
    nanos = ns % 1_000_000_000
    return f"{seconds}:{nanos}"


def _detect_advertise_ip(registry_url: str) -> str:
    try:
        parsed = urlparse(registry_url)
        host = parsed.hostname
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        if not host:
            return socket.gethostbyname(socket.gethostname())
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect((host, port))
            return sock.getsockname()[0]
        finally:
            sock.close()
    except Exception:  # pragma: no cover
        return "127.0.0.1"


def _detect_advertise_ip_from_config(config: AppConfig) -> str:
    # Prefer using the configured registry URL (static) so we advertise the same IP
    # the kernel would use to reach the registry.
    if config.registry.static_urls:
        return _detect_advertise_ip(str(config.registry.static_urls[0]))
    # Otherwise fall back to hostname resolution.
    try:
        return socket.gethostbyname(socket.gethostname())
    except Exception:  # pragma: no cover
        return "127.0.0.1"


def _read_interface_mac(interface_name: str) -> str:
    try:
        mac = open(f"/sys/class/net/{interface_name}/address", "r", encoding="utf-8").read().strip().lower()
        return mac.replace(":", "-")
    except OSError:
        return "00-00-00-00-00-00"


def _coerce_ptp_gmid(value: object) -> str:
    if not isinstance(value, str):
        return "00-00-00-00-00-00-00-00"
    candidate = value.strip().lower()
    # Keep it simple: accept already correctly formatted GMID.
    parts = candidate.split("-")
    if len(parts) == 8 and all(len(p) == 2 for p in parts):
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


def _interface_name(config: AppConfig) -> str:
    # For our SBC setup we expect this to be configured (e.g. end1).
    return config.interface_name or "eth0"


def _build_node(config: AppConfig, identity: NodeIdentity, clock: Dict[str, Any]) -> Dict[str, Any]:
    version = _tai_version()
    iface = _interface_name(config)

    api_host = _detect_advertise_ip_from_config(config)
    api_port = int(os.environ.get("AES67_NMOS_HTTP_PORT", str(config.http_port)))

    return {
        "id": identity.node_id,
        "version": version,
        "label": config.node_friendly_name,
        "description": f"AES67 receiver on {api_host}",
        "tags": {},
        "href": f"http://{api_host}:{api_port}/x-nmos/node/v1.3",
        "api": {
            "versions": ["v1.3", "v1.2", "v1.1"],
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
                "name": iface,
                "chassis_id": None,
                "port_id": _read_interface_mac(iface),
            }
        ],
        "hostname": api_host,
    }


def _build_device(config: AppConfig, identity: NodeIdentity) -> Dict[str, Any]:
    version = _tai_version()
    api_host = _detect_advertise_ip_from_config(config)
    api_port = int(os.environ.get("AES67_NMOS_HTTP_PORT", str(config.http_port)))
    controls: list[Dict[str, Any]] = []
    # Advertise IS-05 Connection API discovery via Device.controls
    for conn_version in SUPPORTED_CONNECTION_API_VERSIONS:
        controls.append(
            {
                "href": f"http://{api_host}:{api_port}/x-nmos/connection/{conn_version}/",
                "type": f"urn:x-nmos:control:sr-ctrl/{conn_version}",
                "authorization": False,
            }
        )
    return {
        "id": identity.device_id,
        "version": version,
        "label": config.node_friendly_name,
        "description": "AES67 mono receiver device",
        "type": "urn:x-nmos:device:generic",
        "node_id": identity.node_id,
        "controls": controls,
        "receivers": [identity.receiver_id],
        "senders": [],
        "tags": {},
    }


def _build_receiver(config: AppConfig, identity: NodeIdentity, store: JsonStateStore) -> Dict[str, Any]:
    version = _tai_version()
    iface = _interface_name(config)

    # Best-effort: reflect current active state from IS-05 persistence.
    receiver_state = store.read_namespace("receiver_state")
    active = bool(receiver_state.get("sink_active"))

    return {
        "id": identity.receiver_id,
        "version": version,
        "label": config.node_friendly_name,
        "description": "Mono AES67 RTP receiver",
        "format": "urn:x-nmos:format:audio",
        "caps": {"media_types": ["audio/L24"]},
        "transport": "urn:x-nmos:transport:rtp.mcast",
        "device_id": identity.device_id,
        "subscription": {"sender_id": None, "active": active},
        "interface_bindings": [iface],
        "tags": {},
    }


def build_node_api_router(
    config: AppConfig,
    identity: NodeIdentity,
    store: JsonStateStore,
    daemon_client: AES67DaemonClient,
) -> APIRouter:
    router = APIRouter()

    async def validate_version(version: str = Path(..., description="IS-04 Node API version")) -> str:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            # Spec-defined error body.
            # Note: 404 is acceptable for unsupported versions in this prototype.
            return version
        return version

    def get_clock() -> Dict[str, Any]:
        # Default clock object (unlocked) until we can query daemon.
        return _ptp_clock_from_status(None)

    async def build_clock_async() -> Dict[str, Any]:
        try:
            ptp = await daemon_client.fetch_ptp_status()
            return _ptp_clock_from_status(ptp if isinstance(ptp, dict) else None)
        except Exception:
            return get_clock()

    @router.get("/x-nmos/node/{version}")
    @router.get("/x-nmos/node/{version}/")
    async def get_base(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        # Per IS-04 schema nodeapi-base.json
        return ["self/", "sources/", "flows/", "devices/", "senders/", "receivers/"]

    @router.get("/x-nmos/node/{version}/self")
    async def get_self(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        clock = await build_clock_async()
        return _build_node(config, identity, clock)

    @router.get("/x-nmos/node/{version}/devices")
    async def list_devices(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return [_build_device(config, identity)]

    @router.get("/x-nmos/node/{version}/devices/{device_id}")
    async def get_device(
        device_id: str,
        version: str = Depends(validate_version),
    ) -> Any:
        if device_id != identity.device_id:
            return _error_response(status.HTTP_404_NOT_FOUND, "Device not found")
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return _build_device(config, identity)

    @router.get("/x-nmos/node/{version}/sources")
    async def list_sources(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return []

    @router.get("/x-nmos/node/{version}/sources/{source_id}")
    async def get_source(source_id: str, version: str = Depends(validate_version)) -> JSONResponse:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return _error_response(status.HTTP_404_NOT_FOUND, "Source not found")

    @router.get("/x-nmos/node/{version}/flows")
    async def list_flows(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return []

    @router.get("/x-nmos/node/{version}/flows/{flow_id}")
    async def get_flow(flow_id: str, version: str = Depends(validate_version)) -> JSONResponse:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return _error_response(status.HTTP_404_NOT_FOUND, "Flow not found")

    @router.get("/x-nmos/node/{version}/senders")
    async def list_senders(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return []

    @router.get("/x-nmos/node/{version}/senders/{sender_id}")
    async def get_sender(sender_id: str, version: str = Depends(validate_version)) -> JSONResponse:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return _error_response(status.HTTP_404_NOT_FOUND, "Sender not found")

    @router.get("/x-nmos/node/{version}/receivers")
    async def list_receivers(version: str = Depends(validate_version)) -> Any:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return [_build_receiver(config, identity, store)]

    @router.get("/x-nmos/node/{version}/receivers/{receiver_id}")
    async def get_receiver(
        receiver_id: str,
        version: str = Depends(validate_version),
    ) -> Any:
        if receiver_id != identity.receiver_id:
            return _error_response(status.HTTP_404_NOT_FOUND, "Receiver not found")
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        return _build_receiver(config, identity, store)

    @router.options("/x-nmos/node/{version}/receivers/{receiver_id}/target")
    async def options_receiver_target(receiver_id: str, version: str = Depends(validate_version)) -> JSONResponse:
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        if receiver_id != identity.receiver_id:
            return _error_response(status.HTTP_404_NOT_FOUND, "Receiver not found")
        return JSONResponse(status_code=status.HTTP_200_OK, content={})

    @router.put("/x-nmos/node/{version}/receivers/{receiver_id}/target")
    async def put_receiver_target(
        receiver_id: str,
        payload: Dict[str, Any],
        version: str = Depends(validate_version),
    ) -> JSONResponse:
        # This endpoint is deprecated in IS-04, but required to exist.
        if version not in SUPPORTED_NODE_API_VERSIONS:
            return _error_response(status.HTTP_404_NOT_FOUND, "Unsupported IS-04 version")
        if receiver_id != identity.receiver_id:
            return _error_response(status.HTTP_404_NOT_FOUND, "Receiver not found")
        return _error_response(status.HTTP_501_NOT_IMPLEMENTED, "Receiver target subscription is not implemented")

    return router
