# Phone Record Mode Plan

**Goal:** capture what a single device hears and does at real concert volume, with no blinky-server nearby. An Android phone running the hosted console (Chrome, Web Bluetooth) connects directly to one device. It starts a recording, and the device streams **raw mic PCM** plus its live analysis to the phone. The phone saves the session to disk for offline analysis.

**Why:** tuning responsiveness in extremely loud rooms is the open problem. Bench playback through speakers doesn't reproduce the mic's behavior near acoustic overload. Raw device audio from the field lets us re-run the whole feature/NN pipeline offline, measure clipping, and build gain/normalization fixes against real data.

## Data-integrity requirements (non-negotiable)

Dropped or laggy frames poison the data. Every design choice below serves these rules:

1. **Raw audio is the authoritative stream.** Analysis and status data are secondary. When the link can't keep up, secondary data is shed first (counted). Audio is shed only as a last resort, and each loss is located to the exact sample.
2. **Alignment is by audio sample index, never by arrival time.** Every audio packet carries the absolute index of its first sample. Every analysis frame carries the sample index at which its FFT window ended. BLE latency and jitter therefore have no effect on alignment.
3. **No silent loss anywhere.** Each packet type has its own sequence number. The device counts every dropped sample and frame and reports the counters in status packets. The converter refuses to zero-fill gaps: it splits the audio into contiguous segments and writes a gap manifest.
4. **Recording must not perturb the device.** The ISR tap is an O(n) copy. Sending runs in its own FreeRTOS task, decoupled from the render loop. Status packets report loop FPS and FFT-ring overruns, so any perturbation shows up in the data itself.

## Bandwidth budget

| Stream | Rate | Bytes/s |
|---|---|---|
| Raw PCM, 16 kHz int16 mono | 118 samples per 244 B packet → ~136 pkt/s | ~33,100 |
| Analysis frame (compact, ~36 B) | 62.5 Hz | ~2,300 |
| Analysis frame with 50×u8 mel (`mel` flag) | 62.5 Hz | ~5,400 |
| Status | 1 Hz | ~60 |
| **Total (default / with mel)** | | **~35.5 / ~38.6 kB/s** |

This needs MTU 247, Data Length Extension, the 2M PHY and a ~15 ms connection interval. nRF52840 → Android Chrome sustains well above this on a properly configured link. **At today's firmware config it's impossible** (see Phase 1).

## Status (2026-10-14)

Phases 1–4 are implemented and compile/test clean, but **nothing has run on hardware yet.** The bench verification below is the outstanding work, and it's a hard gate before any concert use.

| Piece | Location |
|---|---|
| Firmware record mode | `blinky-things/comms/RecordStream.{h,cpp}`, `record start [mel] / stop / status` in `SerialConsole.cpp` |
| Phase 1 fixes | `blinky-things.ino` (bandwidth + begin check), `BleNus.cpp`, `AudioTracker.cpp` + `SharedSpectralAnalysis.*` (accumulator), `AdaptiveMic.*` (clip telemetry, record tap) |
| Console | `blinky-console/src/pages/RecordPage.tsx` (`#record`), `src/services/record/*`, `src/services/transport/WebBluetoothTransport.ts` |
| Converter | `ml-training/scripts/convert_blrec.py` |

The hosted Firebase build was also broken: `build:firebase` inherited `base: '/devices/'`, so asset URLs 404'd. It now passes `--base /`.

## Phase 1: firmware prerequisites (bug fixes)

1. **BLE bandwidth was never configured.** `BleNus::onConnect` requests MTU 247, but the SoftDevice ceiling defaults to MTU 23, event length 3.75 ms and HVN queue depth 1, because `configPrphBandwidth()` is never called before `Bluefruit.begin()`. This caps app-mode NUS too (the fleet server's serial-over-BLE). It does *not* explain the "BLE-DFU negotiates MTU 23" item ([#216](https://github.com/Jdubz/blinky_time/issues/216)): DFU runs in the bootloader, which has its own SoftDevice config.
   - Fix: `Bluefruit.configPrphBandwidth(BANDWIDTH_MAX)`, which is Adafruit's tested config (MTU 247, event length 125 ms, HVN queue 3) and fits the stock linker RAM start for 1 peripheral.
   - On connect, also request the 2M PHY, Data Length Extension and a 15 ms connection interval.
2. **The `Bluefruit.begin()` return value is ignored.** If SoftDevice enable failed, BLE would silently be dead, including the BLE-DFU recovery path. Log `[FALLBACK]` loudly.
3. **The `BleNus` TX ring drops bytes silently when full.** Count the dropped bytes, log once, and surface the count in `ble` diagnostics.
4. **The spectral accumulator drops samples silently.** `AudioTracker` reads up to `FFT_SIZE` samples from the mic ring, but `SharedSpectralAnalysis::addSamples` only accepts `FFT_SIZE - sampleCount_`.
   - After a partial read, the next read discards up to ~200 samples without a log. FFT windows are then non-contiguous, the frame rate falls below 62.5 Hz, and the frames diverge from the contiguous hop-256 framing the NN was trained on.
   - Fix: read only the samples the accumulator needs.
   - **This changes device behavior.** Verify on a bench chip before fleet rollout.
5. **There is no clipping telemetry.** The ISR computes a per-buffer peak, but nothing reads it. Fix: count samples at |x| ≥ 32767 in the ISR (always on, cheap) and track the peak since the last read. Expose both in `json info`, the NN stream and record status.
   - Note: an analog mic can overload *below* digital full scale (soft flat-topping). Only the raw audio shows that, which is another reason the raw stream is the primary payload.

## Phase 2: firmware record mode

**Transport:** a new GATT service, **Blinky Record**:
- service `b1a70000-7d1e-4f0e-9c3a-5265636f7264`
- one notify characteristic, data `b1a70001-…`

Commands and text responses stay on NUS.

**Commands (over NUS or USB):**
- `record start [mel]`: refuses unless a BLE central has subscribed to the data characteristic. It turns off `stream` modes (they would compete for the link) and replies with a JSON header: build, device id, sample rate, hw gain, MTU, samples per packet, mel band count.
- `record stop`: flushes and sends an END packet with the final counters.
- `record status`: JSON counters (sent, dropped, ring high-water mark).

On BLE disconnect, the recording stops. The phone reconnects and starts a new segment, so the session file holds explicit segment boundaries rather than a silent splice.

**Audio path:**
- The PDM ISR copies samples into a dedicated SPSC record ring of 16,384 int16 samples (32 KB static, 1.02 s of slack).
- When the ring is full, the ISR drops the newest samples and counts them. The next audio packet's start index exposes the gap exactly.
- The record ring's sample counter is the same absolute counter as the FFT ring (`s_fftWriteIdx`), so audio and analysis share one timeline.

**Analysis path:**
- After each spectral frame, the loop pushes a compact frame packet into a 64-slot SPSC queue. Fields: sample index at window end, `nna`, pulse strength, level, raw level, peak abs, clip count, PLP phase, BPM, flags (NN ready, pulse fired, overrun-since-last-frame), plus optional mel.
- When the queue is full, the frame is dropped and counted.

**Sender task:**
- A dedicated FreeRTOS task at `TASK_PRIO_LOW`, the same priority as the loop. It blocks on the HVN semaphore inside `notify()`, so it uses no CPU while waiting.
- Priority order: (1) audio once a full packet's worth of samples is available, (2) a frame or status packet, but only while the audio ring is below half full.
- A notify that times out is retried with the same packet, never skipped.

**Packet format** (little-endian, one packet per notification, ≤ MTU-3):

```
common header (4 B): u8 type, u8 flags, u16 seq   (seq is per-type)
type 1 AUDIO : u32 sampleIndex, int16 samples[N]          (N = (mtu-3-8)/2)
type 2 FRAME : u32 sampleIndex, u32 ms, payload (see RecordStream.h)
type 3 STATUS: u32 ms, counters (see RecordStream.h)
type 4 END   : u32 ms, final counters
```

`RecordStream.h` is the canonical layout. The console parser and the Python converter mirror it, and a version byte in the start header guards against drift.

## Phase 3: console record page (Android Chrome)

- **`WebBluetoothTransport`:** a NUS line transport implementing the existing `Transport` interface. It reassembles lines from notifications and chunks writes to MTU.
- **`RecordClient`:** subscribes to the data characteristic, parses packets, and tracks per-type sequence gaps and audio sample-index gaps live.
- **`RecordPage`:** a standalone mobile-first page (`#record`), reachable without blinky-server. It provides:
  - Connect, Start/Stop and the mel toggle
  - live tiles: elapsed time, kB/s, audio gaps, frames dropped, clip count, peak dBFS, device FPS, RSSI
  - Screen Wake Lock while recording
- **Storage:** packets are appended to IndexedDB in ~1 s chunks, so a tab crash loses at most ~1 s. A session list allows export and delete.
- **Export:** a `.blrec` file = magic + JSON header length + JSON header (start reply, phone timestamps, segment list) + the raw packet stream, each packet length-prefixed with a phone receive timestamp.

## Phase 4: offline tooling

`ml-training/scripts/convert_blrec.py`:
- **Audio:** one 16 kHz WAV per contiguous run of samples. Gaps are never zero-filled; they go into `gaps.json`.
- **Analysis frames:** `frames.jsonl` with capture-nn-compatible field names (`ts`, `nna`, `lvl`, `gain`, plus `sample_index`; `mel` as floats when recorded). This feeds `replay_device_capture.py` and `mel_distribution_check.py`.
- **Status:** `status.jsonl`.
- **Summary:** clip-rate and peak-level summary, plus a loud warning when any loss occurred.

## Verification (bench, before any concert)

1. **Bare chip only** (see CLAUDE.md upload safety). Phase 1 changes touch the BLE stack that BLE-DFU recovery depends on.
   - Confirm the boot log shows SoftDevice enable OK.
   - Confirm `ble` reports MTU 247 to the Android phone.
   - Confirm BLE-DFU still works.
2. **Throughput soak:** 30 min recording on the phone, screen on, at 1 m and 10 m. Pass criteria:
   - 0 audio samples dropped
   - 0 frames dropped
   - device loop FPS within 5% of not-recording
   - FFT overruns unchanged
3. **Alignment check:** play a click track and confirm the frame `sampleIndex` vs audio onset offsets are constant (±1 hop).
4. **Background behavior:** what Chrome does to a Web Bluetooth page when the screen locks is unverified. Until it's tested, the Wake Lock keeps the screen on.

## Known side effects to watch on the bench

- **NUS shares the notification queue with the record sender.** The SoftDevice HVN queue is per connection. While recording, any NUS text output (command replies) makes the loop task wait in `BleNus::drainTxBuffer`, up to 100 ms per chunk, for a queue slot. Keep console output quiet during a session, and watch the `maxFrameMs` STATUS field.
- **The fleet scanner gets less radio time while a phone is connected.** Long connection events cut into it, so fleet broadcasts may be missed during a session.
- **The accumulator fix changes spectral timing.** `OSS_FRAME_RATE = 66` in `AudioTracker.h` assumes one OSS sample per loop update with a new frame. That cadence is unchanged, but windows are now contiguous. Compare BPM lock and onset feel before and after.

## Open questions

- **Record-scoped hardware-gain override (not persisted).** Recording at a lower hw gain gives clipping headroom, and offline digital gain can recreate any higher setting. It's not built yet because it changes device behavior during the capture; decide after the first field recording shows whether gain 32 clips digitally.
- **NUS has no pairing.** A crowd could connect and send `bootloader`. That's out of scope here, but record sessions make the exposure more likely.
