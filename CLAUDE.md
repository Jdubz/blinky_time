# Claude Code Instructions for Blinky Project

## CRITICAL: Upload Safety

Upload safety depends on the platform. ESP32-S3 and nRF52840 use completely different upload protocols.

### ESP32-S3: `arduino-cli upload` is SAFE

```bash
# MUST use full FQBN with USBMode=hwcdc — see note below
arduino-cli compile --upload --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240' -p /dev/ttyACM0 blinky-things
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

See `tools/uf2_upload.py --help` for all options.

### nRF52840 Pre-Flash Checklist

**Devices are physically installed — double-tap reset is NOT an option.**
Bootloader entry MUST succeed via software (serial command or 1200-baud touch).
A failed bootloader entry leaves the device running old firmware but wastes time.

**Before EVERY nRF52840 flash attempt:**
1. **Disconnect ALL MCP sessions** — `mcp__blinky-serial__disconnect` on every port, or verify `mcp__blinky-serial__status` shows no connections
2. **Wait 3 seconds** after MCP disconnect — the Node.js `SerialPort.close()` is async; the OS file descriptor may not be released immediately
3. **Do NOT flash immediately after interactive serial use** — always disconnect and wait
4. **Flash one device at a time** unless using `--parallel` mode

**Why this matters:** If an MCP server or console session holds the serial port, `uf2_upload.py` cannot send the bootloader entry command. The device resets but doesn't enter UF2 mode. The script retries 5 times (40+ seconds wasted), then fails. The `uf2_upload.py` script includes a port availability pre-check that will detect and report this condition.

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

### If a Device Becomes Unresponsive

**nRF52840** devices are physically installed and reset buttons are NOT accessible.
If a device stops responding to serial commands:
1. Try power-cycling via USB hub: `uhubctl -a cycle -p <port>`
2. Re-run: `python3 tools/uf2_upload.py --build-dir /tmp/blinky-build /dev/ttyACMx`
3. If the port disappeared entirely, wait 10 seconds and check `ls /dev/ttyACM*`
4. Last resort: physically access the device and double-tap reset

**ESP32-S3**: Just re-run `arduino-cli compile --upload`. The ROM bootloader always survives.

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
# Compile only (MUST use full FQBN — see JTAG/PDM pin conflict above)
arduino-cli compile --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240' blinky-things

# Compile + upload (safe — uses esptool)
arduino-cli compile --upload --fqbn 'esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240' -p /dev/ttyACM0 blinky-things

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

### Key Architecture Components

- **AudioTracker** (`blinky-things/audio/AudioTracker.h`) - Decoupled tempo/onset: spectral flux → ACF+Comb → BPM; NN onset → visual pulse. PLP extracts dominant repeating energy pattern from dual sources (flux + bass energy).
- **FrameOnsetNN** (`blinky-things/audio/FrameOnsetNN.h`) - Conv1D W16 TFLite NN onset detection (single-channel, 13.4 KB INT8, ~7ms). Detects acoustic onsets (kicks/snares), not metrical beats.
- **SharedSpectralAnalysis** (`blinky-things/audio/SharedSpectralAnalysis.h`) - FFT → compressor → whitening → mel bands + spectral flux (HWR)
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - Microphone input with fixed hardware gain (AGC removed v72)
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, plpPulse, phase (PLP-driven), rhythmStrength, onsetDensity (downbeat/beatInMeasure always 0 — not tracked)

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
    ├── [BPM] Spectral flux (HWR) → contrast² → OSS buffer → ACF + comb bank → BPM
    ├── [ONSET] FrameOnsetNN (Conv1D W16, ~7ms) → onset activation → pulse (visual sparks)
    ├── [PLP] Epoch-fold flux + bass energy at detected period → repeating pattern
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
     - Arena: 3404/32768 bytes
     - Used for: visual pulse, energy peak-hold. NOT used for BPM estimation.
   - Non-NN fallback: `mic_.getLevel()` (energy envelope as simple onset signal)

3. **Tempo Estimation & Rhythm Tracking (AudioTracker, v80)**
   - `AudioTracker.h/cpp` - Decoupled tempo/onset architecture (~10 params)
   - **BPM path** (NN-independent): spectral flux → contrast sharpening → OSS buffer (~5.5s, 360 samples @ ~66 Hz) → ACF → period estimate
   - **Onset path** (NN-driven): FrameOnsetNN → onset activation → pulse detection (visual sparks)
     - Pulse detection: floor-tracking baseline (fast drop, slow rise)
   - **PLP path**: Epoch-fold dual sources (spectral flux + bass energy) at detected period → repeating energy pattern → phase + plpPulse
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
   - `ConfigStorage.h/cpp` - Flash-based storage (SETTINGS_VERSION: v80)
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
8. [PLP PATH] PLP pattern extraction from spectral flux + bass energy at detected period
9. PLP cross-correlation phase alignment → phase + plpPulse output
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
- CBSS + beat detection: ~1%
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

**Production Ready:**
- ✅ AudioTracker with ACF+Comb+PLP + pulse baseline tracking
- ✅ FrameOnsetNN (Conv1D W16 onset-only, 13.4 KB INT8, All Onsets F1=0.681, deployed on all 7 devices)
- ✅ ESP32-S3 PDM mic fix (proper I2S configuration)
- ✅ HeatFire/Water/Lightning generators
- ✅ Web UI (React + WebSerial)
- ✅ Testing infrastructure (MCP + param-tuner + batch A/B test scripts)
- ✅ Multi-layer safety mechanisms
- ✅ 3 device configurations (Hat, Tube, Bucket) + Display (32x32 matrix)
- ✅ Mic calibration pipeline + gain-aware training augmentation
- ✅ Fixed hardware gain (AGC removed v72; nRF52840: 32, ESP32-S3: 30)
- ✅ Simulator working (rebuilds with current firmware code)
- ✅ 7 devices: 3 nRF52840 + 2 ESP32-S3 on blinkyhost, 1 nRF52840 tube + 1 ESP32-S3 display local

**Removed (v64-v72):**
- v64: Forward filter, particle filter, HMM phase tracker, multi-agent beat tracking, template/subbeat/metrical octave checks, ODF sources 1-5, legacy spectral flux (~1500 lines)
- v67: EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult (~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved)
- v68: Removed ENABLE_NN_BEAT_ACTIVATION ifdef and nnBeatActivation runtime toggle. FrameOnsetNN always compiled in and active. TFLite is a required dependency.
- v72: AGC removed. Hardware gain fixed at platform optimal. Window/range normalization is sole dynamic range system.
- Spectral noise subtraction (`noiseest=0`): still in SharedSpectralAnalysis, default OFF

**Planned (Not Started):**
- NN model improvements: confidence-weighted loss, tempo auxiliary head
- ESP32-S3 platform-specific model (larger compute budget allows bigger model)
- Bluetooth/BLE support (design doc complete)
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
**Current (v79, deployed):** Decoupled tempo/onset architecture. BPM uses spectral flux (NN-independent). NN onset detection (FrameOnsetNN, Conv1D W16) drives visual pulse. PLP extracts repeating energy pattern for phase.
- Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→1,k=1). 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3. Single output: onset activation. v1 deployed: All Onsets F1=0.681 (Kick 0.607, Snare 0.666). v3 deployed: All Onsets F1=0.787 (Kick 0.688, Snare 0.773). Arena: 3404/32768 bytes.
- Fallback if model fails to load: mic_.getLevel() as simple energy onset signal.
- Design goal: onset detection for visual pulse, spectral-flux-based BPM, PLP phase/pattern extraction. No downbeat tracking. Trigger on kicks and snares only; hi-hats/cymbals create overly busy visuals. See [VISUALIZER_GOALS.md](docs/VISUALIZER_GOALS.md) for the full design philosophy.
- Training data: consensus_v5 labels (7-system), cal63 mel calibration.

### Key Features
- **Decoupled BPM/onset** (v79): BPM estimation uses spectral flux (HWR, NN-independent). NN onset used for visual pulse only. PLP extracts repeating energy pattern for phase. Prevents syncopated/off-beat transients from corrupting ACF periodicity.
- **Single Conv1D NN** (deployed): FrameOnsetNN, Conv1D W16 [24,32] onset-only, 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3. Single output: onset activation. Per-tensor INT8 quantization (CMSIS-NN requirement).
- **Spectral flux** (v75): Half-wave rectified magnitude change from SharedSpectralAnalysis. Peaks at broadband transients, zero during sustain. NN-independent BPM signal for ACF + comb bank.
- **AGC removed** (v72): Hardware gain fixed at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization is sole dynamic range system.
- **Pulse baseline tracking**: Floor-tracking baseline replaces running-mean threshold for pulse detection
- **Energy synthesis**: Hybrid mic level + bass mel energy + onset peak-hold
- **Spectral conditioning** (v23+): Soft-knee compressor (Giannoulis 2012) → per-bin adaptive whitening
- **ACF tempo estimation** (v80): Bare ACF peak-finding on spectral flux (Percival/Rayleigh/comb bank removed v80 — octave errors are non-issues with PLP)
- **PLP pattern extraction** (v79): Epoch-folds dual sources (spectral flux + bass energy) at detected period, extracts dominant repeating energy pattern via cross-correlation phase alignment
- **Tempo-adaptive cooldown**: Shorter cooldown at faster tempos (min 40ms, max 150ms)
