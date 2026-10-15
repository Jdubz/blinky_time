#include "RecordStream.h"
#include "../audio/AudioTracker.h"
#include "../audio/LoopMetrics.h"
#include "../inputs/AdaptiveMic.h"
#include "../types/Version.h"

using namespace RecordProto;

// b1a70000-7d1e-4f0e-9c3a-5265636f7264 ("...Record"), LSB-first as Bluefruit expects.
const uint8_t RecordStream::SERVICE_UUID[16] = {
    0x64, 0x72, 0x6f, 0x63, 0x65, 0x52, 0x3a, 0x9c,
    0x0e, 0x4f, 0x1e, 0x7d, 0x00, 0x00, 0xa7, 0xb1,
};
// b1a70001-7d1e-4f0e-9c3a-5265636f7264
const uint8_t RecordStream::DATA_UUID[16] = {
    0x64, 0x72, 0x6f, 0x63, 0x65, 0x52, 0x3a, 0x9c,
    0x0e, 0x4f, 0x1e, 0x7d, 0x01, 0x00, 0xa7, 0xb1,
};

RecordStream* RecordStream::instance_ = nullptr;
int16_t* RecordStream::ring_ = nullptr;

// Largest notification payload we ever build (MTU 247 - 3).
static constexpr uint16_t MAX_PAYLOAD = 244;
static constexpr uint16_t AUDIO_OVERHEAD = sizeof(Header) + sizeof(AudioPayload);
static constexpr uint16_t MAX_SAMPLES_PER_PACKET = (MAX_PAYLOAD - AUDIO_OVERHEAD) / 2;

// After an audio-ring overrun, resume this far behind the producer so we
// don't immediately overrun again while the link recovers.
static constexpr uint32_t OVERRUN_RESUME_BACKLOG = 4096;

static inline uint16_t q16(float v) {
    // Inputs are already clamped/asserted finite upstream (AudioTracker clamps
    // the NN output, mel/level are clamp01'd); !(v > 0) only guards the cast.
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 65535;
    return (uint16_t)(v * 65535.0f + 0.5f);
}
static inline uint8_t q8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

void RecordStream::begin(AudioTracker* audio, AdaptiveMic* mic) {
    instance_ = this;
    audio_ = audio;
    mic_ = mic;

    // GATT registration can fail (e.g. attribute table full). Record mode is
    // optional: on failure leave it disabled (serviceReady_ stays false, so
    // start() refuses) and say so — never half-register and carry on.
    err_t svcErr = service_.begin();
    if (svcErr != ERROR_NONE) {
        Serial.print(F("[FALLBACK] Record service begin failed err=0x"));
        Serial.println((unsigned)svcErr, HEX);
        return;
    }
    dataChr_.setProperties(CHR_PROPS_NOTIFY);
    dataChr_.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    dataChr_.setMaxLen(MAX_PAYLOAD);
    err_t chrErr = dataChr_.begin();
    if (chrErr != ERROR_NONE) {
        Serial.print(F("[FALLBACK] Record characteristic begin failed err=0x"));
        Serial.println((unsigned)chrErr, HEX);
        return;
    }

    if (audio_) audio_->setFrameHook(onFrameHook, this);
    serviceReady_ = true;
    // The sender task (3 KB stack) and the 32 KB audio ring are created on
    // the first `record start`, so devices that never record keep their
    // boot-time heap exactly as before.
    Serial.println(F("[BLE] Record service started"));
}

// ---------------------------------------------------------------------------
// Control (console: loop task for USB, BLE callback task for NUS)
// ---------------------------------------------------------------------------

bool RecordStream::start(bool withMel, Print& out) {
    if (!serviceReady_) {
        out.println(F("{\"error\":\"record unavailable (service not registered)\"}"));
        return false;
    }
    if (active_) {
        out.println(F("{\"error\":\"already recording\"}"));
        return false;
    }
    uint16_t conn = Bluefruit.connHandle();
    if (conn == BLE_CONN_HANDLE_INVALID || !Bluefruit.connected(conn)) {
        out.println(F("{\"error\":\"no BLE central connected\"}"));
        return false;
    }
    if (!dataChr_.notifyEnabled(conn)) {
        out.println(F("{\"error\":\"subscribe to record data characteristic before 'record start'\"}"));
        return false;
    }
    BLEConnection* c = Bluefruit.Connection(conn);
    if (!c) {
        out.println(F("{\"error\":\"BLE connection lookup failed\"}"));
        return false;
    }
    uint16_t payload = c->getMtu() - 3;
    if (payload < MIN_PAYLOAD) {
        // Refuse rather than degrade: at MTU 23 the link can't carry 16 kHz
        // PCM, and a recording full of gaps is worse than none.
        out.print(F("{\"error\":\"MTU too small for record mode\",\"mtu\":"));
        out.print(c->getMtu());
        out.print(F(",\"need\":"));
        out.print(MIN_PAYLOAD + 3);
        out.println(F("}"));
        return false;
    }
    if (payload > MAX_PAYLOAD) payload = MAX_PAYLOAD;

    if (!task_) {
        // Sender task: same priority as loop. It spends its life blocked —
        // on the task notification while idle, on the HVN semaphore inside
        // notify() while streaming — so it costs ~nothing between packets.
        if (xTaskCreate(senderTaskEntry, "rec", 768, this, TASK_PRIO_LOW, &task_) != pdPASS) {
            task_ = nullptr;
            out.println(F("{\"error\":\"record sender task creation failed (out of heap)\"}"));
            Serial.println(F("[FALLBACK] RecordStream sender task creation failed — record refused"));
            return false;
        }
    }
    if (!ring_) {
        ring_ = new (std::nothrow) int16_t[RING_SAMPLES];
        if (!ring_) {
            out.println(F("{\"error\":\"record ring allocation failed (out of heap)\"}"));
            Serial.println(F("[FALLBACK] RecordStream: 32 KB ring allocation failed — record refused"));
            return false;
        }
    }

    // Fresh session. Sender is idle (active_ == false is only set by the
    // sender once it has fully finished), so resetting shared state is safe.
    connHandle_ = conn;
    withMel_ = withMel;
    samplesPerPacket_ = (payload - AUDIO_OVERHEAD) / 2;
    if (samplesPerPacket_ > MAX_SAMPLES_PER_PACKET) samplesPerPacket_ = MAX_SAMPLES_PER_PACKET;
    // Discard anything left in the queue. qTail_ is consumer-owned and the
    // sender is idle; qHead_ stays producer-owned (never written here).
    qTail_ = qHead_;
    session_ = session_ + 1;
    seqAudio_ = seqFrame_ = seqStatus_ = seqEnd_ = 0;
    audioSamplesSent_ = audioSamplesDropped_ = 0;
    framesQueued_ = framesDropped_ = 0;
    ringHighWater_ = 0;
    sendRetries_ = 0;
    stopRequested_ = false;
    lastFrameCount_ = audio_ ? audio_->getSpectral().getFrameCount() : 0;
    lastOverrunCount_ = AdaptiveMic::getOverrunCount();
    clipBase_ = AdaptiveMic::getClipSampleCount();
    overrunBase_ = AdaptiveMic::getOverrunCount();
    overrunSamplesBase_ = AdaptiveMic::getOverrunSamplesLost();
    if (mic_) mic_->takePeakAbs();
    startMs_ = millis();
    lastStatusMs_ = startMs_;

    // Faster connection interval for the duration (15 ms). A request — the
    // central decides; throughput is verified by the drop counters.
    c->requestConnectionParameter(12);
    // Re-request DLE + 2M PHY: the onConnect requests can lose a race with
    // the MTU exchange (NRF_ERROR_BUSY) and are not retried there. Without
    // DLE every 244-byte notification is fragmented into 27-byte LL PDUs.
    c->requestDataLengthUpdate();
    c->requestPHY(BLE_GAP_PHY_2MBPS);
    c->monitorRssi();

    // Arm the ISR tap last; its first block starts exactly at startIdx_.
    // writeIdx_ is initialized inside armRecordTap's critical section — an
    // ISR block landing right after arming must not be overwritten here.
    startIdx_ = AdaptiveMic::armRecordTap(onIsrSamples, writeIdx_);
    readIdx_ = startIdx_;
    active_ = true;
    xTaskNotifyGive(task_);

    out.print(F("{\"record\":\"started\",\"v\":"));
    out.print(VERSION);
    out.print(F(",\"fw\":\""));
    out.print(F(FIRMWARE_VERSION));
    out.print(F("\",\"sn\":\""));
    {
        char snBuf[17];
        snprintf(snBuf, sizeof(snBuf), "%08lX%08lX",
                 (unsigned long)NRF_FICR->DEVICEID[1],
                 (unsigned long)NRF_FICR->DEVICEID[0]);
        out.print(snBuf);
    }
    out.print(F("\",\"sr\":"));
    out.print((uint32_t)SpectralConstants::SAMPLE_RATE);
    out.print(F(",\"gain\":"));
    out.print(mic_ ? mic_->getHwGain() : -1);
    out.print(F(",\"mtu\":"));
    out.print(c->getMtu());
    out.print(F(",\"spp\":"));
    out.print(samplesPerPacket_);
    out.print(F(",\"mel\":"));
    out.print(withMel_ ? NUM_MEL : 0);
    out.print(F(",\"startIdx\":"));
    out.print(startIdx_);
    out.print(F(",\"ms\":"));
    out.print(startMs_);
    out.print(F(",\"ring\":"));
    out.print(RING_SAMPLES);
    out.println(F("}"));
    Serial.println(F("[REC] started"));
    return true;
}

bool RecordStream::stop(Print& out) {
    if (!active_) {
        out.println(F("{\"error\":\"not recording\"}"));
        return false;
    }
    // Cut the tap now: no new samples. The sender flushes what's buffered,
    // sends END, and only then clears active_.
    AdaptiveMic::disarmRecordTap();
    stopRequested_ = true;
    out.print(F("{\"record\":\"stopping\",\"audioSamplesSent\":"));
    out.print(audioSamplesSent_);
    out.print(F(",\"audioSamplesDropped\":"));
    out.print(audioSamplesDropped_);
    out.println(F("}"));
    return true;
}

void RecordStream::printStatus(Print& out) const {
    out.print(F("{\"record\":"));
    out.print(active_ ? (stopRequested_ ? F("\"stopping\"") : F("\"active\"")) : F("\"idle\""));
    out.print(F(",\"audioSamplesSent\":"));
    out.print(audioSamplesSent_);
    out.print(F(",\"audioSamplesDropped\":"));
    out.print(audioSamplesDropped_);
    out.print(F(",\"framesQueued\":"));
    out.print(framesQueued_);
    out.print(F(",\"framesDropped\":"));
    out.print(framesDropped_);
    out.print(F(",\"sendRetries\":"));
    out.print(sendRetries_);
    out.print(F(",\"spp\":"));
    out.print(samplesPerPacket_);
    out.print(F(",\"lastEnd\":\""));
    out.print(lastEndReason_);
    out.println(F("\"}"));
}

// ---------------------------------------------------------------------------
// ISR producer
// ---------------------------------------------------------------------------

void RecordStream::onIsrSamples(const int16_t* samples, int count, uint32_t firstIndex) {
    RecordStream* self = instance_;
    // The tap is only armed after ring_ is allocated (start()); the null
    // check is belt-and-braces for an ISR that must never fault.
    if (!self || !ring_) return;
    // Slot by absolute index: the sender detects overwrite by index math,
    // so the ISR never blocks or coordinates.
    for (int i = 0; i < count; i++) {
        ring_[(firstIndex + (uint32_t)i) & (RING_SAMPLES - 1)] = samples[i];
    }
    self->writeIdx_ = firstIndex + (uint32_t)count;
}

// ---------------------------------------------------------------------------
// Loop-task producer (FRAME + STATUS)
// ---------------------------------------------------------------------------

bool RecordStream::pushSecondary(const uint8_t* data, uint16_t len, uint32_t session) {
    uint16_t next = (uint16_t)((qHead_ + 1) % QUEUE_SLOTS);
    if (next == qTail_) {
        framesDropped_++;
        return false;
    }
    Slot& s = queue_[qHead_];
    memcpy(s.data, data, len);
    s.len = len;
    s.session = session;
    __DMB();  // slot contents visible before the head advance
    qHead_ = next;
    framesQueued_++;
    return true;
}

void RecordStream::buildStatus(uint8_t type, uint16_t seq, uint8_t* buf, uint16_t& len) {
    Header h{type, 0, seq};
    StatusPayload p{};
    p.ms = millis();
    p.audioSamplesSent = audioSamplesSent_;
    p.audioSamplesDropped = audioSamplesDropped_;
    p.framesQueued = framesQueued_;
    p.framesDropped = framesDropped_;
    p.clipSamples = AdaptiveMic::getClipSampleCount() - clipBase_;
    p.fftOverruns = AdaptiveMic::getOverrunCount() - overrunBase_;
    p.fftOverrunSamples = AdaptiveMic::getOverrunSamplesLost() - overrunSamplesBase_;
    p.peakAbs = mic_ ? mic_->takePeakAbs() : 0;
    float fps = LoopMetrics::getFps() * 10.0f;
    p.fpsX10 = (fps > 65535.0f) ? 65535 : (uint16_t)fps;
    uint32_t mf = LoopMetrics::getMaxFrameMs();
    p.maxFrameMs = (mf > 65535) ? 65535 : (uint16_t)mf;
    uint32_t hw = ringHighWater_;
    ringHighWater_ = 0;
    p.ringHighWater = (hw > 65535) ? 65535 : (uint16_t)hw;
    BLEConnection* c = Bluefruit.Connection(connHandle_);
    p.rssi = c ? c->getRssi() : 0;
    p.hwGain = mic_ ? (uint8_t)mic_->getHwGain() : 0;
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &p, sizeof(p));
    len = sizeof(h) + sizeof(p);
}

void RecordStream::onFrameHook(void* ctx) {
    static_cast<RecordStream*>(ctx)->onFrame();
}

void RecordStream::onFrame() {
    if (!active_ || stopRequested_ || !audio_) return;
    const uint32_t session = session_;

    const SharedSpectralAnalysis& spectral = audio_->getSpectral();
    uint32_t fc = spectral.getFrameCount();
    // Every frame is hooked, so this is 0 unless frames were processed
    // before the session's first hook call.
    uint32_t skipped = fc - lastFrameCount_ - 1;
    if (lastFrameCount_ == 0 || fc <= lastFrameCount_) skipped = 0;
    lastFrameCount_ = fc;

    uint8_t flags = 0;
    if (audio_->getFrameOnsetNN().isReady()) flags |= FRAME_NN_READY;
    float pulse = audio_->getLastFramePulse();
    if (pulse > 0.0f) flags |= FRAME_PULSE;
    uint32_t oc = AdaptiveMic::getOverrunCount();
    if (oc != lastOverrunCount_) { flags |= FRAME_FFT_OVERRUN; lastOverrunCount_ = oc; }
    if (withMel_) flags |= FRAME_HAS_MEL;

    uint8_t buf[MAX_SECONDARY_LEN];
    Header h{PKT_FRAME, flags, seqFrame_++};
    FramePayload p{};
    p.sampleIndex = audio_->getLastFrameEndSampleIdx();
    p.ms = millis();
    p.nna = q16(audio_->getRawNNActivation());
    p.pulse = q16(pulse);
    p.level = q16(mic_ ? mic_->getLevel() : 0.0f);
    p.rawLevel = q16(mic_ ? mic_->getRawLevel() : 0.0f);
    float bpm10 = audio_->getCurrentBpm() * 10.0f;
    p.bpm10 = (bpm10 > 0.0f && bpm10 < 65535.0f) ? (uint16_t)(bpm10 + 0.5f) : 0;
    p.phase = q8(audio_->getPlpPhase());
    // Frame-current periodicity (ACF strength), not control_.rhythmStrength:
    // control_ is only synthesized after the whole drain loop, so inside the
    // frame hook it still holds the previous update's value.
    p.rstr = q8(audio_->getPeriodicityStrength());
    p.skipped = (skipped > 255) ? 255 : (uint8_t)skipped;

    uint16_t len = sizeof(h) + sizeof(p);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &p, sizeof(p));
    if (withMel_) {
        const float* mel = spectral.getRawMelBands();
        for (int i = 0; i < NUM_MEL; i++) buf[len + i] = q8(mel[i]);
        len += NUM_MEL;
    }
    pushSecondary(buf, len, session);
}

void RecordStream::onLoop() {
    if (!active_ || stopRequested_ || !audio_) return;
    const uint32_t session = session_;

    uint32_t now = millis();
    if (now - lastStatusMs_ >= 1000) {
        lastStatusMs_ = now;
        uint8_t buf[MAX_SECONDARY_LEN];
        uint16_t len = 0;
        buildStatus(PKT_STATUS, seqStatus_++, buf, len);
        pushSecondary(buf, len, session);
    }
}

// ---------------------------------------------------------------------------
// Sender task (sole consumer)
// ---------------------------------------------------------------------------

void RecordStream::senderTaskEntry(void* arg) {
    static_cast<RecordStream*>(arg)->senderLoop();
}

bool RecordStream::sendPacket(const uint8_t* data, uint16_t len) {
    // notify() waits up to BLE_GENERIC_TIMEOUT (100 ms) for an HVN buffer.
    // A false return with the link still up is back-pressure: the caller
    // retries the SAME packet (never skips it).
    if (dataChr_.notify(connHandle_, data, len)) return true;
    sendRetries_++;
    vTaskDelay(1);  // never spin if notify fails fast (e.g. hvx error)
    return false;
}

void RecordStream::finishLocked(const char* reason) {
    AdaptiveMic::disarmRecordTap();
    lastEndReason_ = reason;
    // Log before releasing the session: once active_ is false, start() may
    // reset these counters from another task.
    Serial.print(F("[REC] ended: "));
    Serial.print(reason);
    Serial.print(F(" sent="));
    Serial.print(audioSamplesSent_);
    Serial.print(F(" dropped="));
    Serial.print(audioSamplesDropped_);
    Serial.print(F(" framesDropped="));
    Serial.println(framesDropped_);
    // active_ first: clearing stopRequested_ while active_ is still true
    // would briefly re-enable onLoop() production for a dead session.
    active_ = false;
    stopRequested_ = false;
}

void RecordStream::senderLoop() {
    static uint8_t pkt[MAX_PAYLOAD];
    bool overrunLogged = false;

    for (;;) {
        if (!active_) {
            overrunLogged = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (!Bluefruit.connected(connHandle_) || !dataChr_.notifyEnabled(connHandle_)) {
            // Phone gone or unsubscribed. The session ends here; the phone
            // starts a new segment on reconnect (explicit boundary, never a
            // silent splice).
            finishLocked("link lost");
            continue;
        }

        uint32_t w = writeIdx_;
        uint32_t backlog = w - readIdx_;
        if (backlog > RING_SAMPLES) {
            // Producer lapped us: the oldest unsent audio is gone. Skip
            // ahead, count every lost sample — the next AUDIO packet's
            // sampleIndex marks the gap exactly.
            uint32_t newRead = w - OVERRUN_RESUME_BACKLOG;
            audioSamplesDropped_ += newRead - readIdx_;
            readIdx_ = newRead;
            backlog = OVERRUN_RESUME_BACKLOG;
            if (!overrunLogged) {
                overrunLogged = true;
                Serial.println(F("[FALLBACK] Record audio ring overrun — BLE link too slow, samples dropped (counted)"));
            }
        }
        if (backlog > ringHighWater_) ringHighWater_ = backlog;

        bool haveSecondary = (qTail_ != qHead_);

        // Secondary data only while audio has comfortable headroom.
        if (haveSecondary && backlog < RING_SAMPLES / 2) {
            const Slot& s = queue_[qTail_];
            if (s.session != session_) {
                // Straggler from a previous session — never send it.
                __DMB();
                qTail_ = (uint16_t)((qTail_ + 1) % QUEUE_SLOTS);
                continue;
            }
            if (sendPacket(s.data, s.len)) {
                __DMB();
                qTail_ = (uint16_t)((qTail_ + 1) % QUEUE_SLOTS);
            }
            continue;
        }

        uint32_t n = 0;
        if (backlog >= samplesPerPacket_) n = samplesPerPacket_;
        else if (stopRequested_ && backlog > 0) n = backlog;   // final partial packet

        if (n > 0) {
            Header h{PKT_AUDIO, 0, seqAudio_};
            AudioPayload a{readIdx_};
            memcpy(pkt, &h, sizeof(h));
            memcpy(pkt + sizeof(h), &a, sizeof(a));
            int16_t* out = reinterpret_cast<int16_t*>(pkt + AUDIO_OVERHEAD);
            for (uint32_t i = 0; i < n; i++) {
                out[i] = ring_[(readIdx_ + i) & (RING_SAMPLES - 1)];
            }
            // Seqlock check: if the ISR lapped this packet's span while we
            // copied, the copy is torn. Loop back — the overrun branch
            // above skips ahead and counts the loss.
            if ((uint32_t)(writeIdx_ - readIdx_) > RING_SAMPLES) continue;

            if (sendPacket(pkt, (uint16_t)(AUDIO_OVERHEAD + n * 2))) {
                seqAudio_++;
                readIdx_ += n;
                audioSamplesSent_ += n;
            }
            continue;
        }

        if (stopRequested_ && !haveSecondary) {
            // Fully flushed. END carries the final counters.
            // The loop stopped producing once stopRequested_ was set, so the
            // sender may read the loop-owned telemetry here.
            uint16_t len = 0;
            buildStatus(PKT_END, seqEnd_, pkt, len);  // END reuses the STATUS layout
            if (sendPacket(pkt, len)) {
                seqEnd_++;
                finishLocked("stopped");
            }
            continue;
        }

        // Nothing ready: a full audio packet accumulates in ~7 ms.
        vTaskDelay(1);
    }
}
