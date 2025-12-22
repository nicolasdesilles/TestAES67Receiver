"""Configuration models and helpers for the AES67 NMOS wrapper."""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Literal, cast

import yaml
from pydantic import AliasChoices, AnyHttpUrl, BaseModel, Field, field_validator

from service.storage.json_store import JsonStateStore

DEFAULT_CONFIG_PATH = Path(os.environ.get("AES67_NMOS_CONFIG", "config.yaml"))
SUPPORTED_CONNECTION_VERSIONS = ("v1.3", "v1.2", "v1.1")


class RegistryConfig(BaseModel):
    """Settings for NMOS registration discovery and cadence."""

    mode: Literal["dns-sd", "static"] = Field(
        "dns-sd",
        description=(
            "Discovery strategy. DNS-SD auto-detects registries advertising "
            "_nmos-registration._tcp, while static relies on configured URLs."
        ),
    )
    static_urls: list[AnyHttpUrl] = Field(default_factory=list)
    versions: list[str] = Field(default_factory=lambda: list(SUPPORTED_CONNECTION_VERSIONS))
    heartbeat_interval: float = Field(5.0, gt=0, description="Seconds between registration heartbeats")
    dns_sd_timeout: float = Field(3.0, gt=0, description="Seconds to wait for DNS-SD browse results")


_DEFAULT_DAEMON_URL = cast(AnyHttpUrl, "http://127.0.0.1:8080")
DEFAULT_MIXER_CONTROLS = ["DAC LEFT LINEOUT", "DAC RIGHT LINEOUT"]


class DaemonConfig(BaseModel):
    base_url: AnyHttpUrl = Field(
        _DEFAULT_DAEMON_URL,
        description="Base URL of the local aes67-linux-daemon HTTP API",
    )
    sink_id: int = Field(0, ge=0, description="Sink identifier to manage on the daemon")


class AudioConfig(BaseModel):
    capture_device: str = Field("hw:2,0", description="ALSA capture device (daemon-provided)")
    playback_device: str = Field("hw:1,0", description="ALSA playback device (headphone jack)")
    alsaloop_buffer_ms: int = Field(50, ge=10, le=500, description="alsaloop latency buffer in milliseconds")
    amixer_card: str = Field("1", description="amixer -c <card> target")
    amixer_controls: list[str] = Field(
        default_factory=lambda: list(DEFAULT_MIXER_CONTROLS),
        validation_alias=AliasChoices("amixer_controls", "amixer_control"),
        description="Mixer control names for volume/mute (each control is updated in sequence)",
    )
    default_volume: int = Field(80, ge=0, le=100)

    @field_validator("amixer_controls", mode="before")
    @classmethod
    def _coerce_controls(cls, value: object) -> list[str]:
        if value is None:
            return list(DEFAULT_MIXER_CONTROLS)
        if isinstance(value, str):
            return [value]
        if isinstance(value, (list, tuple, set)):
            return [str(item) for item in value]
        return list(DEFAULT_MIXER_CONTROLS)


def _default_registry_config() -> RegistryConfig:
    return RegistryConfig(
        mode="dns-sd",
        static_urls=[],
        versions=list(SUPPORTED_CONNECTION_VERSIONS),
        heartbeat_interval=5.0,
        dns_sd_timeout=3.0,
    )


def _default_daemon_config() -> DaemonConfig:
    return DaemonConfig(base_url=_DEFAULT_DAEMON_URL, sink_id=0)


def _default_audio_config() -> AudioConfig:
    return AudioConfig(
        capture_device="hw:2,0",
        playback_device="hw:1,0",
        alsaloop_buffer_ms=50,
        amixer_card="1",
        amixer_controls=list(DEFAULT_MIXER_CONTROLS),
        default_volume=80,
    )


class AppConfig(BaseModel):
    node_friendly_name: str = Field("AES67 Receiver", description="Human-readable Node label")
    device_friendly_name: str = Field("AES67 Device", description="Human-readable Device label")
    receiver_friendly_name: str = Field("AES67 Mono Receiver", description="Receiver label")
    registry: RegistryConfig = Field(default_factory=_default_registry_config)
    daemon: DaemonConfig = Field(default_factory=_default_daemon_config)
    audio: AudioConfig = Field(default_factory=_default_audio_config)
    state_file: Path = Field(Path("./state/runtime.json"))

    class Config:
        arbitrary_types_allowed = True

    def describe_discovery(self) -> str:
        if self.registry.mode == "dns-sd":
            return (
                "DNS-SD discovery will browse for _nmos-registration._tcp services. "
                "Static registry URLs serve as optional fallback if provided."
            )
        return (
            "Static discovery mode limits the wrapper to configured registry URLs. "
            "DNS-SD is skipped entirely in this configuration."
        )


class NodeIdentity(BaseModel):
    node_id: str
    device_id: str
    receiver_id: str


def load_config(path: Path | str | None = None) -> AppConfig:
    config_path = Path(path) if path else DEFAULT_CONFIG_PATH
    if config_path.exists():
        with config_path.open("r", encoding="utf-8") as infile:
            payload = yaml.safe_load(infile) or {}
    else:
        payload = {}
    settings = AppConfig(**payload)
    # Ensure state directory exists early
    settings.state_file.parent.mkdir(parents=True, exist_ok=True)
    return settings


def ensure_identity(store: JsonStateStore) -> NodeIdentity:
    """Guarantee deterministic UUIDs for Node/Device/Receiver objects."""

    node_id = store.get_or_create_uuid("node_id")
    device_id = store.get_or_create_uuid("device_id")
    receiver_id = store.get_or_create_uuid("receiver_id")
    return NodeIdentity(node_id=node_id, device_id=device_id, receiver_id=receiver_id)


def load_runtime_state(store: JsonStateStore, namespace: str, default: dict) -> dict:
    state = store.read_namespace(namespace)
    if not state:
        store.write_namespace(namespace, default)
        return json.loads(json.dumps(default))
    return state
