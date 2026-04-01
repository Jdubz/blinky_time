# Claude Code Instructions for Blinky Project

## CRITICAL: Upload Safety

Upload safety depends on the platform. ESP32-S3 and nRF52840 use completely different upload protocols.

### ESP32-S3: `arduino-cli upload` is SAFE

```bash
# MUST use full FQBN with USBMode=hwcdc — see note below
arduino-cli compile --upload --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PSRAM=opi' -p /dev/ttyACM0 blinky-things
```

`arduino-cli upload` on ESP32-S3 calls **esptool**, which talks to the chip's hardware ROM bootloader. The ROM bootloader is burned into silicon and cannot be bricked. esptool verifies every write. If interrupted, the ROM bootloader still works and you just re-flash.

### ESP32-S3: JTAG/PDM Pin Conflict

**GPIO42 (PDM CLK) and GPIO41 (PDM DATA) are also JTAG strap pins (MTMS/MTDI).**

ESP32 core 3.3.7 requires `USBMode=hwcdc` for serial (the default TinyUSB mode has unresolved `HWCDCSerial` linker errors — core bug). This enables the `USB_SERIAL_JTAG` peripheral which may claim GPIO42/41 at boot, silently blocking the PDM microphone.

**Mitigation:** `Esp32PdmMic::begin()` calls `gpio_reset_pin()` on both PDM pins before I2S init, then verifies data flows with a 500ms blocking read. If verification fails, `begin()` returns false and the boot log reports `"Audio controller failed to start"`.

**If the ESP32 core is upgraded**, re-test PDM mic on ESP32-S3. If the TinyUSB linker bug is fixed in a future core version, switch back to the default FQBN (`esp32:esp32:XIAO_ESP32S3`) which avoids the JTAG peripheral entirely.

### nRF52840: NEVER use `arduino-cli upload`

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840!**

- `arduino-cli upload` calls `adafruit-nrfutil dfu serial` under the hood
- This protocol has race conditions in USB port re-enumeration
- Single-bank DFU can leave firmware partially written
- Can brick the device, requiring SWD hardware to recover

**Use UF2 instead:**
```bash
make uf2-upload UPLOAD_PORT=/dev/ttyACM0
```

**Why UF2 is safe:**
- Invalid/corrupt UF2 files are silently rejected (cannot brick)
- Simple file copy to mass storage (no serial protocol race conditions)
- Bootloader protects itself (hardware-enforced)
- If interrupted, old firmware stays intact

**Firmware upload is managed by blinky-server** (no standalone scripts):
```bash
# Compile and flash a single device
curl -X POST http://blinkyhost.local:8420/api/ota/compile
curl -X POST http://blinkyhost.local:8420/api/devices/{id}/ota \
  -H 'Content-Type: application/json' \
  -d '{"firmware_path": "/tmp/blinky-build/blinky-things.ino.hex"}'

# Flash ALL connected nRF52840 devices
curl -X POST http://blinkyhost.local:8420/api/fleet/ota \
  -H 'Content-Type: application/json' \
  -d '{"firmware_path": "/tmp/blinky-build/blinky-things.ino.hex"}'
```

The server owns all serial/BLE connections — no port contention, no lock files, no race conditions. It handles bootloader entry, UF2 copy/BLE DFU transfer, and automatic reconnection.

### Safe Operations Summary

**ESP32-S3:**
- `arduino-cli compile --upload` — Safe (uses esptool)
- `arduino-cli upload` — Safe (uses esptool)

**nRF52840:**
- `arduino-cli compile` — Safe (compile only)
- `make uf2-upload` — Safe (uses mass storage, not DFU serial)
- `make uf2-check` — Safe (dry run, no upload)
- `arduino-cli upload` — **NEVER** (uses fragile DFU serial protocol)
- `adafruit-nrfutil dfu serial` — **NEVER**

**Both platforms:**
- `arduino-cli core list/install` — Safe
- Reading serial ports — Safe

### Test Chips (Unenclosed) — Use First for Risky Firmware

There are bare (unenclosed) chips attached to blinkyhost. **Always test untested bootloader changes or risky firmware on these first.**

- Reset button is directly accessible — no disassembly required
- SWD recovery pads are accessible — no emergency disassembly in worst case
- Safe to experiment on before flashing any installed device

### If a Device Becomes Unresponsive

**nRF52840** devices are physically installed and reset buttons are NOT accessible.
If a device stops responding to serial commands:
1. Try power-cycling via USB hub: `uhubctl -a cycle -p <port>`
2. Use blinky-server OTA: `curl -X POST http://blinkyhost.local:8420/api/devices/{id}/ota -d '{"firmware_path":"/tmp/blinky-build/blinky-things.ino.hex"}'`
3. If the port disappeared entirely, wait 10 seconds and check `ls /dev/ttyACM*`
4. Last resort: physically access the device and double-tap reset

## CRITICAL: Long-Running Scripts

**NEVER run ML training or other long-running scripts as Claude session tasks.**
Claude background tasks die when the session ends, killing training mid-run.

Always use tmux:
```bash
tmux new-session -d -s training "cd ml-training && source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py --config configs/frame_fc.yaml --output-dir outputs/<experiment_name> 2>&1 | tee outputs/<experiment_name>/training.log"
```

To check progress: `tmux attach -t training` or `tail -f ml-training/outputs/<experiment_name>/training.log`

`train.py` enforces this — it will refuse to start outside tmux/screen unless `--allow-foreground` is passed.

## Compilation Commands

```bash
# === ESP32-S3 ===
# Requires: arduino-cli lib install "NimBLE-Arduino" (BLE support, fixes core 3.3.7 crash)
# NimBLE 2.4.0 must be installed manually from GitHub (not in registry): h2zero/NimBLE-Arduino@2.4.0
# PSRAM=opi is required — without it, BLE controller malloc(36KB) fails due to internal SRAM exhaustion
# Compile only (MUST use full FQBN — see JTAG/PDM pin conflict above)
arduino-cli compile --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PSRAM=opi' blinky-things

# Compile + upload (safe — uses esptool)
arduino-cli compile --upload --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PSRAM=opi' -p /dev/ttyACM0 blinky-things

# === nRF52840 ===
# Compile only (in-tree build, requires TFLite library)
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense blinky-things

# Compile + validate + upload via UF2 (recommended, NEVER use arduino-cli upload)
make uf2-upload UPLOAD_PORT=/dev/ttyACM0

# Compile + validate only (dry run)
make uf2-check UPLOAD_PORT=/dev/ttyACM0
```

## Documentation Structure

### Primary Documentation

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | **Design philosophy** - visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioTracker architecture (decoupled spectral flux → BPM, NN onset → pulse, PLP pattern extraction) |
| `docs/AUDIO-TUNING-GUIDE.md` | **Main testing guide** - ~10 tunable parameters, test procedures |
| `docs/IMPROVEMENT_PLAN.md` | Current status and roadmap |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator/Effect/Renderer pattern |

### Testing & Calibration

| Document | Purpose |
|----------|---------|
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results (F1 scores, optimal values) |
| `blinky-test-player/NEXT_TESTS.md` | Priority testing tasks |
| `TESTING.md` | Quick start for testing (links to AUDIO-TUNING-GUIDE.md) |

### Hardware & Build

| Document | Purpose |
|----------|---------|
| `docs/HARDWARE.md` | Hardware specifications (XIAO nRF52840 Sense) |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide (config management, safety procedures) |
| `docs/SAFETY.md` | Critical safety guidelines for flashing |
| `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` | **Wireless plan** — BLE, WiFi, OTA, fleet server status |

### Key Architecture Components

- **AudioTracker** (`blinky-things/audio/AudioTracker.h`) - Decoupled tempo/onset: spectral flux → ACF → period; NN onset → visual pulse. Fourier tempogram (Goertzel DFT at candidate frequencies) across 3 mean-subtracted sources (flux, bass, NN onset) selects best period via DFT magnitude + phase alignment for free.
- **FrameOnsetNN** (`blinky-things/audio/FrameOnsetNN.h`) - Conv1D W16 TFLite NN onset detection (single-channel, 13.4 KB INT8, ~7ms). Detects acoustic onsets (kicks/snares), not metrical beats.
- **SharedSpectralAnalysis** (`blinky-things/audio/SharedSpectralAnalysis.h`) - FFT → compressor → whitening → mel bands + spectral flux (HWR)
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - Microphone input with fixed hardware gain (AGC removed v72)
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, plpPulse, phase (PLP-driven), rhythmStrength, onsetDensity

### Obsolete Documents (Removed)

The following were deleted as outdated:
- `RHYTHM-TRACKING-REFACTOR-PROPOSAL.md` - Replaced by AudioController implementation (Dec 2025)
- `TUNING-PLAN.md` - Superseded by AUDIO-TUNING-GUIDE.md (Dec 2025)
- `docs/plans/MUSIC_MODE_TESTING_PLAN.md` - Referenced old MusicMode/PLL (Dec 2025)
- `docs/plans/TRANSIENT_DETECTION_TESTING.md` - Work completed, merged into AUDIO-TUNING-GUIDE.md (Dec 2025)
- `docs/AUDIO_IMPROVEMENTS_PLAN.md` - Phase 1&2 completed, vanity doc (Jan 2026)
- `docs/RHYTHM_ANALYSIS_ENHANCEMENT_PLAN.md` - Replaced by AUDIO_ARCHITECTURE.md (Jan 2026)
- `docs/ENSEMBLE_CALIBRATION_ASSESSMENT.md` - Completed assessment (Jan 2026)
- `blinky-serial-mcp/TOOLING-ASSESSMENT.md` - Completed assessment (Jan 2026)
- `docs/MULTI_HYPOTHESIS_OPEN_QUESTIONS_ANSWERED.md` - Obsolete (Jan 2026)
- `docs/MULTI_HYPOTHESIS_TRACKING_PLAN.md` - Replaced by CBSS beat tracking (Feb 2026)
- `docs/RHYTHM_ANALYSIS_IMPROVEMENTS.md` - Obsolete, referenced deleted params (Feb 2026)
- `docs/RHYTHM_ANALYSIS_TEST_PLAN.md` - Referenced old hypothesis system (Feb 2026)
- `docs/RHYTHM_ANALYSIS_IMPROVEMENT_PLAN.md` - Referenced CombFilterPhaseTracker/fusion (Feb 2026)
- `docs/COMB_FILTER_IMPROVEMENT_PLAN.md` - Old comb filter plan, system replaced (Feb 2026)
- `docs/AUDIO_IMPROVEMENT_ANALYSIS.md` - Completed analysis (Feb 2026)
- `blinky-test-player/src/param-tuner/hypothesis-validator.ts` - Old hypothesis test (Feb 2026)
- `blinky-test-player/TUNING_SCENARIOS.md` - Merged into PARAM_TUNER_GUIDE.md (Jan 2026)
- `docs/PLAN-blinky-console-ui.md` - Console UI built, plan obsolete (Mar 2026)
- `docs/ML_TRAINING_PLAN.md` - Superseded by IMPROVEMENT_PLAN.md sections (Mar 2026)
- `docs/FREQUENCY_DETECTION.md` - Feature removed from firmware (Mar 2026)
- `docs/PLATFORM_FIX.md` - Applied to old Seeeduino mbed platform, no longer used (Mar 2026)
- `docs/COMMON_SCENARIO_TEST_PLAN.md` - Superseded by multi-device A/B test infrastructure (Mar 2026)
- `docs/PATTERN_HISTOGRAM_DESIGN.md` - v77 pattern memory replaced by pattern slot cache v82 (Mar 2026)
- `docs/RFC_MULTI_HYPOTHESIS_PATTERN_AGENTS.md` - Octave-agent design rejected, replaced by slot cache (Mar 2026)
- `docs/RFC_BAYESIAN_ONSET_MODULATION.md` - Referenced v77 pattern memory architecture, obsolete (Mar 2026)

## System Architecture Overview

### Project Structure

The Blinky Time project consists of 5 major components:

```
blinky_time/
├── blinky-things/          Arduino firmware (nRF52840 + ESP32-S3)
├── blinky-console/         React web UI (WebSerial interface)
├── blinky-test-player/     Node.js testing CLI (parameter tuning)
├── blinky-serial-mcp/      MCP server (AI integration)
└── blinky-simulator/       Desktop GIF renderer (compiles actual firmware)
```

### Firmware Architecture (blinky-things/)

**Core Pipeline: Generator → Effect → Renderer**

```
PDM Microphone (16 kHz)
    ↓
AdaptiveMic (fixed gain + window/range normalization)
    ↓
SharedSpectralAnalysis (FFT-256 → compressor → whitening → mel bands + spectral flux)
    ↓
    ├── [BPM] Spectral flux (HWR) → contrast² → OSS buffer → ACF → period estimate
    ├── [ONSET] FrameOnsetNN (Conv1D W16, ~7ms) → onset activation → pulse (visual sparks)
    ├── [PLP] Fourier tempogram (Goertzel DFT) across 3 sources (flux, bass, NN onset) → best period + pattern
    ↓
AudioTracker (decoupled: spectral flux → tempo, NN onset → pulse, PLP → phase/pattern)
    ↓
AudioControl {energy, pulse, plpPulse, phase, rhythmStrength, onsetDensity}
    ↓
Generator (Fire/Water/Lightning)
    ↓
Effect (HueRotation/NoOp)
    ↓
RenderPipeline → LED Output
```

**Key Components:**

1. **Audio Input & Processing**
   - `AdaptiveMic.h` - PDM microphone with fixed hardware gain (AGC removed v72; nRF52840: gain=32, ESP32-S3: gain=30)
   - `SharedSpectralAnalysis.h` - FFT-256 (128 freq bins @ 62.5 Hz), soft-knee compressor → per-bin whitening (v23+), spectral flux (HWR)
   - Window/range normalization (0-1 output) — sole dynamic range system

2. **Onset Detection (single Conv1D model, deployed)**
   - `FrameOnsetNN.h` - Single-model TFLite NN inference for acoustic onset detection
     - Conv1D W16 (256ms), [24,32] channels, 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3
     - Single output channel: onset activation (kicks/snares — cannot distinguish on-beat from off-beat)
     - v1 deployed: All Onsets F1=0.681 (Kick 0.607, Snare 0.666, HiHat 0.704)
     - v3 deployed: All Onsets F1=0.787 (Kick 0.688, Snare 0.773, HiHat 0.806)
     - v11-final deployed (4/5 blinkyhost): de-duplicated onset labels, Kick recall 0.743, Snare 0.727, HiHat 0.701
     - Arena: 3404/32768 bytes
     - Used for: visual pulse, energy peak-hold. NOT used for BPM estimation.
   - Non-NN fallback: `mic_.getLevel()` (energy envelope as simple onset signal)

3. **Tempo Estimation & Rhythm Tracking (AudioTracker, v82)**
   - `AudioTracker.h/cpp` - Decoupled tempo/onset architecture (~10 params)
   - **Period path** (NN-independent): spectral flux + bass energy → multi-source ACF (lags 20-80, ~4ms) → beat-level peaks → bar multipliers (2×/3×/4×) → epoch-fold variance scoring selects best pattern period. Parabolic interpolation on ACF peaks for sub-step precision. **The system finds the period that produces the best visual pattern, not the "correct" BPM. Half/double time periods are valid if they capture more rhythmic structure.**
   - **Onset path** (NN-driven): FrameOnsetNN → onset activation → pulse detection (visual sparks)
     - Pulse detection: floor-tracking baseline (fast drop, slow rise)
   - **PLP path**: Epoch-fold at detected period → repeating energy pattern. ACF strength × sqrt(variance) scoring with multiplier penalty. Soft blend (PLP pattern / cosine fallback, continuous by confidence). Cold-start template seeding (8 patterns, cosine similarity > 0.50). Epoch-fold pattern extraction for visual pattern shape. Confidence = ACF peak strength × signal presence (steep mic level gate). Adaptive phase correction (EMA variance: fast during convergence, slow when locked).
   - **Pattern slot cache** (v82): 4-slot LRU cache of 16-bin PLP pattern digests. Every bar (4 beats), current PLP pattern resampled to 16-bin digest and compared via cosine similarity against cached slots. Match > 0.70 triggers instant recall. Enables rapid section switching (verse/chorus/bridge).
   - Energy synthesis: hybrid mic level + bass mel energy + onset peak-hold

4. **Generators (Visual Effects)**
   - `Fire.cpp/h` - HeatFire: hybrid audio-reactive design, dt-based scroll speed, energy drives full flame height
   - `Water.cpp/h` - Wave simulation with ripples
   - `Lightning.cpp/h` - Branching bolt effects
   - All generators consume `AudioControl` struct

5. **Post-Processing Effects**
   - `HueRotationEffect.h` - Color cycling
   - `NoOpEffect.h` - Pass-through (identity)
   - Effect chaining supported

6. **Configuration & Persistence**
   - `ConfigStorage.h/cpp` - Flash-based storage (SETTINGS_VERSION: v82)
   - `SettingsRegistry.h/cpp` - Tunable parameters (~30 after BandFlux removal)
   - Runtime validation (min/max bounds)
   - Factory reset capability

7. **Device Abstraction**
   - `HatConfig.h` - 89 LEDs (STRING layout)
   - `TubeLightConfig.h` - 60 LEDs (4x15 MATRIX)
   - `BucketTotemConfig.h` - 128 LEDs (16x8 MATRIX)
   - Compile-time device selection

8. **Hardware Abstraction Layer (HAL)**
   - `IPdmMic.h`, `ISystemTime.h`, `ILedStrip.h` - Interfaces
   - `DefaultHal.h` - Platform implementations
   - `MockHal.h` - Test implementations

9. **Serial Interface**
   - `SerialConsole.h/cpp` - Command interpreter
   - JSON API (settings, streaming, info)
   - 50+ commands (get/set/show/save/load)
   - Audio streaming (~20 Hz)

### Web UI Architecture (blinky-console/)

**Stack: React 18 + TypeScript + Vite + WebSerial**

```
React Components
├── ConnectionBar (device status, battery)
├── SettingsPanel (50+ parameters)
├── AudioVisualizer (Chart.js real-time display)
├── GeneratorSelector (Fire/Water/Lightning)
├── EffectSelector (HueRotation/NoOp)
├── SerialConsoleModal (raw command interface)
└── TabView (multi-panel layout)
```

**Features:**
- WebSerial API for direct USB serial connection
- Real-time audio visualization (energy, pulse, transients)
- Hierarchical settings organization
- PWA (offline capable, installable)
- Responsive design (desktop/mobile)

### Testing Infrastructure

**blinky-test-player (Parameter Tuning)**
- Playwright-based audio pattern playback
- Ground truth comparison (expected vs detected transients)
- Binary search optimization (~30 min per param)
- Sweep mode (exhaustive parameter range testing)
- 60+ test patterns (simple-beat, complex-rhythm, polyrhythmic, melodic, etc.)

**blinky-serial-mcp (AI Integration)**
- 20+ MCP tools for device interaction
- Connection management (list_ports, connect, status)
- Settings control (get_settings, set_setting, save_settings)
- Audio streaming (stream_start, get_audio, monitor_audio)
- Testing (run_test, start_test, stop_test)
- Pattern library (list_patterns)

### MCP Testing Best Practices

**ALWAYS use `run_test` for pattern testing** - it automatically:
1. Connects to the device
2. Plays the pattern and records detections
3. Disconnects when complete

```
run_test(pattern: "steady-120bpm", port: "COM11")
```

**Gain is fixed** - AGC was removed in v72. Hardware gain is set at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization handles dynamic range.

**DO NOT manually connect/disconnect** - Using separate `connect`, `stream_start`, `start_test`, `stop_test`, `disconnect` calls:
- Risks leaving the port locked if an error occurs
- Prevents firmware flashing until manually disconnected
- Is more error-prone and verbose

**Exception**: Use manual connection only when:
- Exploring settings interactively (`get_settings`, `set_setting`)
- Monitoring audio continuously (`monitor_audio` with long duration)
- Debugging device state (`status`, `send_command`)

**Unit & Integration Tests**
- `tests/unit/` - Device configs, LED mapping, parameter bounds
- `tests/integration/` - Generator output, effect chaining, serial commands
- Custom BlinkyTest.h framework (Arduino-compatible)

### Data Flow Example (Fire Effect)

```
1. PDM mic samples → AdaptiveMic (fixed gain + window/range normalization)
2. AdaptiveMic → SharedSpectralAnalysis (FFT-256 → compressor → per-bin whitening → mel bands + spectral flux)
3. [BPM PATH] Spectral flux (HWR: sum of positive magnitude changes) → contrast²
4. Contrast-sharpened spectral flux → OSS buffer (~5.5s, 360 samples @ ~66 Hz)
5. ACF every 150ms → raw peak-finding → BPM / period estimate
6. [ONSET PATH] SharedSpectralAnalysis → FrameOnsetNN (16-frame mel window → Conv1D → onset activation)
7. Onset activation → pulse detection (visual sparks)
8. [PLP PATH] Fourier tempogram (Goertzel DFT) across 3 mean-subtracted sources (flux, bass, NN onset)
9. DFT magnitude selects period, DFT phase gives alignment → epoch-fold pattern → phase + plpPulse output
10. Output: AudioControl{energy=0.45, pulse=0.85, phase=0.12, rhythmStrength=0.75,
     onsetDensity=3.2}
11. Fire generator:
    - energy → baseline flame height
    - pulse → spark burst intensity
    - phase → breathing effect (0=on-beat)
    - rhythmStrength → blend music/organic mode
    - onsetDensity → content classification (dance=2-6/s, ambient=0-1/s)
12. Fire heat diffusion (matrix propagation)
13. HueRotationEffect (optional color shift)
14. RenderPipeline → LED strip output
```

### Resource Usage (nRF52840)

**Memory:**
- RAM: ~16 KB globals + arena 3404/32768 bytes (Conv1D W16 model) + 1.6 KB mel buffer
- Flash: ~345 KB with single model (13.4 KB INT8 + TFLite Micro runtime). ~30 KB settings storage.
- Available: 256 KB RAM, 1 MB Flash

**CPU (64 MHz):**
- Microphone + FFT: ~4%
- FrameOnsetNN inference (62.5 Hz): 6.8ms/frame (nRF52840), 5.8ms/frame (ESP32-S3)
- Autocorrelation (500ms): ~3% amortized
- ACF + PLP: ~1%
- Fire generator: ~5-8%
- LED rendering: ~2%
- **Total: ~20-25%** (much lighter with W16 model)

### Safety Architecture (Multi-Layer Defense)

**Layer 1: Documentation**
- CLAUDE.md (persistent AI warnings)
- DEVELOPMENT.md (safety procedures)
- SAFETY.md (mechanism overview)

**Layer 2: Compile-Time**
- Static assertions (struct size validation)
- CONFIG_VERSION enforcement
- Type-safe parameter access

**Layer 3: Runtime**
- Parameter validation (min/max bounds)
- Corrupt data detection
- Flash address validation (bootloader protection)

**Layer 4: Flash Safety**
- Bootloader region protection (< 0x30000)
- Sector alignment validation
- System halt on unsafe write

**Layer 5: Automation**
- Pre-commit hooks
- Safety check scripts
- Git hooks enforcement

**Layer 6: Upload Enforcement**
- Arduino IDE only (NO arduino-cli)
- Device bricking prevention

### Current Status (March 2026)

> **ESP32-S3 support has been cut** (March 2026). All active development targets nRF52840 only.

**Production Ready:**
- ✅ AudioTracker with ACF+PLP (Fourier tempogram) + pulse baseline tracking + pattern slot cache (v82)
- ✅ FrameOnsetNN (Conv1D W16 onset-only, 13.4 KB INT8, v11-final deployed on 4/5 blinkyhost devices)
- ✅ HeatFire/Water/Lightning generators
- ✅ Web UI (React + WebSerial)
- ✅ Testing infrastructure (MCP + param-tuner + batch A/B test scripts)
- ✅ Multi-layer safety mechanisms
- ✅ 3 device configurations (Hat, Tube, Bucket) + Display (32x32 matrix)
- ✅ Mic calibration pipeline + gain-aware training augmentation
- ✅ Fixed hardware gain (AGC removed v72; nRF52840: gain=32)
- ✅ Simulator working (rebuilds with current firmware code)
- ✅ Active devices: nRF52840 on blinkyhost + 1 nRF52840 tube local

**Removed (v64-v82):**
- v64: Forward filter, particle filter, HMM phase tracker, multi-agent beat tracking, template/subbeat/metrical octave checks, ODF sources 1-5, legacy spectral flux (~1500 lines)
- v67: EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult (~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved)
- v68: Removed ENABLE_NN_BEAT_ACTIVATION ifdef and nnBeatActivation runtime toggle. FrameOnsetNN always compiled in and active. TFLite is a required dependency.
- v72: AGC removed. Hardware gain fixed at platform optimal. Window/range normalization is sole dynamic range system.
- v82: v77 pattern memory (IOI histogram + bar histogram + onset buffer, ~200 lines, 10 params) replaced by pattern slot cache (4-slot LRU of PLP pattern digests, 5 params). Fisher's g removed (dead code). downbeat/beatInMeasure removed from AudioControl.
- Spectral noise subtraction (`noiseest=0`): still in SharedSpectralAnalysis, default OFF

**Wireless (March 31, 2026):**
- ✅ BLE NUS bidirectional on nRF52840 (Print& refactor, all commands work over BLE)
- ✅ BLE NUS bidirectional on ESP32-S3 (NUS TX wired, verified `show nn` over BLE)
- ✅ Wireless-only mode (`--no-serial`, 6 devices managed via BLE only)
- ✅ Server OTA: `POST /api/devices/{id}/ota` + `POST /api/fleet/ota` (delegates to uf2_upload.py)
- ✅ BLE DFU protocol reverse-engineered (Legacy DFU SDK v11, START_DFU notification verified)
- ✅ Platform detection via `json info` (`"platform":"nrf52840"` / `"esp32s3"`)
- ✅ Cross-transport identity: `json info` reports `"sn"` (FICR DEVICEID) and `"ble"` (BLE MAC address)
- ✅ BLE disconnect detection: bleak `disconnected_callback` → auto-reconnect within 10s
- ✅ BLE liveness checks: background ping every ~30s catches silent disconnects
- ✅ BLE DFU as UF2 fallback: serial OTA auto-falls back to BLE DFU when UF2 fails
- ✅ DFU recovery detection: scans for DFU service UUID, detects SafeBoot crash recovery, surfaces `dfu_recovery` state
- ✅ DFU recovery OTA: `POST /devices/{id}/ota` pushes firmware directly to devices in DFU bootloader
- ✅ Fleet server on blinkyhost (5 serial + 6 BLE devices, hardware_sn dedup, REST API)
- ✅ BLE DFU proven end-to-end (Legacy DFU SDK v11, 510KB in ~5.5min, tested Mar 30 on bare chip)
- ✅ API enrichment: `GET /devices` includes `hardware_sn`, `ble_address`, `rssi`, `last_seen`
- ⚠️ Post-BLE-DFU: device needs physical power cycle for USB/BLE re-enumeration (uhubctl insufficient on Pi)
- ⚠️ ESP32-S3 WiFi blocked by antenna (u.FL only, no PCB antenna on Sense variant)
- See `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` for full details

**Planned (Not Started):**
- NN model improvements: confidence-weighted loss, tempo auxiliary head
- ESP32-S3 platform-specific model (larger compute budget allows bigger model)
- Dynamic device switching (runtime config)
- CI/CD automation

**Closed (mel-spectrogram CNN, v4-v9):**
- All architectures (standard conv, BN-fused, DS-TCN) measured 79-98ms on Cortex-M4F — 8-10× over frame budget
- Superseded by frame-level FC approach (~0.2-5ms at 31.25 Hz)

**Closed (beat-synchronous hybrid, March 2026):**
- FC on accumulated spectral summaries at beat rate (~2 Hz). Circular dependency with CBSS, negligible discriminative power in per-beat features, misaligned with all leading approaches. Superseded by frame-level FC.

**Closed (W192 FC, March 2026):**
- FC(4992→64→32→2), 322K params, 314 KB INT8. Severe regression from W32. FC flattening destroys temporal locality for wide windows.

**Closed (Dual-model architecture, March 2026):**
- OnsetNN + RhythmNN split abandoned. Every published beat/downbeat system uses a single joint model. Split underperformed FC baseline on both tasks. Superseded by single Conv1D W64 with Beat This! sum head.

## Documentation Guidelines

**Only create documentation with future value:**
- Architecture designs and technical specifications
- Implementation plans and roadmaps
- Todo lists and outstanding action items
- Testing procedures and calibration guides

**DO NOT create:**
- Code review documents (delete after fixes are implemented)
- Analysis reports of completed work (vanity documentation)
- Historical "what we did" summaries (use git commit history instead)
- Post-mortem reports (capture lessons in architecture docs or commit messages)

**Reviews and analysis must focus on outstanding actions**, not documenting past work. Git history serves as the permanent record of changes and decisions.

## Current Audio System (March 2026)

### Detection Architecture
**Previous (v68):** FrameOnsetNN (then named FrameBeatNN) — single FC model, FC(832→64→32→2), 56.8 KB INT8, W32 (0.5s).
**Previous (v69):** Dual-model (OnsetNN + RhythmNN) — abandoned Mar 16. Every published system uses single joint model; split underperformed FC baseline.
**Current (v83, deployed):** Multi-source ACF + PLP architecture with pattern slot cache. ACF scans beat-level lags (20-80) on 3 sources (flux, bass, NN onset), parabolic interpolation refines peaks. Bar-level candidates via 2×/3×/4× multipliers with sqrt(multiplier) penalty. Epoch-fold variance scoring selects the period with the best pattern contrast — **the correct period is whichever produces the best visual pattern, regardless of musical BPM. Half/double time matches are valid.** Pattern slot cache (4-slot LRU of 16-bin PLP pattern digests) enables instant section recall. NN onset detection (FrameOnsetNN, Conv1D W16) drives visual pulse independently. Performance: ACF ~4ms (vs 75ms old Fourier tempogram). Primary test metrics: plpAtTransient (pattern-onset alignment), plpAutoCorr (pattern periodicity), plpPeakiness (pattern structure).
- Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→1,k=1). 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3. Single output: onset activation. v1 deployed: All Onsets F1=0.681 (Kick 0.607, Snare 0.666). v3 deployed: All Onsets F1=0.787 (Kick 0.688, Snare 0.773). v11-final deployed (4/5 blinkyhost): de-duplicated onset labels, Kick recall 0.743, Snare 0.727, HiHat 0.701. Arena: 3404/32768 bytes.
- Fallback if model fails to load: mic_.getLevel() as simple energy onset signal.
- Design goal: onset detection for visual pulse, pattern-quality-driven period detection, PLP phase/pattern extraction. No downbeat tracking. **Pattern accuracy is the primary metric — BPM accuracy is irrelevant; octave-matched periods (half/double time) are correct if they produce better visual patterns.** Trigger on kicks and snares only; hi-hats/cymbals create overly busy visuals. See [VISUALIZER_GOALS.md](docs/VISUALIZER_GOALS.md) for the full design philosophy.
- Training data: consensus_v5 labels (7-system), cal63 mel calibration.

### Key Features
- **Multi-source ACF + PLP architecture** (v83): ACF scans beat-level lags (20-80) on 3 mean-subtracted sources (flux, bass, NN onset). Parabolic interpolation refines peaks. Bar multipliers (2×/3×/4×) generate candidates scored by ACF strength × sqrt(epoch-fold variance) / sqrt(multiplier). **Pattern quality is the objective — period selection optimizes for visual pattern contrast, not BPM accuracy. Half/double time matches are correct if they produce better patterns.** PLP epoch-folds NN-gated flux at detected period for visual pattern shape. Cosine OLA pulse buffer for smooth PLP output. Pattern slot cache (4-slot LRU of 16-bin PLP digests) for instant section recall. NN onset used for visual pulse only.
- **Single Conv1D NN** (deployed): FrameOnsetNN, Conv1D W16 [24,32] onset-only, 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3. Single output: onset activation. Per-tensor INT8 quantization (CMSIS-NN requirement).
- **Spectral flux** (v75): Half-wave rectified magnitude change from SharedSpectralAnalysis. Peaks at broadband transients, zero during sustain. NN-independent signal for ACF period estimation and PLP Fourier tempogram.
- **AGC removed** (v72): Hardware gain fixed at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization is sole dynamic range system.
- **Pulse baseline tracking**: Floor-tracking baseline replaces running-mean threshold for pulse detection
- **Energy synthesis**: Hybrid mic level + bass mel energy + onset peak-hold
- **Spectral conditioning** (v23+): Soft-knee compressor (Giannoulis 2012) → per-bin adaptive whitening
- **Multi-source ACF period detection** (v83): Replaces Fourier tempogram (75ms → ~4ms). ACF at beat-level lags with parabolic interpolation, bar multipliers (2×/3×/4×) with sqrt penalty, epoch-fold variance scoring. Phase from epoch-fold peak position.
- **PLP pattern extraction + slot cache** (v83): Epoch-fold at ACF-detected period across NN-gated flux. Cosine OLA pulse buffer for smooth output. Soft blend: PLP pattern and cosine fallback blended continuously by confidence (no hard threshold). Cold-start template seeding (8 patterns, cosine similarity > 0.50, cuts warm-up ~8→~2 bars). Pattern slot cache: 4-slot LRU of 16-bin PLP pattern digests, compared every bar via cosine similarity — match > 0.70 triggers instant recall by seeding plpPattern_ from cached slot. BPM shift >15% invalidates all slots. 5 tunable slot params. Steep signal gate for mic level. Beat stability gated learning (fill/breakdown immunity). Pattern normalization uses min-max (signal is mean-subtracted, may have negatives). Adaptive phase correction (EMA variance: fast during convergence, slow when locked).
- **Tempo-adaptive cooldown**: Shorter cooldown at faster tempos (min 40ms, max 150ms)
