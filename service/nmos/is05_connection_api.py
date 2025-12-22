"""IS-05 Connection API surface for the single AES67 receiver."""
from __future__ import annotations

import asyncio
import datetime as dt
import logging
from ipaddress import IPv4Address
from typing import Any, Dict, List

from fastapi import APIRouter, Body, Depends, HTTPException, Path, status
from pydantic import BaseModel, Field, IPvAnyAddress

from service.audio.amixer_control import AmixerController
from service.audio.alsaloop import AlsaLoopController
from service.config import AppConfig, SUPPORTED_CONNECTION_VERSIONS
from service.daemon.aes67d_client import AES67DaemonClient
from service.storage.json_store import JsonStateStore

LOGGER = logging.getLogger(__name__)
STATE_NAMESPACE = "receiver_state"


class ActivationParams(BaseModel):
    mode: str = Field("activate_immediate")
    requested_time: str | None = None


class AudioParams(BaseModel):
    volume: int = Field(80, ge=0, le=100)
    mute: bool = False


class TransportParams(BaseModel):
    destination_ip: IPvAnyAddress = Field(default=IPv4Address("239.0.0.1"))
    destination_port: int = Field(5004, ge=1, le=65535)
    source_ip: IPvAnyAddress | None = None
    interface_ip: IPvAnyAddress | None = None
    ttl: int = Field(64, ge=1, le=255)
    sample_rate: int = Field(48000, ge=8000, le=192000)
    encoding_name: str = Field("L16")
    payload_type: int = Field(96, ge=0, le=127)


def _default_transport_params() -> TransportParams:
    return TransportParams(
        destination_ip=IPv4Address("239.0.0.1"),
        destination_port=5004,
        ttl=64,
        sample_rate=48000,
        encoding_name="L16",
        payload_type=96,
    )


def _default_transport_params_list() -> List[TransportParams]:
    return [_default_transport_params()]


def _default_activation_params() -> ActivationParams:
    return ActivationParams(mode="activate_immediate")


def _default_audio_params() -> AudioParams:
    return AudioParams(volume=80, mute=False)


class StagedState(BaseModel):
    master_enable: bool = False
    transport_params: List[TransportParams] = Field(default_factory=_default_transport_params_list)
    activation: ActivationParams = Field(default_factory=_default_activation_params)
    audio: AudioParams = Field(default_factory=_default_audio_params)


class ReceiverState(BaseModel):
    staged: StagedState
    active: StagedState
    last_activated: str | None = None
    sink_active: bool = False


class ReceiverStateController:
    def __init__(self, store: JsonStateStore, default_volume: int) -> None:
        self._store = store
        self._lock = asyncio.Lock()
        self._default_volume = default_volume
        self._state = self._load_state()

    def _load_state(self) -> ReceiverState:
        payload = self._store.read_namespace(STATE_NAMESPACE)
        if not payload:
            staged = StagedState(audio=AudioParams(volume=self._default_volume))
            active = StagedState(**staged.model_dump())
            receiver_state = ReceiverState(staged=staged, active=active, sink_active=False)
            self._store.write_namespace(STATE_NAMESPACE, receiver_state.model_dump(mode="json"))
            return receiver_state
        return ReceiverState(**payload)

    async def snapshot(self) -> ReceiverState:
        async with self._lock:
            return ReceiverState(**self._state.model_dump())

    async def update_staged(self, patch: Dict[str, Any]) -> ReceiverState:
        async with self._lock:
            staged_dict = self._state.staged.model_dump()
            staged_dict.update(patch)
            staged = StagedState(**staged_dict)
            self._state.staged = staged
            await self._persist()
            return ReceiverState(**self._state.model_dump())

    async def commit_activation(self, sink_active: bool) -> ReceiverState:
        async with self._lock:
            self._state.active = self._state.staged
            self._state.last_activated = dt.datetime.utcnow().isoformat() + "Z"
            self._state.sink_active = sink_active
            await self._persist()
            return ReceiverState(**self._state.model_dump())

    async def _persist(self) -> None:
        self._store.write_namespace(STATE_NAMESPACE, self._state.model_dump(mode="json"))


class SDPBuilder:
    @staticmethod
    def build(params: TransportParams, stream_label: str) -> str:
        lines = [
            "v=0",
            f"o=- 0 0 IN IP4 {params.destination_ip}",
            f"s={stream_label}",
            "t=0 0",
            f"c=IN IP4 {params.destination_ip}/{params.ttl}",
            f"m=audio {params.destination_port} RTP/AVP {params.payload_type}",
            f"a=rtpmap:{params.payload_type} {params.encoding_name}/{params.sample_rate}/1",
        ]
        return "\r\n".join(lines) + "\r\n"


def build_router(
    config: AppConfig,
    store: JsonStateStore,
    daemon_client: AES67DaemonClient,
    alsaloop: AlsaLoopController,
    amixer: AmixerController,
) -> APIRouter:
    controller = ReceiverStateController(store, config.audio.default_volume)
    router = APIRouter()

    async def validate_version(version: str = Path(..., description="IS-05 version")) -> str:
        if version not in SUPPORTED_CONNECTION_VERSIONS:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Unsupported IS-05 version")
        return version

    async def ensure_receiver(receiver_id: str = Path(...)) -> str:
        # Single receiver implementation; identity ensured elsewhere
        return receiver_id

    @router.get("/x-nmos/connection/{version}/single/receivers/{receiver_id}/transporttype")
    async def get_transport_type(
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, str]:
        return {"type": "urn:x-nmos:transport:rtp.mcast"}

    @router.get("/x-nmos/connection/{version}/single/receivers/{receiver_id}/constraints")
    async def get_constraints(
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, Any]:
        return {
            "sample_rates": [48000],
            "channels": [1],
            "encodings": ["L16"],
            "destination_modes": ["multicast", "unicast"],
        }

    @router.get("/x-nmos/connection/{version}/single/receivers/{receiver_id}/staged")
    async def get_staged(
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, Any]:
        state = await controller.snapshot()
        return state.staged.model_dump()

    @router.patch("/x-nmos/connection/{version}/single/receivers/{receiver_id}/staged")
    async def patch_staged(
        payload: Dict[str, Any] = Body(...),
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, Any]:
        state = await controller.update_staged(payload)
        return state.staged.model_dump()

    @router.get("/x-nmos/connection/{version}/single/receivers/{receiver_id}/active")
    async def get_active(
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, Any]:
        state = await controller.snapshot()
        return state.active.model_dump()

    @router.post(
        "/x-nmos/connection/{version}/single/receivers/{receiver_id}/staged/activation",
        status_code=status.HTTP_202_ACCEPTED,
    )
    async def activate(
        version: str = Depends(validate_version),
        receiver_id: str = Depends(ensure_receiver),
    ) -> Dict[str, Any]:
        state = await controller.snapshot()
        staged = state.staged
        if not staged.master_enable:
            await daemon_client.delete_sink()
            await alsaloop.stop()
            await controller.commit_activation(False)
            return {"state": "disconnected"}

        params = staged.transport_params[0]
        sdp = SDPBuilder.build(params, config.receiver_friendly_name)
        payload = {
            "use_sdp": True,
            "sdp": sdp,
            "map": [0, 0],  # duplicate mono channel on both playback legs
            "delay": 0,
        }
        await daemon_client.upsert_sink(payload)
        await alsaloop.ensure_running()
        await amixer.set_volume(staged.audio.volume)
        await amixer.set_mute(staged.audio.mute)
        await controller.commit_activation(True)
        return {"state": "connected"}

    return router
