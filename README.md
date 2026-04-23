# AirPlay Stream

[![Build](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml/badge.svg)](https://github.com/aomkoyo/obs-airplay-receiver/actions/workflows/build.yml)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/aomkoyo/obs-airplay-receiver)](https://github.com/aomkoyo/obs-airplay-receiver/releases)

**`airplay-stream.exe`** — a standalone Windows command-line tool that receives AirPlay screen mirroring from any Apple device and re-streams it as MPEG-TS over TCP so you can open it directly in **VLC**, mpv, ffplay, or any compatible media player.

> **Built entirely with [Claude Code](https://claude.ai/code)** (Anthropic's AI coding agent). This project is a Windows port of [mika314/obs-airplay](https://github.com/mika314/obs-airplay), using [UxPlay](https://github.com/FDH2/UxPlay)'s battle-tested AirPlay 2 protocol library.

---

## How it works

```
iPhone/iPad/Mac  ──(AirPlay)──►  airplay-stream.exe  ──(TCP MPEG-TS)──►  VLC / mpv / ffplay
```

- **Video**: H.264 frames are passed directly into the MPEG-TS container — no decode/re-encode, minimal CPU, no quality loss.
- **Audio**: AAC-ELD audio is decoded and re-encoded as AAC-LC for maximum player compatibility.
- **Hardware acceleration**: Pass `--hw-accel` to use Windows Media Foundation (`aac_mf`) for audio decode/encode, offloading processing to Intel/AMD/NVIDIA hardware when available.

## Requirements

- **Windows 10/11 64-bit**
- **Apple Bonjour** — Required for mDNS discovery so Apple devices can find the receiver.
  Install [iTunes](https://www.apple.com/itunes/) or the [Bonjour Print Services](https://support.apple.com/kb/DL999) package.
- **OpenSSL** — `libcrypto-3-x64.dll` (OpenSSL 3.x) or `libcrypto-4-x64.dll` (OpenSSL 4.x) must be present next to `airplay-stream.exe`.
  The easiest way to get it is the free **OpenSSL Light** installer:
  - OpenSSL 4.x: [Win64OpenSSL_Light-4_0_0.msi](https://slproweb.com/download/Win64OpenSSL_Light-4_0_0.msi)
  - OpenSSL 3.x: [slproweb.com/products/Win32OpenSSL.html](https://slproweb.com/products/Win32OpenSSL.html)

  After installation, copy the matching `libcrypto-*-x64.dll` from the OpenSSL `bin\` folder next to `airplay-stream.exe`.

## Installation

1. Download the latest release `.zip` from the [Releases](../../releases) page
2. Extract the zip to any folder
3. Run `airplay-stream.exe` from the extracted folder

## Usage

```bat
airplay-stream.exe [options]
```

| Option | Default | Description |
|---|---|---|
| `--name <name>` | `AirPlay Stream` | Server name shown on Apple device |
| `--port <port>` | `8888` | TCP port for the MPEG-TS stream |
| `--webrtc-port <port>` | disabled | HTTP port for the built-in WebRTC player (< 100 ms latency) |
| `--width <px>` | device native | Requested video width |
| `--height <px>` | device native | Requested video height |
| `--fps <fps>` | `60` | Requested frame rate |
| `--hw-accel` | off | Enable hardware audio codec (Windows Media Foundation) |
| `--help` | | Show help and exit |

**Example:**

```bat
airplay-stream.exe --name "My Stream" --port 8888
```

With the WebRTC browser player (< 100 ms latency):

```bat
airplay-stream.exe --name "My Stream" --webrtc-port 3020
```

Then open `http://localhost:3020/` in any browser — no plugin required.

With hardware acceleration:

```bat
airplay-stream.exe --name "My Stream" --port 8888 --hw-accel
```

Then open in VLC:
- Command line: `vlc tcp://localhost:8888`
- GUI: **Media → Open Network Stream** → `tcp://localhost:8888`

Or with FFplay: `ffplay tcp://localhost:8888`

## Firewall

Allow the following through Windows Firewall:
- **TCP port 7000** (AirPlay)
- **TCP port `<stream-port>`** (MPEG-TS output, default 8888)
- **TCP port `<webrtc-port>`** (WebRTC HTTP signalling, when `--webrtc-port` is used)
- **UDP port 5353** (mDNS/Bonjour)

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
- **No audio** — VLC sometimes needs a moment to buffer; try pausing and unpausing
- **VLC can't connect** — Check that Windows Firewall allows the stream port (default TCP 8888); try `telnet localhost 8888`
- **One client at a time** — AirPlay screen mirroring supports a single connected device
- **`--hw-accel` has no effect** — Your system may not have a supported hardware codec; the tool automatically falls back to software processing

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) — Open-source AirPlay 2 server (core protocol: FairPlay, pairing, encryption)
- [mika314/obs-airplay](https://github.com/mika314/obs-airplay) — Original OBS AirPlay plugin for Linux (inspiration and reference)
- [FFmpeg](https://ffmpeg.org/) — H.264 and AAC-ELD decoding, AAC-LC encoding
- [libplist](https://github.com/libimobiledevice/libplist) — Apple binary plist format
- [OpenSSL](https://www.openssl.org/) — Cryptography (AES, SHA, Ed25519)

## Contributing

Contributions welcome! Please open an issue or pull request.

## License

LGPL-2.1 — same as UxPlay, which this project depends on. See [LICENSE](LICENSE).

