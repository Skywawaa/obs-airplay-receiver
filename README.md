# AirPlay Stream

[![Build](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml/badge.svg)](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/aomkoyo/obs-airplay-receiver)](https://github.com/aomkoyo/obs-airplay-receiver/releases)

**`airplay-stream.exe`** — a standalone Windows command-line tool that receives AirPlay screen mirroring from any Apple device and streams it to any browser at **< 100 ms latency** via a **mediasoup SFU**.

> **Built entirely with [Claude Code](https://claude.ai/code)** (Anthropic's AI coding agent). This project is a Windows port of [mika314/obs-airplay](https://github.com/mika314/obs-airplay), using [UxPlay](https://github.com/FDH2/UxPlay)'s battle-tested AirPlay 2 protocol library.

---

## How it works

```
iPhone/iPad/Mac  ──(AirPlay)──►  airplay-stream.exe
                                        │
                               H.264 + Opus (plain UDP RTP)
                                        │
                               ┌────────▼─────────┐
                               │  mediasoup server  │  (Node.js, localhost)
                               │   (SFU + signaling)│
                               └────────┬─────────┘
                                        │  WebRTC (DTLS/SRTP)
                               ┌────────▼─────────┐
                               │ Browser(s)        │
                               └──────────────────┘
```

- **Video**: H.264 frames are packetised directly into plain RTP — no decode/re-encode, minimal CPU, no quality loss.
- **Audio**: AAC-ELD audio is decoded, resampled to 48 kHz, and encoded to Opus for WebRTC delivery.
- **SFU**: mediasoup handles all WebRTC complexity (ICE, DTLS, SRTP) for browsers. Any number of browsers can watch simultaneously.
- **No external service**: everything runs locally — no Docker, no cloud.

## Requirements

- **Windows 10/11 64-bit**
- **Node.js >= 18** — Required to run the mediasoup SFU server. Download from [nodejs.org](https://nodejs.org/).
- **Apple Bonjour** — Required for mDNS discovery so Apple devices can find the receiver.
  Install [iTunes](https://www.apple.com/itunes/) or the [Bonjour Print Services](https://support.apple.com/kb/DL999) package.
- **OpenSSL** — `libcrypto-3-x64.dll` / `libcrypto-4-x64.dll` must be present next to `airplay-stream.exe` (required by the AirPlay crypto layer). The DLL is included in the release zip.

## Quick Start

### 1. Start the mediasoup SFU server

```bat
cd mediasoup-server
npm install
node server.js
```

Leave this terminal open while streaming.

### 2. Run airplay-stream

In a second terminal:

```bat
airplay-stream.exe --name "My Stream" --port 8888
```

The `--port` value must match the `PORT` used by the mediasoup server (default: `8888`).

### 3. Mirror your screen

On your iPhone/iPad/Mac, open Control Center → Screen Mirroring and tap **"My Stream"** (or whatever `--name` you chose).

### 4. Open in browser

Navigate to `http://localhost:8888/` — the player loads automatically.

## Installation

1. Download the latest release `.zip` from the [Releases](../../releases) page
2. Extract the zip to any folder
3. Install [Node.js >= 18](https://nodejs.org/) if not already present
4. Follow the **Quick Start** above

## Usage

```bat
airplay-stream.exe [options]
```

| Option | Default | Description |
|---|---|---|
| `--name <name>` | `AirPlay Stream` | Server name shown on Apple device |
| `--port <port>` | `8888` | Port of the mediasoup server (must match `PORT` env var) |
| `--width <px>` | device native | Requested video width |
| `--height <px>` | device native | Requested video height |
| `--fps <fps>` | `60` | Requested frame rate |
| `--hw-accel` | off | Enable hardware audio codec (Windows Media Foundation) |
| `--help` | | Show help and exit |

### mediasoup server environment variables

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8888` | HTTP port for browser player and signalling |
| `ANNOUNCED_IP` | `127.0.0.1` | ICE candidate IP. Set to your LAN IP for LAN access. |
| `RTC_MIN_PORT` | `40000` | Lower bound of UDP port range for mediasoup |
| `RTC_MAX_PORT` | `40100` | Upper bound of UDP port range for mediasoup |

**LAN access example** (browsers on other devices):

```bat
REM Terminal 1 — mediasoup server
cd mediasoup-server
set ANNOUNCED_IP=192.168.1.100
node server.js

REM Terminal 2 — AirPlay receiver
airplay-stream.exe --name "My Stream" --port 8888
```

Then open `http://192.168.1.100:8888/` on the remote device.

## Firewall

Allow the following through Windows Firewall:

- **TCP port 7000** (AirPlay)
- **TCP port `<PORT>`** (mediasoup HTTP + WebSocket, default 8888)
- **UDP ports 40000–40100** (mediasoup RTP/ICE, default range)
- **UDP port 5353** (mDNS/Bonjour)

## Hardware Acceleration

Pass `--hw-accel` to enable hardware-accelerated audio processing:

- **Audio decode** (AAC-ELD → PCM): tries `aac_mf` (Windows Media Foundation) first.
- **Video**: H.264 is passed through without decoding — no GPU needed, no quality loss.

## Building from Source

### Prerequisites

- Visual Studio 2019 or 2022 with C/C++ workload
- CMake 3.16+
- Node.js >= 18 (for the mediasoup server)
- **OpenSSL** — `choco install openssl` or from [slproweb.com](https://slproweb.com/)
- Git

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
   - **w32-pthreads** headers — extract to `deps/w32-pthreads/`
   - **dnssd.lib** — from Apple Bonjour SDK — copy to `deps/dnssd.lib`
   - **w32-pthreads.lib** — copy to `deps/w32-pthreads.lib`

3. **Build `airplay-stream.exe`**:
   ```bat
   mkdir build-standalone && cd build-standalone
   cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl ^
       -DBUILD_OBS_PLUGIN=OFF -DBUILD_STANDALONE=ON
   nmake
   ```

4. **Install mediasoup server dependencies**:
   ```bat
   cd standalone\mediasoup-server
   npm install
   ```

5. Output: `build-standalone\standalone\airplay-stream.exe`

## Troubleshooting

- **Device doesn't see the receiver** — Check that Bonjour is installed and Windows Firewall allows TCP 7000 and UDP 5353.
- **Browser shows "Connecting…" forever** — Make sure `node server.js` is running in the `mediasoup-server/` directory.
- **`airplay-stream.exe` shows "Waiting for mediasoup server"** — Start `node server.js` first and make sure `--port` matches `PORT`.
- **Video freezes after reconnect** — The app injects a cached IDR keyframe automatically; video should resume within a second.
- **`--hw-accel` has no effect** — Your system may not have a supported hardware codec; falls back to software automatically.
- **One AirPlay client at a time** — AirPlay screen mirroring supports a single connected device.

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) — Open-source AirPlay 2 server (core protocol: FairPlay, pairing, encryption)
- [mika314/obs-airplay](https://github.com/mika314/obs-airplay) — Original OBS AirPlay plugin for Linux (inspiration and reference)
- [mediasoup](https://mediasoup.org/) — Self-hosted WebRTC SFU (server-side media routing)
- [mediasoup-client](https://github.com/versatica/mediasoup-client) — Browser-side WebRTC SDK for mediasoup
- [FFmpeg](https://ffmpeg.org/) — AAC-ELD decoding, Opus encoding, SWR resampling
- [libplist](https://github.com/libimobiledevice/libplist) — Apple binary plist format
- [OpenSSL](https://www.openssl.org/) — AirPlay cryptography (AES, SHA, pairing)

## Contributing

Contributions welcome! Please open an issue or pull request.

## License

LGPL-2.1 — same as UxPlay, which this project depends on. See [LICENSE](LICENSE).

