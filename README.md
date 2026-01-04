# Loudness Balance Monitor (音量バランスモニター)

OBS Studio plugin for real-time audio loudness monitoring. Detects when your voice is too quiet or buried in BGM during streaming.

## Features

- **Voice Activity Detection (VAD)**: Threshold-based detection with attack/release timing
- **LUFS Measurement**: Short-term loudness (3-second window) using libebur128
- **Balance Monitoring**: Voice-BGM delta with OK/WARN/BAD status indicators
- **Mix Loudness**: Overall loudness level monitoring
- **Peak/Clip Detection**: Sample peak monitoring to prevent clipping
- **Qt Dock UI**: Integrated OBS dock with meters and status colors

## Build Requirements

| Platform | Tool |
|----------|------|
| Windows | Visual Studio 17 2022 |
| macOS | XCode 16.0 |
| Windows, macOS | CMake 3.30+ |
| Ubuntu 24.04 | CMake 3.28+, ninja-build, pkg-config, build-essential |

## Build Instructions (Windows)

```powershell
# Configure
cmake --preset windows-x64-local

# Build and install
cmake --build build_x64 --config RelWithDebInfo
```

The plugin will be automatically installed to `C:/ProgramData/obs-studio/plugins`.

For other platforms:
```bash
# macOS
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo

# Ubuntu
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --config RelWithDebInfo
```

## Usage

1. Open OBS Studio
2. Go to **Docks** menu and enable **Loudness Balance Monitor**
3. In the dock:
   - Select your **Voice** source (microphone)
   - Check the **BGM** sources you want to monitor
   - Adjust **VAD Threshold** if voice detection is too sensitive/insensitive
   - Set **Balance Target** (default: +6 LU)
   - Choose **Mix Preset** (YouTube Standard, Quiet/Safe, Loud/Aggressive)

### Status Indicators

| Status | Balance (Voice-BGM) | Mix Loudness | Clip |
|--------|---------------------|--------------|------|
| OK (Green) | >= +6 LU | >= -18 LUFS | < -1 dBFS |
| WARN (Yellow) | +3 to +6 LU | -22 to -18 LUFS | -1 to 0 dBFS |
| BAD (Red) | < +3 LU | < -22 LUFS | >= 0 dBFS |

### Recommendations

- Keep **Balance** at OK (green) so your voice is clearly audible over BGM
- Keep **Mix** at OK to ensure your stream isn't too quiet for viewers
- Avoid **Clip** warnings by reducing source volumes if peaks are too high

## Known Limitations

- **Mix is an estimate**: The plugin combines Voice + selected BGM sources. It does not capture the actual master output bus.
- **Fader positions may not be reflected**: Audio is captured before OBS volume faders are applied.
- **True Peak not implemented**: Uses Sample Peak only (Phase 1).
- **Windows priority**: Built and tested primarily on Windows. macOS/Linux should work but are less tested.

## Technical Notes

### Audio Processing

- Audio is captured via `obs_source_add_audio_capture_callback()`
- Lock-free SPSC queue transfers audio from callback to worker thread
- LUFS calculation uses [libebur128](https://github.com/jiixyj/libebur128) (MIT license)
- UI updates at 10 Hz via QTimer

### Thread Model

```
Audio Thread (OBS) → Lock-free Queue → Worker Thread (LUFS) → Atomic Results → UI (10Hz)
```

### VAD Parameters

- Attack time: 150 ms
- Release time: 600 ms
- Default threshold: -40 dBFS

## License

GPL-2.0 (same as OBS Studio)

## Author

AllegroMoltoV - https://www.allegromoltov.jp
