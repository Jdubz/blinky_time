# Next-gen fixtures: XIAO ESP32-S3 Sense, dual-core

**Status:** planned (decided 2026-10-15). The current fleet stays on the XIAO nRF52840 Sense; nothing here changes deployed devices. The next generation of fixtures is built on the XIAO ESP32-S3 Sense.

## Why

1. **Visuals come first.** Smooth LED output matters more than audio. On the nRF52840 a single 64 MHz core runs rendering, LED output, BLE and the whole audio pipeline. Audio work (FFT, mel, onset NN, tempo/phase) competes with rendering for the same core. The NN is already skipped on catch-up frames (#165) and stream formatting can stall the loop (#171).
2. **Every unit is fully independent.** Each fixture has its own listener and its own analysis and works alone. Fleet features (broadcast scenes, gossip ACK, shared timing) are a bonus layered on top, never a dependency.
3. **The fixtures are being rebuilt anyway.** Any physical change dwarfs firmware cost, so the platform can change now at no extra hardware cost.

## Architecture: one board, two cores

```
Core 0 (PRO_CPU): renderer             Core 1 (APP_CPU): listener
  generators / effects / pipeline        PDM mic (I2S-PDM DMA)
  LED output (RMT or I2S DMA)            FFT -> mel -> onset NN (every frame)
  BLE / fleet / serial console           tempo, phase, PLP pattern
  reads latest AudioControl ------<----- publishes AudioControl per frame
```

- **Renderer has priority by construction.** It owns core 0 and never waits on audio. The LED bit timing is done in hardware (RMT/I2S DMA), so a busy core can't corrupt frames.
- **Listener runs on the audio clock.** It is a task pinned to core 1 and processes every frame, with no catch-up skipping. Its CPU and PSRAM budget allow a larger onset model than the nRF52 can run.
- **Handoff is a lock-free snapshot.** The listener writes the frame's `AudioControl` (≈30 B) through a seqlock or double buffer, and the renderer reads the newest copy whenever it draws. Neither side blocks the other.
- **Graceful degradation.** If the listener stalls or reports no rhythm, the renderer runs organic mode. A frozen or absent audio core must never freeze the visuals.

### The contract stays the same

`AudioControl` (`blinky-things/audio/AudioControl.h`) is the interface between analysis and visuals: `energy`, `pulse`, `onset`, `phase`, `beatCount`, `plpPulse`, `rhythmStrength`, `onsetDensity`. Generators keep consuming it unchanged. What runs underneath is free to change: the model, the features, even a different listener entirely.

## What carries over

- **Already in the tree, dormant:** `hal/hardware/Esp32PdmMic`, `comms/Esp32BleNus`, `comms/WifiManager`, `comms/WifiCommandServer`, `comms/BleAdvertiser`, and the `BLINKY_PLATFORM_ESP32S3` branches in `hal/PlatformDetect.h` and the files that use it. CI no longer builds them, so expect bit-rot to fix first.
- **Portable code:** generators, effects, render pipeline, `AudioTracker`, `SharedSpectralAnalysis` (needs an ESP-DSP FFT in place of CMSIS), `FrameOnsetNN` (TFLite Micro, using ESP-NN kernels), and the settings/config storage logic.
- **Tooling:** the `tests/tracker` harness and corpus gate are platform-independent, and the validation corpus stays `blinky-test-player/music/edm/` (CLAUDE.md).

## Known risks (from the March 2026 cut and since)

| Risk | Notes / mitigation |
|---|---|
| Antenna / RF | Reported in the earlier ESP32-S3 work. Check fixture layout and enclosure early, with BLE range measured inside a built fixture. |
| Sense pin conflicts | On the Sense, the PDM mic shares pins with JTAG/other functions. Fix the pin map before the PCB/fixture layout. |
| Mic calibration | Different mic and gain path: re-establish gain staging, then the `edm/` corpus metrics from scratch. Never compare against nRF52 numbers. |
| Power | Tens of mA active with radio, against a few mA on the nRF52840. Size battery fixtures for it, and measure under real LED and audio load. |
| Flash writes stall both cores | Code fetch stops on both cores during flash erase/write (settings save, OTA). Put the audio ISR and the listener's hot path in IRAM, and keep settings saves rare and debounced (same class of problem as #191). |
| Recovery path | The ROM bootloader can't be bricked (BOOT + USB always works), which is better than the nRF52. But there is no equivalent of the nRF52's BLE-DFU path for sealed fixtures: design OTA (dual app partitions with rollback) before sealing anything. |
| Platform split | Two fleets during the transition. Keep `AudioControl` and the fleet protocol identical on both so the server and console treat them the same. |

## Bench gates before building fixtures

1. **Render isolation.** With the listener running at full load, render fps and frame-time jitter on the largest planned LED count match a build with audio disabled.
2. **Every-frame inference.** The onset NN runs on 100 % of frames at 62.5 Hz, with core-1 headroom recorded.
3. **Audio quality.** The `edm/` corpus and onset F1 meet or beat the current nRF52 fleet, measured on the new mic.
4. **Robustness.** Settings save and OTA cause no visible render hitch. A deliberately crashed or stalled listener leaves the visuals running (organic mode).
5. **Power.** Measured current under typical and worst-case load, against the fixture battery budget.
6. **Recovery drill.** Interrupted OTA → rollback → re-flash, with the fixture sealed.

## Open questions

- LED driver choice: RMT versus I2S-parallel DMA, given the number of strands and LEDs per fixture.
- How large a model the listener can afford, and whether to retrain for the new mic or fine-tune.
- Fleet transport on ESP32-S3: BLE only, or Wi-Fi/ESP-NOW for fleet features (bonus only, never required).
- When to retire the nRF52 fleet, and whether mixed fleets need any protocol changes.
