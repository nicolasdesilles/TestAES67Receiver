### TestAES67Receiver

Minimal headless **AES67/RAVENNA receiver CLI** built on `external/ravennakit/`.

### What it does

- **Queries an NMOS registry** (IS-04 Query API) to list RTP audio senders
- Lets you **select a sender**
- Fetches its **SDP** from `manifest_href`
- Starts a `rav::RavennaReceiver` and plays audio to **ALSA** via **PortAudio**

### Build prerequisites

- **CMake 3.22+**
- **C++17 toolchain**
- `python3` (for vcpkg bootstrap scripts)

On Armbian (Rock2A): also install ALSA headers for PortAudio:

- `libasound2-dev`

### Build (macOS)

From repo root:

```bash
./external/ravennakit/submodules/vcpkg/bootstrap-vcpkg.sh

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain.cmake \
  -DVCPKG_TARGET_TRIPLET=macos-arm64

cmake --build build -j
```

### Build (Armbian / Linux arm64)

Install basics:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config python3 libasound2-dev zip unzip
```

Build:

```bash
./external/ravennakit/submodules/vcpkg/bootstrap-vcpkg.sh

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain.cmake \
  -DVCPKG_TARGET_TRIPLET=linux-arm64

cmake --build build -j
```

### Run

First list audio devices (optional):

```bash
./build/apps/aes67_rx_cli/aes67_rx_cli --registry http://<REGISTRY_IP>:<PORT> --interfaces "<SBC_IP>" --list-audio-devices
```

Then receive:

```bash
./build/apps/aes67_rx_cli/aes67_rx_cli --registry http://<REGISTRY_IP>:<PORT> --interfaces "<SBC_IP>"
```

Notes:

- `--interfaces` must match a real system network interface selector that ravennakit can resolve (identifier/display name/MAC/IP).
- `--audio-device "<PortAudio device name>"` can be used to force a specific output device.
