# AirPlay Stream

[![Build](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml/badge.svg)](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/aomkoyo/obs-airplay-receiver)](https://github.com/aomkoyo/obs-airplay-receiver/releases)

**`airplay-stream.exe`** — a standalone Windows command-line tool that receives AirPlay screen mirroring from any Apple device and streams it to any browser at **< 100 ms latency** via a LiveKit SFU.

> **Built entirely with [Claude Code](https://claude.ai/code)** (Anthropic's AI coding agent). This project is a Windows port of [mika314/obs-airplay](https://github.com/mika314/obs-airplay), using [UxPlay](https://github.com/FDH2/UxPlay)'s battle-tested AirPlay 2 protocol library.

---

## How it works

```
iPhone/iPad/Mac  ──(AirPlay)──►  airplay-stream.exe
                                        │
                               H.264 + Opus (WHIP)
                                        │
                               ┌────────▼─────────┐
                               │  LiveKit Server   │  (Docker / local)
                               └────────┬─────────┘
                                        │  WebRTC subscribe
                               ┌────────▼─────────┐
                               │  Browser (JS SDK) │
                               └──────────────────┘
```

- **Video**: H.264 frames are passed directly into RTP — no decode/re-encode, minimal CPU, no quality loss.
- **Audio**: AAC-ELD audio is decoded, resampled to 48 kHz, and encoded to Opus for WebRTC delivery.
- **SFU reconnection**: The LiveKit JS SDK handles reconnection automatically — no page reload needed when the publisher restarts.
- **Hardware acceleration**: Pass `--hw-accel` to use Windows Media Foundation (`aac_mf`) for audio decode/encode.

## Requirements

- **Windows 10/11 64-bit**
- **Apple Bonjour** — Required for mDNS discovery so Apple devices can find the receiver.
  Install [iTunes](https://www.apple.com/itunes/) or the [Bonjour Print Services](https://support.apple.com/kb/DL999) package.
- **OpenSSL** — `libcrypto-3-x64.dll` (OpenSSL 3.x) or `libcrypto-4-x64.dll` (OpenSSL 4.x) must be present next to `airplay-stream.exe`.
  The easiest way to get it is the free **OpenSSL Light** installer:
  - OpenSSL 4.x: [Win64OpenSSL_Light-3_6_2.msi](https://slproweb.com/download/Win64OpenSSL_Light-3_6_2.msi)

  After installation, copy the matching `libcrypto-*-x64.dll` from the OpenSSL `bin\` folder next to `airplay-stream.exe`.
- **LiveKit server** — self-hosted via Docker (see [Quick Start](#quick-start) below).

## Quick Start

### 1. Start LiveKit

```bat
docker run --rm ^
  -p 7880:7880 -p 7881:7881 ^
  -p 50000-60000:50000-60000/udp ^
  livekit/livekit-server --dev
```

The `--dev` flag uses a stable key/secret pair: `APIKey=devkey` / `APISecret=secret` — perfect for local use.

### 2. Run airplay-stream

```bat
airplay-stream.exe --name "My Stream" --port 8888
```

### 3. Open in browser

Navigate to `http://localhost:8888/` — the LiveKit JS SDK player loads automatically.

## Installation

1. Download the latest release `.zip` from the [Releases](../../releases) page
2. Extract the zip to any folder
3. Start a LiveKit server (see Quick Start above)
4. Run `airplay-stream.exe` from the extracted folder

## Usage

```bat
airplay-stream.exe [options]
```

| Option | Default | Description |
|---|---|---|
| `--name <name>` | `AirPlay Stream` | Server name shown on Apple device |
| `--port <port>` | `8888` | HTTP port for the browser WebRTC player |
| `--livekit-url <url>` | `http://localhost:7880` | LiveKit server URL |
| `--api-key <key>` | `devkey` | LiveKit API key |
| `--api-secret <secret>` | `secret` | LiveKit API secret |
| `--width <px>` | device native | Requested video width |
| `--height <px>` | device native | Requested video height |
| `--fps <fps>` | `60` | Requested frame rate |
| `--hw-accel` | off | Enable hardware audio codec (Windows Media Foundation) |
| `--help` | | Show help and exit |

**Default (local LiveKit with dev credentials):**

```bat
airplay-stream.exe --name "My Stream" --port 8888
```

Then open `http://localhost:8888/` in any browser — no plugin required.

**Remote LiveKit server:**

```bat
airplay-stream.exe --name "My Stream" --port 8888 ^
    --livekit-url http://192.168.1.100:7880 ^
    --api-key mykey --api-secret mysecret
```

With hardware acceleration:

```bat
airplay-stream.exe --name "My Stream" --port 8888 --hw-accel
```

## Firewall

Allow the following through Windows Firewall:
- **TCP port 7000** (AirPlay)
- **TCP port `<port>`** (WebRTC viewer HTTP server, default 8888)
- **TCP port 7880** (LiveKit HTTP/WHIP — needed when LiveKit is remote)
- **TCP port 7881** (LiveKit WebSocket — needed when LiveKit is remote)
- **UDP port 5353** (mDNS/Bonjour)
- **UDP ports 50000–60000** (LiveKit media — needed when LiveKit is remote)

## Hardware Acceleration

Pass `--hw-accel` to enable hardware-accelerated audio processing:

- **Audio decode** (AAC-ELD → PCM): tries `aac_mf` (Windows Media Foundation) before falling back to `libfdk_aac` / software AAC.
- **Audio encode** (PCM → AAC-LC): tries `aac_mf` before falling back to software AAC.
- **Video**: H.264 is already passed through without decoding — no GPU needed and no quality loss.

`aac_mf` leverages hardware decoders/encoders available on Intel, AMD, and NVIDIA platforms via the Windows Media Foundation API. If the hardware codec is unavailable on your system, `airplay-stream.exe` automatically falls back to software processing.

## Building from Source

### Prerequisites

- Visual Studio 2019 or 2022 with C/C++ workload
- CMake 3.16+
- **OpenSSL** — install the **OpenSSL Light** package:
    - OpenSSL 4.x (recommended): [Win64OpenSSL_Light-4_0_0.msi](https://slproweb.com/download/Win64OpenSSL_Light-4_0_0.msi)
    - Or via Chocolatey: `choco install openssl`
    - Or via Scoop: `scoop install openssl`
- Git (for submodules)

### Steps

All commands below should be run from a **VS Developer Command Prompt**.

1. **Clone with submodules**
   ```bash
   git clone --recursive https://github.com/aomkoyo/obs-airplay-receiver.git
   cd obs-airplay-receiver
   ```

2. **Download dependencies** into the `deps/` directory:
   - **FFmpeg 7.1 headers** — extract to `deps/ffmpeg7-include/`
   - **FFmpeg import libs** — extract to `deps/obs-ffmpeg-libs/`
   - **libplist 2.7.0** — extract to `deps/libplist-2.7.0/`
   - **w32-pthreads** headers — extract to `deps/w32-pthreads/` (or use the OBS SDK path)
   - **dnssd.lib** — from Apple Bonjour SDK — copy to `deps/dnssd.lib`
   - **w32-pthreads.lib** — copy to `deps/w32-pthreads.lib`

3. **Build libplist**
   ```bat
   cd deps\libplist-2.7.0
   mkdir build && cd build
   cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
   nmake
   cd ..\..\..
   ```

4. **Build UxPlay libraries** (playfair, llhttp, airplay)
   ```bat
   cd deps\uxplay-build
   mkdir build && cd build
   cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
   nmake
   cd ..\..\..
   ```

5. **Build `airplay-stream.exe`** using the dedicated script:
   ```bat
   build-standalone.bat
   ```

   Or manually:
   ```bat
   mkdir build-standalone && cd build-standalone
   cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl ^
       -DBUILD_OBS_PLUGIN=OFF -DBUILD_STANDALONE=ON
   nmake
   ```

6. Output: `build-standalone\standalone\airplay-stream.exe`

## Troubleshooting

- **Device doesn't see the receiver** — Check that Bonjour is installed and Windows Firewall allows TCP 7000 and UDP 5353
- **Browser shows "Connecting…" forever** — Check that the LiveKit server is running (`docker ps`) and that port 7880 is reachable from the machine
- **"WHIP publish failed"** — LiveKit is not running or the URL / credentials are wrong; verify with `curl http://localhost:7880/` which should return a JSON health response
- **Video freezes after reconnect** — Normal; the publisher automatically reconnects within ~5 s; the browser SDK reconnects within ~2 s
- **`--hw-accel` has no effect** — Your system may not have a supported hardware codec; the tool automatically falls back to software processing
- **One AirPlay client at a time** — AirPlay screen mirroring supports a single connected device

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) — Open-source AirPlay 2 server (core protocol: FairPlay, pairing, encryption)
- [mika314/obs-airplay](https://github.com/mika314/obs-airplay) — Original OBS AirPlay plugin for Linux (inspiration and reference)
- [LiveKit](https://livekit.io) — Self-hosted WebRTC SFU with WHIP publish and battle-tested SDK reconnection
- [FFmpeg](https://ffmpeg.org/) — H.264 and AAC-ELD decoding, Opus encoding
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) — ICE / DTLS / SRTP transport
- [libplist](https://github.com/libimobiledevice/libplist) — Apple binary plist format
- [OpenSSL](https://www.openssl.org/) — Cryptography (AES, SHA, HMAC-SHA256 for JWT signing)

## Contributing

Contributions welcome! Please open an issue or pull request.

## License

LGPL-2.1 — same as UxPlay, which this project depends on. See [LICENSE](LICENSE).

