/**
 * Test runner - executes patterns and measures detection performance
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy detection mode switching has been removed.
 */
import { spawn } from 'child_process';
import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { EventEmitter } from 'events';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { PARAMETERS } from './types.js';
const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
// Path to test player CLI (use dist/index.js compiled version)
const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'dist', 'index.js');
const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;
export class TestRunner extends EventEmitter {
    options;
    port = null;
    parser = null;
    portPath;
    streaming = false;
    pendingCommand = null;
    // Test recording state
    testStartTime = null;
    transientBuffer = [];
    audioSampleBuffer = [];
    constructor(options) {
        super();
        this.options = options;
        this.portPath = options.port;
    }
    async connect() {
        if (this.port) {
            return;
        }
        return new Promise((resolve, reject) => {
            this.port = new SerialPort({
                path: this.portPath,
                baudRate: BAUD_RATE,
            });
            this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));
            this.port.on('error', (err) => {
                this.emit('error', err);
                reject(err);
            });
            this.port.on('close', () => {
                this.port = null;
                this.parser = null;
                this.streaming = false;
            });
            this.parser.on('data', (line) => {
                this.handleLine(line.trim());
            });
            this.port.on('open', async () => {
                // Small delay for device to be ready
                await new Promise(r => setTimeout(r, 500));
                resolve();
            });
        });
    }
    async disconnect() {
        if (this.streaming) {
            await this.stopStream();
        }
        if (this.port && this.port.isOpen) {
            return new Promise((resolve) => {
                this.port.close(() => {
                    this.port = null;
                    this.parser = null;
                    resolve();
                });
            });
        }
    }
    async sendCommand(command) {
        if (!this.port || !this.port.isOpen) {
            throw new Error('Not connected');
        }
        const wasStreaming = this.streaming;
        if (wasStreaming) {
            await this.stopStream();
        }
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.pendingCommand = null;
                reject(new Error(`Command timeout: ${command}`));
            }, COMMAND_TIMEOUT_MS);
            this.pendingCommand = { resolve, reject, timeout };
            this.port.write(command + '\n');
        }).then(async (result) => {
            if (wasStreaming) {
                await this.startStream();
            }
            return result;
        });
    }
    async startStream() {
        if (!this.port || !this.port.isOpen) {
            throw new Error('Not connected');
        }
        this.port.write('stream fast\n');
        this.streaming = true;
    }
    async stopStream() {
        if (!this.port || !this.port.isOpen) {
            return;
        }
        this.port.write('stream off\n');
        this.streaming = false;
    }
    handleLine(line) {
        // Check for JSON audio data
        if (line.startsWith('{"a":')) {
            try {
                const parsed = JSON.parse(line);
                const audio = parsed.a;
                // If in test mode, record transients and audio samples
                if (this.testStartTime !== null) {
                    const timestampMs = Date.now() - this.testStartTime;
                    this.audioSampleBuffer.push({
                        timestampMs,
                        level: audio.l,
                        raw: audio.raw,
                        transient: audio.t || 0,
                    });
                    if (audio.t > 0) {
                        this.transientBuffer.push({
                            timestampMs,
                            type: 'unified',
                            strength: audio.t,
                        });
                    }
                }
            }
            catch {
                // Ignore parse errors
            }
            return;
        }
        // Check for pending command response
        if (this.pendingCommand) {
            clearTimeout(this.pendingCommand.timeout);
            this.pendingCommand.resolve(line);
            this.pendingCommand = null;
        }
    }
    /**
     * Set a single parameter using the new ensemble command format
     */
    async setParameter(name, value) {
        const param = PARAMETERS[name];
        if (param && param.command) {
            // Use the custom command format (e.g., "detector_thresh drummer")
            await this.sendCommand(`set ${param.command} ${value}`);
        }
        else {
            // Fall back to direct parameter name (for non-ensemble params like musicthresh)
            await this.sendCommand(`set ${name} ${value}`);
        }
    }
    /**
     * Set multiple parameters
     */
    async setParameters(params) {
        for (const [name, value] of Object.entries(params)) {
            await this.setParameter(name, value);
        }
    }
    /**
     * Set detector enabled state
     */
    async setDetectorEnabled(detector, enabled) {
        await this.sendCommand(`set detector_enable ${detector} ${enabled ? 1 : 0}`);
    }
    /**
     * Set detector weight
     */
    async setDetectorWeight(detector, weight) {
        await this.sendCommand(`set detector_weight ${detector} ${weight}`);
    }
    /**
     * Set detector threshold
     */
    async setDetectorThreshold(detector, threshold) {
        await this.sendCommand(`set detector_thresh ${detector} ${threshold}`);
    }
    /**
     * Set agreement boost value
     */
    async setAgreementBoost(level, boost) {
        if (level < 0 || level > 6) {
            throw new Error('Agreement level must be 0-6');
        }
        await this.sendCommand(`set agree_${level} ${boost}`);
    }
    /**
     * Reset parameters to defaults for ensemble
     */
    async resetDefaults() {
        const ensembleParams = Object.values(PARAMETERS).filter(p => p.mode === 'ensemble');
        for (const param of ensembleParams) {
            await this.setParameter(param.name, param.default);
        }
    }
    /**
     * Save current settings to device flash memory
     */
    async saveToFlash() {
        await this.sendCommand('save');
        // Give the device time to complete the flash write
        await new Promise(r => setTimeout(r, 500));
    }
    /**
     * Get current parameter value from device
     */
    async getParameter(name) {
        const response = await this.sendCommand(`show ${name}`);
        // Parse response like "hitthresh: 2.0"
        const match = response.match(/:\s*([\d.]+)/);
        if (match) {
            return parseFloat(match[1]);
        }
        throw new Error(`Failed to parse parameter value: ${response}`);
    }
    /**
     * Run a single test pattern and return results
     */
    async runPattern(patternId) {
        // Lock gain if specified
        if (this.options.gain !== undefined) {
            await this.sendCommand(`test lock hwgain ${this.options.gain}`);
        }
        // Clear buffers and start streaming
        this.transientBuffer = [];
        this.audioSampleBuffer = [];
        await this.startStream();
        // Run the test player CLI
        const result = await new Promise((resolve) => {
            const child = spawn('node', [TEST_PLAYER_PATH, 'play', patternId, '--quiet'], {
                stdio: ['ignore', 'pipe', 'pipe'],
            });
            let stdout = '';
            let stderr = '';
            // Start recording when the process starts
            this.testStartTime = Date.now();
            child.stdout.on('data', (data) => {
                stdout += data.toString();
            });
            child.stderr.on('data', (data) => {
                stderr += data.toString();
            });
            child.on('close', (code) => {
                if (code === 0) {
                    try {
                        const groundTruth = JSON.parse(stdout);
                        resolve({ success: true, groundTruth });
                    }
                    catch {
                        resolve({ success: false, error: 'Failed to parse ground truth: ' + stdout });
                    }
                }
                else {
                    resolve({ success: false, error: stderr || `Process exited with code ${code}` });
                }
            });
            child.on('error', (err) => {
                resolve({ success: false, error: err.message });
            });
        });
        // Stop recording
        const recordStopTime = Date.now();
        const rawDuration = recordStopTime - (this.testStartTime || recordStopTime);
        let detections = [...this.transientBuffer];
        const recordStartTime = this.testStartTime;
        this.testStartTime = null;
        this.transientBuffer = [];
        this.audioSampleBuffer = [];
        await this.stopStream();
        // Unlock gain
        if (this.options.gain !== undefined) {
            await this.sendCommand('test unlock hwgain');
        }
        if (!result.success) {
            throw new Error(result.error || 'Test failed');
        }
        // Calculate metrics
        const groundTruth = result.groundTruth;
        // Calculate timing offset
        let timingOffsetMs = 0;
        if (groundTruth.startedAt && recordStartTime) {
            const audioStartTime = new Date(groundTruth.startedAt).getTime();
            timingOffsetMs = audioStartTime - recordStartTime;
            detections = detections.map(d => ({
                ...d,
                timestampMs: d.timestampMs - timingOffsetMs,
            })).filter(d => d.timestampMs >= 0);
        }
        // Calculate F1/precision/recall
        const TIMING_TOLERANCE_MS = 350;
        const STRONG_BEAT_THRESHOLD = 0.8;
        const allHits = groundTruth.hits || [];
        const expectedHits = allHits.filter((h) => {
            if (typeof h.expectTrigger === 'boolean') {
                return h.expectTrigger;
            }
            return h.strength >= STRONG_BEAT_THRESHOLD;
        });
        // Estimate audio latency
        const offsets = [];
        detections.forEach((detection) => {
            let minDist = Infinity;
            let closestOffset = 0;
            expectedHits.forEach((expected) => {
                if (detection.type !== 'unified' && expected.type !== detection.type)
                    return;
                const offset = detection.timestampMs - expected.timeMs;
                if (Math.abs(offset) < Math.abs(minDist)) {
                    minDist = offset;
                    closestOffset = offset;
                }
            });
            if (Math.abs(minDist) < 1000) {
                offsets.push(closestOffset);
            }
        });
        let audioLatencyMs = 0;
        if (offsets.length > 0) {
            offsets.sort((a, b) => a - b);
            audioLatencyMs = offsets[Math.floor(offsets.length / 2)];
        }
        // Match detections to expected hits
        const matchedExpected = new Set();
        const matchedDetections = new Set();
        const matchPairs = new Map();
        detections.forEach((detection, dIdx) => {
            let bestMatchIdx = -1;
            let bestMatchDist = Infinity;
            const correctedTime = detection.timestampMs - audioLatencyMs;
            expectedHits.forEach((expected, eIdx) => {
                if (matchedExpected.has(eIdx))
                    return;
                if (detection.type !== 'unified' && expected.type !== detection.type)
                    return;
                const dist = Math.abs(correctedTime - expected.timeMs);
                if (dist < bestMatchDist && dist <= TIMING_TOLERANCE_MS) {
                    bestMatchDist = dist;
                    bestMatchIdx = eIdx;
                }
            });
            if (bestMatchIdx >= 0) {
                matchedExpected.add(bestMatchIdx);
                matchedDetections.add(dIdx);
                matchPairs.set(dIdx, { expectedIdx: bestMatchIdx, timingError: bestMatchDist });
            }
        });
        const truePositives = matchedDetections.size;
        const falsePositives = detections.length - truePositives;
        const falseNegatives = expectedHits.length - truePositives;
        const precision = detections.length > 0 ? truePositives / detections.length : 0;
        const recall = expectedHits.length > 0 ? truePositives / expectedHits.length : 0;
        const f1 = (precision + recall) > 0
            ? 2 * (precision * recall) / (precision + recall)
            : 0;
        let totalTimingError = 0;
        matchPairs.forEach(({ timingError }) => {
            totalTimingError += timingError;
        });
        const avgTimingErrorMs = matchPairs.size > 0 ? totalTimingError / matchPairs.size : null;
        return {
            pattern: patternId,
            durationMs: groundTruth.durationMs || rawDuration,
            f1: Math.round(f1 * 1000) / 1000,
            precision: Math.round(precision * 1000) / 1000,
            recall: Math.round(recall * 1000) / 1000,
            truePositives,
            falsePositives,
            falseNegatives,
            expectedTotal: expectedHits.length,
            avgTimingErrorMs: avgTimingErrorMs !== null ? Math.round(avgTimingErrorMs) : null,
            audioLatencyMs: Math.round(audioLatencyMs),
        };
    }
    /**
     * Run multiple patterns and return aggregated results
     */
    async runPatterns(patterns) {
        const byPattern = {};
        let totalF1 = 0;
        let totalPrecision = 0;
        let totalRecall = 0;
        for (const pattern of patterns) {
            const result = await this.runPattern(pattern);
            byPattern[pattern] = result;
            totalF1 += result.f1;
            totalPrecision += result.precision;
            totalRecall += result.recall;
        }
        const n = Object.keys(byPattern).length;
        return {
            byPattern,
            avgF1: n > 0 ? Math.round((totalF1 / n) * 1000) / 1000 : 0,
            avgPrecision: n > 0 ? Math.round((totalPrecision / n) * 1000) / 1000 : 0,
            avgRecall: n > 0 ? Math.round((totalRecall / n) * 1000) / 1000 : 0,
        };
    }
}
