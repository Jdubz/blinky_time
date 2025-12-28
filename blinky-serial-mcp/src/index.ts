#!/usr/bin/env node
/**
 * Blinky Serial MCP Server
 *
 * Provides tools for interacting with blinky devices via serial port.
 */

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import { BlinkySerial } from './serial.js';
import type { AudioSample, TransientEvent, MusicModeState, BeatEvent } from './types.js';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { writeFileSync, mkdirSync, existsSync } from 'fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to test player CLI
const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'blinky-test-player', 'dist', 'index.js');

// Path to test results directory
const TEST_RESULTS_DIR = join(__dirname, '..', '..', 'test-results');

// Ensure test results directory exists
function ensureTestResultsDir(): void {
  if (!existsSync(TEST_RESULTS_DIR)) {
    mkdirSync(TEST_RESULTS_DIR, { recursive: true });
  }
}

// Global serial connection
const serial = new BlinkySerial();

// Audio sample buffer for monitoring
let lastAudioSample: AudioSample | null = null;
let audioSampleCount = 0;

// Transient event buffer for test mode
let transientBuffer: TransientEvent[] = [];
let testStartTime: number | null = null;

// Audio sample buffer for test mode (with timestamps)
interface TimestampedSample {
  timestampMs: number;
  level: number;
  raw: number;
  transient: number;  // Unified transient strength from "Drummer's Algorithm"
}
let audioSampleBuffer: TimestampedSample[] = [];

// Music mode buffers for test mode
interface TimestampedMusicState {
  timestampMs: number;
  active: boolean;
  bpm: number;
  phase: number;
  confidence: number;
}
let musicStateBuffer: TimestampedMusicState[] = [];
let beatEventBuffer: BeatEvent[] = [];
let lastMusicState: MusicModeState | null = null;

// Set up event listeners
serial.on('audio', (sample: AudioSample) => {
  lastAudioSample = sample;
  audioSampleCount++;

  // If in test mode, record transients and audio samples
  if (testStartTime !== null) {
    const timestampMs = Date.now() - testStartTime;

    // Record audio sample
    audioSampleBuffer.push({
      timestampMs,
      level: sample.l,
      raw: sample.raw,
      transient: sample.t || 0,  // Record transient for debugging
    });

    // Record transients (unified detection using 't' field)
    // The simplified "Drummer's Algorithm" outputs a single transient strength
    if (sample.t > 0) {
      transientBuffer.push({
        timestampMs,
        type: 'unified',
        strength: sample.t,
      });
    }
  }
});

serial.on('error', (err: Error) => {
  console.error('Serial error:', err.message);
});

// Music mode event listeners
serial.on('music', (state: MusicModeState) => {
  lastMusicState = state;

  // If in test mode, record music state
  if (testStartTime !== null) {
    const timestampMs = Date.now() - testStartTime;
    musicStateBuffer.push({
      timestampMs,
      active: state.a === 1,
      bpm: state.bpm,
      phase: state.ph,
      confidence: state.conf,
    });
  }
});

serial.on('beat', (beat: { type: string; bpm: number }) => {
  // If in test mode, record beat events
  if (testStartTime !== null) {
    const timestampMs = Date.now() - testStartTime;
    beatEventBuffer.push({
      timestampMs,
      bpm: beat.bpm,
      type: beat.type as 'quarter' | 'half' | 'whole',
    });
  }
});

// Create MCP server
const server = new Server(
  {
    name: 'blinky-serial-mcp',
    version: '1.0.0',
  },
  {
    capabilities: {
      tools: {},
    },
  }
);

// List available tools
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return {
    tools: [
      {
        name: 'list_ports',
        description: 'List available serial ports',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'connect',
        description: 'Connect to a blinky device on the specified serial port',
        inputSchema: {
          type: 'object',
          properties: {
            port: {
              type: 'string',
              description: 'Serial port path (e.g., COM3 on Windows, /dev/ttyUSB0 on Linux)',
            },
          },
          required: ['port'],
        },
      },
      {
        name: 'disconnect',
        description: 'Disconnect from the current device',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'status',
        description: 'Get current connection status and device info',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'send_command',
        description: 'Send a raw command to the device and get the response',
        inputSchema: {
          type: 'object',
          properties: {
            command: {
              type: 'string',
              description: 'Command to send (e.g., "show onsetthresh", "set onsetthresh 3.0")',
            },
          },
          required: ['command'],
        },
      },
      {
        name: 'stream_start',
        description: 'Start audio streaming from the device',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'stream_stop',
        description: 'Stop audio streaming',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'get_audio',
        description: 'Get the most recent audio sample data (requires streaming)',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'get_settings',
        description: 'Get all device settings as JSON',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'set_setting',
        description: 'Set a device setting value',
        inputSchema: {
          type: 'object',
          properties: {
            name: {
              type: 'string',
              description: 'Setting name (e.g., "onsetthresh", "risethresh", "cooling")',
            },
            value: {
              type: 'number',
              description: 'New value for the setting',
            },
          },
          required: ['name', 'value'],
        },
      },
      {
        name: 'save_settings',
        description: 'Save current settings to device flash memory',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'reset_defaults',
        description: 'Reset all settings to factory defaults',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'start_test',
        description: 'Start recording transient detections for a test',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'stop_test',
        description: 'Stop recording and get all detected transients',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'run_test',
        description: 'Run a complete test: play a pattern and record detections simultaneously. Automatically connects and disconnects from the device. If gain is specified, locks hardware gain for the test and unlocks afterward.',
        inputSchema: {
          type: 'object',
          properties: {
            pattern: {
              type: 'string',
              description: 'Pattern ID to play (e.g., "simple-beat", "simultaneous")',
            },
            port: {
              type: 'string',
              description: 'Serial port to connect to (e.g., "COM5"). Required.',
            },
            gain: {
              type: 'number',
              description: 'Optional hardware gain to lock during test (0-80). If specified, gain will be locked before test and unlocked (255) after completion.',
            },
          },
          required: ['pattern', 'port'],
        },
      },
      {
        name: 'list_patterns',
        description: 'List available test patterns',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'monitor_audio',
        description: 'Monitor audio for a specified duration and return statistics',
        inputSchema: {
          type: 'object',
          properties: {
            duration_ms: {
              type: 'number',
              description: 'Duration to monitor in milliseconds (default: 1000)',
            },
          },
        },
      },
    ],
  };
});

// Handle tool calls
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      case 'list_ports': {
        const ports = await serial.listPorts();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ ports }, null, 2),
            },
          ],
        };
      }

      case 'connect': {
        const port = (args as { port: string }).port;
        const deviceInfo = await serial.connect(port);
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, deviceInfo }, null, 2),
            },
          ],
        };
      }

      case 'disconnect': {
        await serial.disconnect();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, message: 'Disconnected' }, null, 2),
            },
          ],
        };
      }

      case 'status': {
        const state = serial.getState();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify(state, null, 2),
            },
          ],
        };
      }

      case 'send_command': {
        const command = (args as { command: string }).command;
        const response = await serial.sendCommand(command);
        return {
          content: [
            {
              type: 'text',
              text: response,
            },
          ],
        };
      }

      case 'stream_start': {
        await serial.startStream();
        lastAudioSample = null;
        audioSampleCount = 0;
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, message: 'Streaming started' }, null, 2),
            },
          ],
        };
      }

      case 'stream_stop': {
        await serial.stopStream();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                success: true,
                message: 'Streaming stopped',
                samplesReceived: audioSampleCount,
              }, null, 2),
            },
          ],
        };
      }

      case 'get_audio': {
        const state = serial.getState();
        if (!state.streaming) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({ error: 'Streaming not active. Call stream_start first.' }, null, 2),
              },
            ],
          };
        }
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                sample: lastAudioSample,
                sampleCount: audioSampleCount,
              }, null, 2),
            },
          ],
        };
      }

      case 'get_settings': {
        const settings = await serial.getSettings();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ settings }, null, 2),
            },
          ],
        };
      }

      case 'set_setting': {
        const { name: settingName, value } = args as { name: string; value: number };
        const response = await serial.setSetting(settingName, value);
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, response }, null, 2),
            },
          ],
        };
      }

      case 'save_settings': {
        const response = await serial.saveSettings();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, response }, null, 2),
            },
          ],
        };
      }

      case 'reset_defaults': {
        const response = await serial.resetDefaults();
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ success: true, response }, null, 2),
            },
          ],
        };
      }

      case 'start_test': {
        // Clear buffers and start recording
        transientBuffer = [];
        audioSampleBuffer = [];
        testStartTime = Date.now();

        // Ensure streaming is on
        const state = serial.getState();
        if (!state.streaming) {
          await serial.startStream();
        }

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                success: true,
                message: 'Test started. Transients are being recorded.',
                startTime: testStartTime,
              }, null, 2),
            },
          ],
        };
      }

      case 'stop_test': {
        if (testStartTime === null) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({ error: 'No test in progress' }, null, 2),
              },
            ],
          };
        }

        const duration = Date.now() - testStartTime;
        const detections = [...transientBuffer];
        const audioSamples = [...audioSampleBuffer];

        // Reset test state
        testStartTime = null;
        transientBuffer = [];
        audioSampleBuffer = [];

        // Calculate basic stats
        const lowCount = detections.filter(d => d.type === 'low').length;
        const highCount = detections.filter(d => d.type === 'high').length;

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                success: true,
                durationMs: duration,
                totalDetections: detections.length,
                lowDetections: lowCount,
                highDetections: highCount,
                detections,
                audioSamples,
              }, null, 2),
            },
          ],
        };
      }

      case 'monitor_audio': {
        const durationMs = (args as { duration_ms?: number }).duration_ms || 1000;

        const state = serial.getState();
        if (!state.streaming) {
          await serial.startStream();
        }

        // Collect samples for the duration
        const samples: AudioSample[] = [];
        const startCount = audioSampleCount;

        await new Promise(resolve => setTimeout(resolve, durationMs));

        const endCount = audioSampleCount;
        const sampleRate = (endCount - startCount) / (durationMs / 1000);

        // Get current sample for analysis
        const current = lastAudioSample;

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                durationMs,
                samplesReceived: endCount - startCount,
                sampleRate: sampleRate.toFixed(1) + ' Hz',
                currentSample: current,
              }, null, 2),
            },
          ],
        };
      }

      case 'list_patterns': {
        // Available patterns (must match blinky-test-player)
        const patterns = [
          // Calibrated patterns (deterministic samples with known loudness)
          { id: 'strong-beats', name: 'Strong Beats (Calibrated)', durationMs: 16000, description: 'Hard kicks/snares only - baseline test (120 BPM)', calibrated: true },
          { id: 'medium-beats', name: 'Medium Beats (Calibrated)', durationMs: 16000, description: 'Medium kicks/snares - moderate challenge (120 BPM)', calibrated: true },
          { id: 'soft-beats', name: 'Soft Beats (Calibrated)', durationMs: 16000, description: 'Soft kicks/snares - sensitivity test (120 BPM)', calibrated: true },
          { id: 'hat-rejection', name: 'Hat Rejection (Calibrated)', durationMs: 16000, description: 'Hard beats + soft hats - rejection test (120 BPM)', calibrated: true },
          { id: 'mixed-dynamics', name: 'Mixed Dynamics (Calibrated)', durationMs: 16000, description: 'Varying loudness - realistic simulation (120 BPM)', calibrated: true },
          { id: 'tempo-sweep', name: 'Tempo Sweep (Calibrated)', durationMs: 16000, description: 'Tests 80, 100, 120, 140 BPM', calibrated: true },
          // Melodic/harmonic patterns (bass, synth, lead, pad, chord)
          { id: 'bass-line', name: 'Bass Line (Calibrated)', durationMs: 16000, description: 'Kicks + bass notes - tests low freq transients', calibrated: true },
          { id: 'synth-stabs', name: 'Synth Stabs (Calibrated)', durationMs: 16000, description: 'Sharp synth stabs - should trigger detection', calibrated: true },
          { id: 'lead-melody', name: 'Lead Melody (Calibrated)', durationMs: 19200, description: 'Lead notes + drums - tests melodic transients', calibrated: true },
          { id: 'pad-rejection', name: 'Pad Rejection (Calibrated)', durationMs: 24000, description: 'Sustained pads - tests false positive rejection', calibrated: true },
          { id: 'chord-rejection', name: 'Chord Rejection (Calibrated)', durationMs: 21333, description: 'Sustained chords - tests false positive rejection', calibrated: true },
          { id: 'full-mix', name: 'Full Mix (Calibrated)', durationMs: 16000, description: 'Drums + bass + synth + lead - realistic music', calibrated: true },
          // Legacy patterns (random samples)
          { id: 'basic-drums', name: 'Basic Drum Pattern', durationMs: 16000, description: 'Kick on 1&3, snare on 2&4, hats on 8ths (120 BPM)' },
          { id: 'kick-focus', name: 'Kick Focus', durationMs: 12000, description: 'Various kick patterns - tests low-band detection' },
          { id: 'snare-focus', name: 'Snare Focus', durationMs: 10000, description: 'Snare patterns including rolls - tests high-band detection' },
          { id: 'hat-patterns', name: 'Hi-Hat Patterns', durationMs: 12000, description: 'Various hi-hat patterns: 8ths, 16ths, offbeats' },
          { id: 'full-kit', name: 'Full Drum Kit', durationMs: 16000, description: 'All drum elements: kick, snare, hat, tom, clap' },
          { id: 'simultaneous', name: 'Simultaneous Hits', durationMs: 10000, description: 'Kick + snare/clap at same time - tests concurrent detection' },
          { id: 'fast-tempo', name: 'Fast Tempo (150 BPM)', durationMs: 10000, description: 'High-speed drum pattern - tests detection at fast tempos' },
          { id: 'sparse', name: 'Sparse Pattern', durationMs: 15000, description: 'Widely spaced hits - tests detection after silence' },
        ];
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ patterns }, null, 2),
            },
          ],
        };
      }

      case 'run_test': {
        const { pattern: patternId, port, gain } = args as { pattern: string; port: string; gain?: number };

        // Connect to device (will disconnect at end)
        try {
          if (!serial.getState().connected) {
            await serial.connect(port);
          }

          // Lock hardware gain if specified
          if (gain !== undefined) {
            await serial.sendCommand(`set hwgainlock ${gain}`);
          }

          // Clear buffers and start recording
          transientBuffer = [];
          audioSampleBuffer = [];
          musicStateBuffer = [];
          beatEventBuffer = [];

          // Use fast streaming (100Hz) for tests to catch transient pulses
          // Transients are single-frame pulses that last ~16ms at 60Hz update rate
          // Normal streaming at 20Hz (50ms) would miss most of them
          await serial.sendCommand('stream fast');

        // Run the test player CLI and capture output
        const result = await new Promise<{ success: boolean; groundTruth?: unknown; error?: string }>((resolve) => {
          const child = spawn('node', [TEST_PLAYER_PATH, 'play', patternId, '--quiet'], {
            stdio: ['ignore', 'pipe', 'pipe'],
          });

          let stdout = '';
          let stderr = '';

          // Start recording when the process starts
          testStartTime = Date.now();

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
              } catch {
                resolve({ success: false, error: 'Failed to parse ground truth: ' + stdout });
              }
            } else {
              resolve({ success: false, error: stderr || `Process exited with code ${code}` });
            }
          });

          child.on('error', (err) => {
            resolve({ success: false, error: err.message });
          });
        });

        // Stop recording
        const recordStopTime = Date.now();
        const rawDuration = recordStopTime - (testStartTime || recordStopTime);
        let detections = [...transientBuffer];
        let audioSamples = [...audioSampleBuffer];
        let musicStates = [...musicStateBuffer];
        let beatEvents = [...beatEventBuffer];

        const recordStartTime = testStartTime;
        testStartTime = null;
        transientBuffer = [];
        audioSampleBuffer = [];
        musicStateBuffer = [];
        beatEventBuffer = [];

        if (!result.success) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({ error: result.error }, null, 2),
              },
            ],
          };
        }

        // Calculate timing offset: ground truth startedAt vs our recording start
        // The ground truth contains the actual audio playback start time
        // Our recording started earlier (when CLI process launched)
        const groundTruth = result.groundTruth as {
          pattern: string;
          durationMs: number;
          startedAt: string;
          hits: Array<{ timeMs: number; type: string; strength: number }>;
        };

        let timingOffsetMs = 0;
        if (groundTruth.startedAt && recordStartTime) {
          const audioStartTime = new Date(groundTruth.startedAt).getTime();
          timingOffsetMs = audioStartTime - recordStartTime;

          // Adjust all timestamps to be relative to actual audio start
          detections = detections.map(d => ({
            ...d,
            timestampMs: d.timestampMs - timingOffsetMs,
          })).filter(d => d.timestampMs >= 0); // Remove detections before audio started

          audioSamples = audioSamples.map(s => ({
            ...s,
            timestampMs: s.timestampMs - timingOffsetMs,
          })).filter(s => s.timestampMs >= 0);

          musicStates = musicStates.map(s => ({
            ...s,
            timestampMs: s.timestampMs - timingOffsetMs,
          })).filter(s => s.timestampMs >= 0);

          beatEvents = beatEvents.map(b => ({
            ...b,
            timestampMs: b.timestampMs - timingOffsetMs,
          })).filter(b => b.timestampMs >= 0);
        }

        // Calculate stats
        const lowCount = detections.filter(d => d.type === 'low').length;
        const highCount = detections.filter(d => d.type === 'high').length;
        const duration = groundTruth.durationMs || rawDuration;

        // Calculate F1/precision/recall metrics
        // Match detections to expected hits within a timing tolerance
        const TIMING_TOLERANCE_MS = 350; // Allow 350ms timing variance (accounts for audio output latency)
        const STRONG_BEAT_THRESHOLD = 0.8; // Only count strong beats (kicks, snares) not hi-hats
        const allHits = groundTruth.hits || [];
        // Filter to expected transients only:
        // 1. If expectTrigger is defined, use it (explicit: pads/chords = false)
        // 2. Otherwise, fall back to strength threshold (hi-hats are weak)
        const expectedHits = allHits.filter((h: { strength: number; expectTrigger?: boolean }) => {
          // Use expectTrigger if defined (new patterns with explicit transient marking)
          if (typeof h.expectTrigger === 'boolean') {
            return h.expectTrigger;
          }
          // Fall back to strength threshold for legacy patterns
          return h.strength >= STRONG_BEAT_THRESHOLD;
        });

        // First pass: estimate systematic audio latency by finding median offset
        // This helps compensate for consistent delays (speaker output, air travel, mic processing)
        const offsets: number[] = [];
        detections.forEach((detection) => {
          // Find closest expected hit ('unified' type matches any expected type)
          let minDist = Infinity;
          let closestOffset = 0;
          expectedHits.forEach((expected) => {
            // 'unified' type matches any expected type (we no longer have dual-band detection)
            if (detection.type !== 'unified' && expected.type !== detection.type) return;
            const offset = detection.timestampMs - expected.timeMs;
            if (Math.abs(offset) < Math.abs(minDist)) {
              minDist = offset;
              closestOffset = offset;
            }
          });
          if (Math.abs(minDist) < 1000) { // Only consider reasonable matches
            offsets.push(closestOffset);
          }
        });

        // Calculate median offset as systematic latency estimate
        let audioLatencyMs = 0;
        if (offsets.length > 0) {
          offsets.sort((a, b) => a - b);
          audioLatencyMs = offsets[Math.floor(offsets.length / 2)];
        }

        // Track which expected hits were matched and the mapping
        const matchedExpected = new Set<number>();
        const matchedDetections = new Set<number>();
        // Store actual match pairs: detection index -> { expectedIdx, timingError }
        const matchPairs = new Map<number, { expectedIdx: number; timingError: number }>();

        // Match each detection to nearest expected hit (if within tolerance)
        // Apply latency correction to detection timestamps
        detections.forEach((detection, dIdx) => {
          let bestMatchIdx = -1;
          let bestMatchDist = Infinity;
          const correctedTime = detection.timestampMs - audioLatencyMs;

          expectedHits.forEach((expected, eIdx) => {
            if (matchedExpected.has(eIdx)) return; // Already matched
            // 'unified' type matches any expected type (we no longer have dual-band detection)
            if (detection.type !== 'unified' && expected.type !== detection.type) return;

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
        const f1Score = (precision + recall) > 0
          ? 2 * (precision * recall) / (precision + recall)
          : 0;

        // Calculate average timing error from matched pairs
        let totalTimingError = 0;
        matchPairs.forEach(({ timingError }) => {
          totalTimingError += timingError;
        });
        const avgTimingErrorMs = matchPairs.size > 0 ? totalTimingError / matchPairs.size : null;

        const metrics = {
          f1Score: Math.round(f1Score * 1000) / 1000,
          precision: Math.round(precision * 1000) / 1000,
          recall: Math.round(recall * 1000) / 1000,
          truePositives,
          falsePositives,
          falseNegatives,
          expectedTotal: expectedHits.length,
          avgTimingErrorMs: avgTimingErrorMs !== null ? Math.round(avgTimingErrorMs) : null,
          audioLatencyMs: Math.round(audioLatencyMs),
        };

        // Calculate music mode metrics
        const activeStates = musicStates.filter(s => s.active);
        const activationTime = activeStates.length > 0 ? activeStates[0].timestampMs : null;
        const avgConfidence = activeStates.length > 0
          ? activeStates.reduce((sum, s) => sum + s.confidence, 0) / activeStates.length
          : 0;
        const avgBpm = activeStates.length > 0
          ? activeStates.reduce((sum, s) => sum + s.bpm, 0) / activeStates.length
          : 0;

        // Get expected BPM from ground truth if available
        const expectedBPM = (groundTruth as { bpm?: number }).bpm || 0;
        const bpmError = expectedBPM > 0 && avgBpm > 0
          ? Math.abs(avgBpm - expectedBPM) / expectedBPM * 100
          : null;

        const musicMetrics = {
          activationMs: activationTime,
          avgConfidence: Math.round(avgConfidence * 100) / 100,
          avgBpm: Math.round(avgBpm * 10) / 10,
          expectedBpm: expectedBPM,
          bpmError: bpmError !== null ? Math.round(bpmError * 10) / 10 : null,
          beatCount: beatEvents.length,
        };

        // Write detailed results to file (saves tokens)
        ensureTestResultsDir();
        const timestamp = Date.now();
        const detailsFilename = `${patternId}-${timestamp}.json`;
        const detailsPath = join(TEST_RESULTS_DIR, detailsFilename);

        const fullResults = {
          pattern: patternId,
          timestamp: new Date(timestamp).toISOString(),
          durationMs: duration,
          timingOffsetMs,
          metrics,
          musicMetrics,
          groundTruth: result.groundTruth,
          detections,
          audioSamples,
          musicStates,
          beatEvents,
        };

        writeFileSync(detailsPath, JSON.stringify(fullResults, null, 2));

        // Also write to latest.json for quick access
        writeFileSync(join(TEST_RESULTS_DIR, 'latest.json'), JSON.stringify(fullResults, null, 2));

        // Return compact summary only (saves tokens)
        const summary = {
          pattern: patternId,
          durationMs: duration,
          transient: {
            f1: metrics.f1Score,
            precision: metrics.precision,
            recall: metrics.recall,
            tp: truePositives,
            fp: falsePositives,
            fn: falseNegatives,
          },
          music: {
            active: activationTime !== null,
            activationMs: musicMetrics.activationMs,
            bpm: musicMetrics.avgBpm,
            bpmError: musicMetrics.bpmError,
            confidence: musicMetrics.avgConfidence,
            beats: musicMetrics.beatCount,
          },
          timing: {
            avgErrorMs: metrics.avgTimingErrorMs,
            latencyMs: metrics.audioLatencyMs,
          },
          counts: {
            expected: expectedHits.length,
            detected: detections.length,
            low: lowCount,
            high: highCount,
          },
          detailsFile: detailsFilename,
        };

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify(summary),
            },
          ],
        };
        } finally {
          // Unlock hardware gain if it was locked
          if (gain !== undefined && serial.getState().connected) {
            try {
              await serial.sendCommand('set hwgainlock 255');
            } catch (err) {
              // Log error but don't throw - we still want to disconnect
              console.error('Failed to unlock hardware gain:', err);
            }
          }

          // Always disconnect to release serial port for other tools (e.g., Arduino IDE)
          await serial.disconnect();
        }
      }

      default:
        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({ error: `Unknown tool: ${name}` }, null, 2),
            },
          ],
        };
    }
  } catch (error) {
    return {
      content: [
        {
          type: 'text',
          text: JSON.stringify({
            error: error instanceof Error ? error.message : String(error),
          }, null, 2),
        },
      ],
    };
  }
});

// Start the server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error('Blinky Serial MCP server running');
}

main().catch(console.error);
