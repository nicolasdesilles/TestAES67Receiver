# AES67 NMOS Wrapper

Python-based microservice that registers a mono AES67 receiver with an NMOS IS-04 registry, exposes the IS-05 single receiver API, and controls a local `aes67-linux-daemon` sink plus ALSA plumbing on an Armbian SBC.

## Features (planned)

- Registry discovery via DNS-SD (`_nmos-registration._tcp`) with configurable static fallback list.
- IS-04 Node/Device/Receiver registration for versions v1.1–v1.3, including heartbeats and re-registration.
- IS-05 `/constraints`, `/transporttype`, `/staged`, `/active` endpoints for a single receiver, translating activations into daemon sink changes.
- ALSA routing (card2 capture → card1 playback) managed with `alsaloop`, plus volume/mute via `amixer`.
- JSON state persistence to survive reboots (UUIDs, staged params, audio settings).

## Repository Layout

```
service/
  main.py                 # Entrypoint for FastAPI app and worker tasks
  config.py               # Configuration models + discovery settings
  storage/json_store.py   # Lightweight JSON persistence utilities
  nmos/
    is04_registration.py  # Registration + heartbeat worker
    is05_connection_api.py# IS-05 REST resources + validation
  daemon/
    aes67d_client.py      # HTTP client for aes67-linux-daemon
  audio/
    alsaloop.py           # ALSA loopback process control
    amixer_control.py     # Volume/mute helpers
```

## Development Notes

- Target runtime: Python 3.10+ (tested on macOS dev, deployed on Armbian SBC).
- Install dependencies with `uv pip install -r requirements.txt` or `pip install -e .` once virtual environment is prepared.
- Run the API locally via `uvicorn service.main:app --reload` (macOS) for integration testing against mocked daemon endpoints and your NMOS registry.

## Configuration

Create a `config.yaml` beside `pyproject.toml` to override defaults. Example:

```yaml
node_friendly_name: "Studio Receiver 01"
interface_name: "end1" # network interface to bind in NMOS resources
http_port: 8000 # wrapper listen port advertised in IS-04
registry:
  mode: dns-sd # auto-discover `_nmos-registration._tcp`
  static_urls: # optional fallbacks if discovery fails
    - "http://registry.local:3210/x-nmos/registration/v1.3"
  heartbeat_interval: 5
daemon:
  base_url: "http://127.0.0.1:8080" # must match where aes67-linux-daemon listens (often "http://<sbc-ip>:8080")
  sink_id: 0
audio:
  capture_device: "hw:2,0" # aes67-linux-daemon capture side
  playback_device: "hw:1,0" # headphone out on the SBC
  alsaloop_buffer_ms: 50
  amixer_card: "1"
  amixer_controls:
    - "DAC LEFT LINEOUT"
    - "DAC RIGHT LINEOUT"
  default_volume: 80
```

**DNS-SD vs static discovery**

- _DNS-SD_ mode browses the LAN for `_nmos-registration._tcp` advertisements (via mDNS or unicast DNS) and automatically follows registry changes.
- _Static_ mode skips discovery and uses the configured `static_urls` list. Provide at least one fully qualified Registration API base URL.

## Running on Armbian SBC

1. **OS packages**

```bash
sudo apt update
sudo apt install -y python3-venv python3-pip alsa-utils alsa-loopback
```

Ensure `aes67-linux-daemon` is already installed and reachable at the configured `daemon.base_url`.

Tip: on the SBC, confirm which address the daemon is bound to using `ss -ltnp | grep ':8080'`. If it listens on the LAN IP (e.g. `192.168.x.y:8080`) and not on `127.0.0.1:8080`, set `daemon.base_url: "http://192.168.x.y:8080"` in `config.yaml`.

2. **Clone + install**

```bash
git clone <your-repo-url> ~/aes67-nmos-wrapper
cd ~/aes67-nmos-wrapper
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -e .
```

3. **Configure**

- Copy the sample `config.yaml` above, adjust ALSA card indices (`aplay -l`, `arecord -l` outputs) and registry URLs.
- Verify `alsaloop` can open `hw:2,0 → hw:1,0` and `amixer -c 1 scontrols` lists both mixer controls.

4. **Run service**

```bash
source .venv/bin/activate
uvicorn service.main:app --host 0.0.0.0 --port 8000
```

Logs should show registry discovery, IS-04 heartbeats, and IS-05 endpoint readiness.

5. **Optional systemd unit** (run as `aes67` user)

```ini
[Unit]
Description=AES67 NMOS Wrapper
After=network-online.target

[Service]
WorkingDirectory=/home/aes67/aes67-nmos-wrapper
Environment="AES67_NMOS_CONFIG=/home/aes67/aes67-nmos-wrapper/config.yaml"
ExecStart=/home/aes67/aes67-nmos-wrapper/.venv/bin/uvicorn service.main:app --host 0.0.0.0 --port 8000
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Reload systemd (`sudo systemctl daemon-reload`), enable (`sudo systemctl enable --now aes67-nmos.service`).

6. **Validation steps**

- Use `curl http://localhost:8000/health/live` to check liveness.
- Monitor `journalctl -u aes67-nmos.service -f` for registry heartbeats and activation events.
- From a controller, PATCH the IS-05 `/staged` endpoint and activate to confirm `aes67-linux-daemon` sink 0 receives the generated SDP (`/api/sink/status/0`).

## Wrapper HTTP API (port 8000)

The wrapper exposes a small HTTP API (default port 8000):

- Health
  - `GET /health/live`
  - `GET /health/ready`
- NMOS IS-05 Connection API (single receiver)

  - Base traversal
    - `GET /x-nmos/connection/{version}`
    - `GET /x-nmos/connection/{version}/single/receivers`
    - `GET /x-nmos/connection/{version}/single/receivers/{receiver_id}`
  - Single receiver control
    - `GET /x-nmos/connection/{version}/single/receivers/{receiver_id}/transporttype`
    - `GET /x-nmos/connection/{version}/single/receivers/{receiver_id}/constraints`
    - `GET /x-nmos/connection/{version}/single/receivers/{receiver_id}/staged`
    - `PATCH /x-nmos/connection/{version}/single/receivers/{receiver_id}/staged`
    - `GET /x-nmos/connection/{version}/single/receivers/{receiver_id}/active`
    - `POST /x-nmos/connection/{version}/single/receivers/{receiver_id}/staged/activation`

- NMOS IS-04 Node API (read-only)
  - Base traversal
    - `GET /x-nmos/node/{version}/`
    - `GET /x-nmos/node/{version}/self`
  - Resources
    - `GET /x-nmos/node/{version}/devices`
    - `GET /x-nmos/node/{version}/devices/{device_id}`
    - `GET /x-nmos/node/{version}/receivers`
    - `GET /x-nmos/node/{version}/receivers/{receiver_id}`
    - `OPTIONS /x-nmos/node/{version}/receivers/{receiver_id}/target`
    - `PUT /x-nmos/node/{version}/receivers/{receiver_id}/target` (returns 501; deprecated in IS-04)
    - `GET /x-nmos/node/{version}/senders` (empty)
    - `GET /x-nmos/node/{version}/senders/{sender_id}` (404)
    - `GET /x-nmos/node/{version}/sources` (empty)
    - `GET /x-nmos/node/{version}/sources/{source_id}` (404)
    - `GET /x-nmos/node/{version}/flows` (empty)
    - `GET /x-nmos/node/{version}/flows/{flow_id}` (404)

Notes:

- IS-04 Node API `{version}` supports `v1.1`, `v1.2`, `v1.3`.
- IS-05 Connection API `{version}` supports `v1.0`, `v1.1`.
- `{receiver_id}` is the stable UUID persisted in `state/runtime.json` and registered in IS-04.
