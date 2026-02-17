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
import type { AudioSample, TransientEvent, MusicModeState, BeatEvent, LedTelemetry } from './types.js';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { writeFileSync, mkdirSync, existsSync, statSync } from 'fs';

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
let lastLedSample: LedTelemetry | null = null;
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
      type: beat.type as 'quarter',
    });
  }
});

serial.on('led', (led: LedTelemetry) => {
  lastLedSample = led;
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
        description: 'Monitor audio for a specified duration and return statistics including transient count, level min/max/avg, and music mode status',
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
      {
        name: 'get_music_status',
        description: 'Get current music mode status (BPM, confidence, phase, beat counts). Requires streaming to be active.',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'monitor_music',
        description: 'Monitor music mode for BPM tracking assessment over a duration. Returns BPM accuracy, stability, confidence trends, and beat detection metrics.',
        inputSchema: {
          type: 'object',
          properties: {
            duration_ms: {
              type: 'number',
              description: 'Duration to monitor in milliseconds (default: 5000)',
            },
            expected_bpm: {
              type: 'number',
              description: 'Expected BPM for accuracy calculation (optional)',
            },
          },
        },
      },
      {
        name: 'get_hypotheses',
        description: 'Get multi-hypothesis tracker state including all 4 hypothesis slots (BPM, phase, confidence, strength, beatCount). Useful for validating tempo tracking behavior and hypothesis promotion.',
        inputSchema: {
          type: 'object',
          properties: {},
        },
      },
      {
        name: 'render_preview',
        description: 'Render a visual preview of an LED effect to animated GIFs. Runs the actual firmware generator code in simulation. Outputs both low-res (for AI analysis) and high-res (for human viewing) GIFs, plus params.json and metrics.json for agent-assisted optimization.',
        inputSchema: {
          type: 'object',
          properties: {
            generator: {
              type: 'string',
              description: 'Generator to use: fire, water, lightning (default: fire)',
              enum: ['fire', 'water', 'lightning'],
            },
            effect: {
              type: 'string',
              description: 'Effect to apply: none, hue (default: none)',
              enum: ['none', 'hue'],
            },
            pattern: {
              type: 'string',
              description: 'Audio pattern: steady-120bpm, steady-90bpm, steady-140bpm, silence, burst, complex (default: steady-120bpm)',
            },
            device: {
              type: 'string',
              description: 'Device config: bucket (16x8), tube (4x15), hat (89 string) (default: bucket)',
              enum: ['bucket', 'tube', 'hat'],
            },
            duration_ms: {
              type: 'number',
              description: 'Duration in milliseconds (default: 3000)',
            },
            fps: {
              type: 'number',
              description: 'Frames per second (default: 30)',
            },
            hue_shift: {
              type: 'number',
              description: 'Hue shift for hue effect (0.0-1.0, default: 0.0)',
            },
            params: {
              type: 'string',
              description: 'Parameter overrides for generator tuning (e.g., "baseSpawnChance=0.15,gravity=-12,burstSparks=10")',
            },
            output_dir: {
              type: 'string',
              description: 'Output directory (default: previews in blinky-simulator)',
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
        lastLedSample = null;
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
                music: lastMusicState,
                led: lastLedSample,
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
        // Switch to audio visualization generator for testing
        await serial.sendCommand('gen audio');

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

        // Switch to audio visualization generator for monitoring
        await serial.sendCommand('gen audio');

        const state = serial.getState();
        if (!state.streaming) {
          await serial.startStream();
        }

        // Initialize statistics collectors
        let transientCount = 0;
        let levelSum = 0;
        let levelMin = Infinity;
        let levelMax = -Infinity;
        let rawSum = 0;
        let rawMin = Infinity;
        let rawMax = -Infinity;
        let musicSampleCount = 0;
        let musicActiveCount = 0;
        let bpmSum = 0;
        let confSum = 0;
        let beatCount = 0;
        let samplesDuringMonitor = 0;

        const startCount = audioSampleCount;

        // Set up temporary listeners for statistics
        const onAudio = (sample: AudioSample) => {
          samplesDuringMonitor++;
          if (sample.t > 0) transientCount++;
          levelSum += sample.l;
          levelMin = Math.min(levelMin, sample.l);
          levelMax = Math.max(levelMax, sample.l);
          rawSum += sample.raw;
          rawMin = Math.min(rawMin, sample.raw);
          rawMax = Math.max(rawMax, sample.raw);
        };

        const onMusic = (musicState: MusicModeState) => {
          musicSampleCount++;
          if (musicState.a === 1) {
            musicActiveCount++;
            bpmSum += musicState.bpm;
            confSum += musicState.conf;
          }
          if (musicState.q === 1) beatCount++;
        };

        serial.on('audio', onAudio);
        serial.on('music', onMusic);

        await new Promise(resolve => setTimeout(resolve, durationMs));

        serial.off('audio', onAudio);
        serial.off('music', onMusic);

        const endCount = audioSampleCount;
        const samplesReceived = endCount - startCount;
        const sampleRate = samplesReceived / (durationMs / 1000);

        // Build response with statistics
        const response: Record<string, unknown> = {
          durationMs,
          samplesReceived,
          sampleRate: sampleRate.toFixed(1) + ' Hz',
          statistics: {
            transientCount,
            transientRate: (transientCount / (durationMs / 1000)).toFixed(2) + ' /sec',
            level: {
              min: levelMin === Infinity ? null : parseFloat(levelMin.toFixed(3)),
              max: levelMax === -Infinity ? null : parseFloat(levelMax.toFixed(3)),
              avg: samplesDuringMonitor > 0 ? parseFloat((levelSum / samplesDuringMonitor).toFixed(3)) : null,
            },
            raw: {
              min: rawMin === Infinity ? null : parseFloat(rawMin.toFixed(3)),
              max: rawMax === -Infinity ? null : parseFloat(rawMax.toFixed(3)),
              avg: samplesDuringMonitor > 0 ? parseFloat((rawSum / samplesDuringMonitor).toFixed(3)) : null,
            },
          },
          currentSample: lastAudioSample,
        };

        // Add music mode statistics if we received any music data
        if (musicSampleCount > 0) {
          response.musicMode = {
            samplesReceived: musicSampleCount,
            activePercent: parseFloat(((musicActiveCount / musicSampleCount) * 100).toFixed(1)),
            avgBpm: musicActiveCount > 0 ? parseFloat((bpmSum / musicActiveCount).toFixed(1)) : null,
            avgConfidence: musicActiveCount > 0 ? parseFloat((confSum / musicActiveCount).toFixed(2)) : null,
            beatCount,
            beatRate: (beatCount / (durationMs / 1000)).toFixed(2) + ' /sec',
          };
          response.currentMusic = lastMusicState;
        }

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify(response, null, 2),
            },
          ],
        };
      }

      case 'get_music_status': {
        const state = serial.getState();
        if (!state.streaming) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({
                  error: 'Streaming not active. Call stream_start first.',
                  hint: 'Use stream_start to begin receiving music mode data',
                }, null, 2),
              },
            ],
          };
        }

        if (!lastMusicState) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({
                  error: 'No music mode data received yet',
                  hint: 'Wait a moment for data to arrive, or check if device supports music mode',
                }, null, 2),
              },
            ],
          };
        }

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                active: lastMusicState.a === 1,
                bpm: lastMusicState.bpm,
                phase: lastMusicState.ph,
                rhythmStrength: lastMusicState.str,
                confidence: lastMusicState.conf,
                beatCount: lastMusicState.bc,
                beat: lastMusicState.q === 1,
                energy: lastMusicState.e,
                pulse: lastMusicState.p,
                debug: {
                  periodicityStrength: lastMusicState.ps,
                },
              }, null, 2),
            },
          ],
        };
      }

      case 'monitor_music': {
        const durationMs = (args as { duration_ms?: number; expected_bpm?: number }).duration_ms || 5000;
        const expectedBpm = (args as { duration_ms?: number; expected_bpm?: number }).expected_bpm;

        const state = serial.getState();
        if (!state.streaming) {
          await serial.startStream();
        }

        // Enable debug mode for detailed music tracking
        await serial.sendCommand('stream debug');

        // Collect music mode samples over time
        interface TimestampedMusic extends MusicModeState {
          timestampMs: number;
        }
        const samples: TimestampedMusic[] = [];
        const beats: { type: string; timestampMs: number; bpm: number }[] = [];
        const startTime = Date.now();

        const onMusic = (musicState: MusicModeState) => {
          const timestampMs = Date.now() - startTime;
          samples.push({ ...musicState, timestampMs });
          if (musicState.q === 1) beats.push({ type: 'quarter', timestampMs, bpm: musicState.bpm });
        };

        serial.on('music', onMusic);
        await new Promise(resolve => setTimeout(resolve, durationMs));
        serial.off('music', onMusic);

        // Calculate metrics
        const activeStates = samples.filter(s => s.a === 1);
        const firstActive = activeStates.length > 0 ? activeStates[0] : null;
        const bpmValues = activeStates.map(s => s.bpm);

        // BPM statistics
        const avgBpm = bpmValues.length > 0
          ? bpmValues.reduce((a, b) => a + b, 0) / bpmValues.length : 0;
        const bpmVariance = bpmValues.length > 1
          ? bpmValues.reduce((sum, b) => sum + Math.pow(b - avgBpm, 2), 0) / bpmValues.length : 0;
        const bpmStdDev = Math.sqrt(bpmVariance);

        // Confidence statistics
        const confValues = activeStates.map(s => s.conf);
        const avgConf = confValues.length > 0
          ? confValues.reduce((a, b) => a + b, 0) / confValues.length : 0;
        const finalConf = samples.length > 0 ? samples[samples.length - 1].conf : 0;

        // Stability assessment
        let stability: string;
        if (bpmStdDev < 0.5) stability = 'excellent';
        else if (bpmStdDev < 2) stability = 'good';
        else if (bpmStdDev < 5) stability = 'fair';
        else stability = 'poor';

        // BPM accuracy if expected provided
        const bpmError = expectedBpm && avgBpm > 0
          ? Math.abs(avgBpm - expectedBpm) / expectedBpm * 100 : null;

        // Get debug fields from last sample
        const lastSample = samples.length > 0 ? samples[samples.length - 1] : null;

        const response: Record<string, unknown> = {
          durationMs,
          totalSamples: samples.length,
          activation: {
            activeSamples: activeStates.length,
            activePercent: samples.length > 0 ? parseFloat(((activeStates.length / samples.length) * 100).toFixed(1)) : 0,
            activationMs: firstActive?.timestampMs || null,
          },
          bpm: {
            current: lastSample?.bpm || null,
            average: avgBpm > 0 ? parseFloat(avgBpm.toFixed(1)) : null,
            stdDev: parseFloat(bpmStdDev.toFixed(2)),
            stability,
            expected: expectedBpm || null,
            errorPercent: bpmError !== null ? parseFloat(bpmError.toFixed(1)) : null,
          },
          confidence: {
            current: finalConf,
            average: parseFloat(avgConf.toFixed(2)),
          },
          beats: {
            total: beats.length,
            quarterNotes: beats.filter(b => b.type === 'quarter').length,
            rate: parseFloat((beats.filter(b => b.type === 'quarter').length / (durationMs / 1000)).toFixed(2)),
          },
        };

        // Add debug info if available
        if (lastSample && (lastSample.sb !== undefined || lastSample.mb !== undefined)) {
          response.debug = {
            stableBeats: lastSample.sb,
            missedBeats: lastSample.mb,
            peakEnergy: lastSample.pe !== undefined ? parseFloat(lastSample.pe.toFixed(4)) : null,
            errorIntegral: lastSample.ei !== undefined ? parseFloat(lastSample.ei.toFixed(3)) : null,
          };
        }

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify(response, null, 2),
            },
          ],
        };
      }

      case 'get_hypotheses': {
        if (!serial.getState().connected) {
          throw new Error('Not connected to a device');
        }

        // Send "json hypotheses" command and parse response
        const response = await serial.sendCommand('json hypotheses');

        // Parse JSON response
        let parsed;
        try {
          parsed = JSON.parse(response);
        } catch (e) {
          throw new Error(`Failed to parse hypothesis data: ${response}`);
        }

        // Format for MCP response
        const primaryIdx = parsed.primaryIndex || 0;
        const formatted = {
          hypotheses: parsed.hypotheses || [],
          primaryIndex: primaryIdx,
          summary: {
            activeCount: parsed.hypotheses.filter((h: { active: boolean }) => h.active).length,
            primaryBpm: parsed.hypotheses[primaryIdx]?.bpm || null,
            primaryConfidence: parsed.hypotheses[primaryIdx]?.confidence || null,
            primaryBeats: parsed.hypotheses[primaryIdx]?.beatCount || null,
          },
        };

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify(formatted, null, 2),
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

          // Switch to audio visualization generator for testing
          // This provides visual feedback of audio data during tests
          await serial.sendCommand('gen audio');

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
          bpm?: number;
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
        // Match detections to expected hits within a BPM-aware timing tolerance
        // Tighter tolerance at faster tempos prevents matching the wrong beat
        // When BPM is unknown, use the maximum tolerance (200ms) as a safe default
        const TIMING_TOLERANCE_MS = groundTruth.bpm
          ? Math.min(200, Math.round(60000 / groundTruth.bpm * 0.25))
          : 200;
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
        let latencyStdDev: number | null = null;
        let latencyWarning: string | null = null;
        if (offsets.length > 0) {
          offsets.sort((a, b) => a - b);
          audioLatencyMs = offsets[Math.floor(offsets.length / 2)];

          // Check latency estimate quality via standard deviation
          // High variance suggests mixed true/false positive detections
          if (offsets.length >= 3) {
            const mean = offsets.reduce((s, v) => s + v, 0) / offsets.length;
            const variance = offsets.reduce((s, v) => s + (v - mean) ** 2, 0) / offsets.length;
            latencyStdDev = Math.round(Math.sqrt(variance));
            if (latencyStdDev > 100) {
              latencyWarning = `High offset variance (stddev=${latencyStdDev}ms) — latency estimate may be unreliable due to false positives`;
            }
          }
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
          latencyStdDev,
          latencyWarning,
          timingToleranceMs: TIMING_TOLERANCE_MS,
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

      case 'render_preview': {
        const {
          generator = 'fire',
          effect = 'none',
          pattern = 'steady-120bpm',
          device = 'bucket',
          duration_ms = 3000,
          fps = 30,
          hue_shift = 0.0,
          params = '',
          output_dir,
        } = args as {
          generator?: string;
          effect?: string;
          pattern?: string;
          device?: string;
          duration_ms?: number;
          fps?: number;
          hue_shift?: number;
          params?: string;
          output_dir?: string;
        };

        // Path to C++ simulator executable (runs actual firmware code)
        const isWindows = process.platform === 'win32';
        const SIMULATOR_EXE = isWindows ? 'blinky-simulator.exe' : 'blinky-simulator';
        const SIMULATOR_DIR = join(__dirname, '..', '..', 'blinky-simulator');
        const SIMULATOR_PATH = join(SIMULATOR_DIR, 'build', SIMULATOR_EXE);

        // Determine output directory (relative to simulator dir)
        const outputBaseDir = output_dir || join(SIMULATOR_DIR, 'previews');

        // Check if simulator is built
        if (!existsSync(SIMULATOR_PATH)) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({
                  error: 'Simulator not built',
                  hint: isWindows
                    ? 'Build the simulator first: cd blinky-simulator && build_vs.cmd'
                    : 'Build the simulator first: cd blinky-simulator && ./build.sh',
                  simulatorPath: SIMULATOR_PATH,
                  note: 'The simulator compiles actual firmware C++ code. Requires a C++ compiler (Visual Studio, g++, or clang++).',
                }, null, 2),
              },
            ],
          };
        }

        // Build CLI arguments
        const cliArgs = [
          '-g', generator,
          '-e', effect,
          '-p', pattern,
          '-d', device,
          '-t', duration_ms.toString(),
          '-f', fps.toString(),
          '-o', outputBaseDir,
        ];

        if (effect === 'hue' && hue_shift > 0) {
          cliArgs.push('--hue', hue_shift.toString());
        }

        if (params) {
          cliArgs.push('--params', params);
        }

        // Run C++ simulator (runs actual firmware code)
        const result = await new Promise<{ success: boolean; output?: string; error?: string }>((resolve) => {
          const child = spawn(SIMULATOR_PATH, cliArgs, {
            stdio: ['ignore', 'pipe', 'pipe'],
            cwd: SIMULATOR_DIR,  // Run from simulator directory
          });

          let stdout = '';
          let stderr = '';

          child.stdout.on('data', (data) => {
            stdout += data.toString();
          });

          child.stderr.on('data', (data) => {
            stderr += data.toString();
          });

          child.on('close', (code) => {
            if (code === 0) {
              resolve({ success: true, output: stdout.trim() });
            } else {
              resolve({ success: false, error: stderr || stdout || `Process exited with code ${code}` });
            }
          });

          child.on('error', (err) => {
            resolve({ success: false, error: err.message });
          });
        });

        if (!result.success) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({
                  error: 'Simulation failed',
                  details: result.error,
                }, null, 2),
              },
            ],
          };
        }

        // Parse simulator output to extract file paths and metrics
        // Output format includes "Created:" followed by file paths, then "Metrics summary:"
        const output = result.output || '';
        const lines = output.split('\n');

        // Extract file paths from output
        let lowResPath = '';
        let highResPath = '';
        let paramsJsonPath = '';
        let metricsJsonPath = '';

        for (const line of lines) {
          const trimmed = line.trim();
          if (trimmed.includes('/low-res/') && trimmed.endsWith('.gif')) {
            lowResPath = trimmed;
          } else if (trimmed.includes('/high-res/') && trimmed.endsWith('.gif')) {
            highResPath = trimmed;
          } else if (trimmed.endsWith('-params.json')) {
            paramsJsonPath = trimmed;
          } else if (trimmed.endsWith('-metrics.json')) {
            metricsJsonPath = trimmed;
          }
        }

        // Extract metrics from output
        const metricsMatch = output.match(/Metrics summary:\s*\n([\s\S]*?)$/);
        let metrics = {};
        if (metricsMatch) {
          const metricsLines = metricsMatch[1].split('\n');
          for (const line of metricsLines) {
            const match = line.match(/^\s*(\w+):\s*(.+)$/);
            if (match) {
              const key = match[1].toLowerCase();
              const valueStr = match[2];
              // Parse key=value pairs from line
              const values: Record<string, number | string> = {};
              const pairs = valueStr.split(',');
              for (const pair of pairs) {
                const [k, v] = pair.trim().split('=');
                if (k && v) {
                  values[k.trim()] = parseFloat(v) || v;
                }
              }
              if (Object.keys(values).length > 0) {
                metrics = { ...metrics, [key]: values };
              } else {
                // Single value (like "Lit pixels: 59%")
                metrics = { ...metrics, [key]: valueStr.trim() };
              }
            }
          }
        }

        // Get file sizes (handle race condition if file deleted between check and stat)
        let lowResSize = 0;
        let highResSize = 0;
        try {
          if (lowResPath && existsSync(lowResPath)) {
            lowResSize = statSync(lowResPath).size;
          }
        } catch (e: unknown) {
          if ((e as NodeJS.ErrnoException).code !== 'ENOENT') throw e;
        }
        try {
          if (highResPath && existsSync(highResPath)) {
            highResSize = statSync(highResPath).size;
          }
        } catch (e: unknown) {
          if ((e as NodeJS.ErrnoException).code !== 'ENOENT') throw e;
        }

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                success: true,
                outputs: {
                  lowResGif: lowResPath,
                  highResGif: highResPath,
                  paramsJson: paramsJsonPath,
                  metricsJson: metricsJsonPath,
                },
                sizes: {
                  lowResBytes: lowResSize,
                  highResBytes: highResSize,
                },
                metrics,
                config: {
                  generator,
                  effect,
                  pattern,
                  device,
                  durationMs: duration_ms,
                  fps,
                  hueShift: hue_shift,
                  params: params || null,
                },
                rawOutput: output,
              }, null, 2),
            },
          ],
        };
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
