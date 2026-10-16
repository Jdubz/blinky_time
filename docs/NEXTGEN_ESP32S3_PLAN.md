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
- **Handoff, part 1: a snapshot for the continuous fields.** These are `energy`, `pulse`, `phase`, `plpPulse`, `rhythmStrength` and `onsetDensity`. The listener publishes them each frame through a seqlock or double buffer, with a publish timestamp.
  - The renderer makes a **bounded** number of read attempts, for example 3.
  - If it can't get a consistent copy (the writer is mid-publish or stalled), it reuses its last complete snapshot. It never spins waiting for the writer.
  - Neither side ever blocks the other.
- **Handoff, part 2: events delivered as counters, not as values in the snapshot.** `onset` and `beatCount` are one-shot events. `AudioControl.h` promises exactly one non-zero `onset` per hit, and `Water` edge-triggers on every non-zero read.
  - With a latest-only snapshot on two independent clocks, a slower renderer misses an onset that a newer frame overwrote, and a faster one reads the same onset twice.
  - So the listener keeps monotonic event counters instead: `onsetSeq`, plus the strength of the newest onset, and `beatCount`, which already is one.
  - The renderer remembers the last value it consumed and synthesises one `onset` per draw when the counter has advanced.
  - When several onsets landed between two draws, it emits one onset with the **maximum** strength and records the count. That matches the existing one-event-per-update contract, and the generators' own cooldowns would merge them anyway.
  - Counters are 32-bit and compared with wrap-safe subtraction, so there is no queue and nothing can overflow.
- **Graceful degradation.** The renderer checks the snapshot's age. If no fresh frame has arrived within a timeout (for example 200 ms), or the listener reports no rhythm, it runs organic mode. A frozen or absent audio core must never freeze the visuals.

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
| Mic calibration | Different mic and gain path. **Mic-level** calibration (hardware gain, levels, normalizer thresholds, noise floor) is redone from scratch for the new mic; don't copy nRF52 values. **End-to-end quality** (onset F1 against `.onsets_consensus.json` on `edm/`, and the tracker metrics) is measured against the same ground truth, so it *is* compared with the nRF52 fleet. That comparison is bench gate 3. |
| Power | Tens of mA active with radio, against a few mA on the nRF52840. Size battery fixtures for it, and measure under real LED and audio load. |
| Flash writes stall both cores | **Unresolved; must be solved before fixtures.** During an ESP-IDF flash erase or write (settings save, OTA), the normal flash path disables the cache and parks the other core. Ordinary tasks on *both* cores are suspended, including IRAM-resident ones, so IRAM placement alone doesn't prevent a render hitch. Options to evaluate on the bench: flash auto-suspend (`CONFIG_SPI_FLASH_AUTO_SUSPEND`) on a flash chip that supports it; LED DMA fed from internal-RAM frame buffers, so output continues while rendering pauses; and scheduling flash writes so they can't overlap visible animation (deferred saves, OTA only in a maintenance state). Any ISR that may run with the cache disabled must keep all its code **and data** in internal RAM. Same class of problem as #191 on the nRF52. |
| Recovery path | The ROM bootloader can't be bricked (BOOT + USB always works), which is better than the nRF52. But there is no equivalent of the nRF52's BLE-DFU path for sealed fixtures: design OTA (dual app partitions with rollback) before sealing anything. |
| Platform split | Two fleets during the transition. Keep `AudioControl` and the fleet protocol identical on both so the server and console treat them the same. |

## Bench gates before building fixtures

1. **Render isolation.** With the listener running at full load, render fps and frame-time jitter on the largest planned LED count match a build with audio disabled.
2. **Every-frame inference.** The onset NN runs on 100 % of frames at 62.5 Hz, with core-1 headroom recorded.
3. **Audio quality.** The `edm/` corpus and onset F1 meet or beat the current nRF52 fleet, measured on the new mic.
4. **Robustness.**
   - Settings save and OTA cause no visible render hitch, using whichever flash-concurrency design the bench work picks (see the risks table).
   - A deliberately crashed or stalled listener, including one stalled mid-publish, leaves the visuals running in organic mode.
   - Onsets are neither lost nor doubled when render fps and audio frame rate differ.
5. **Power.** Measured current under typical and worst-case load, against the fixture battery budget.
6. **Recovery drill.** Interrupted OTA → rollback → re-flash, with the fixture sealed.

## Open questions

- LED driver choice: RMT versus I2S-parallel DMA, given the number of strands and LEDs per fixture.
- How large a model the listener can afford, and whether to retrain for the new mic or fine-tune.
- Fleet transport on ESP32-S3: BLE only, or Wi-Fi/ESP-NOW for fleet features (bonus only, never required).
- When to retire the nRF52 fleet, and whether mixed fleets need any protocol changes.
