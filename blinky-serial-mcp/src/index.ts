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
import type { AudioSample, TransientEvent } from './types.js';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to test player CLI
const TEST_PLAYER_PATH = join(__dirname, '..', '..', 'blinky-test-player', 'dist', 'index.js');

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
  lowStrength: number;  // Renamed from lowEnergy (it's onset strength, not raw energy)
  highStrength: number; // Renamed from highEnergy (it's onset strength, not raw energy)
}
let audioSampleBuffer: TimestampedSample[] = [];

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
      lowStrength: sample.los,
      highStrength: sample.his,
    });

    // Record transients
    if (sample.lo === 1) {
      transientBuffer.push({
        timestampMs,
        type: 'low',
        strength: sample.los,
      });
    }
    if (sample.hi === 1) {
      transientBuffer.push({
        timestampMs,
        type: 'high',
        strength: sample.his,
      });
    }
  }
});

serial.on('error', (err: Error) => {
  console.error('Serial error:', err.message);
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
        description: 'Run a complete test: play a pattern and record detections simultaneously',
        inputSchema: {
          type: 'object',
          properties: {
            pattern: {
              type: 'string',
              description: 'Pattern ID to play (e.g., "simple-beat", "simultaneous")',
            },
          },
          required: ['pattern'],
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
          // Basic patterns (no background audio)
          { id: 'simple-beat', name: 'Alternating Low/High', durationMs: 16000, description: 'Low on 1&3, high on 2&4 (120 BPM, 8 bars)', hasBackground: false },
          { id: 'low-barrage', name: 'Low Band Barrage', durationMs: 8000, description: 'Rapid bass transients at varying intervals', hasBackground: false },
          { id: 'high-burst', name: 'High Band Burst', durationMs: 6000, description: 'Rapid high-frequency transients', hasBackground: false },
          { id: 'mixed-pattern', name: 'Mixed Low/High', durationMs: 10000, description: 'Interleaved low and high with varying dynamics', hasBackground: false },
          { id: 'timing-test', name: 'Timing Precision Test', durationMs: 10000, description: 'Transients at 100-250ms intervals', hasBackground: false },
          { id: 'simultaneous', name: 'Simultaneous Hits', durationMs: 8000, description: 'Low and high at exactly the same time', hasBackground: false },
          // Realistic patterns with background audio
          { id: 'realistic-track', name: 'Realistic Electronic Track', durationMs: 16000, description: 'Kick/hi-hat with sub-bass drone, pad, noise floor (128 BPM)', hasBackground: true },
          { id: 'heavy-background', name: 'Heavy Background', durationMs: 10000, description: 'Transients over loud continuous audio (140 BPM)', hasBackground: true },
          { id: 'baseline-only', name: 'Baseline Only (No Transients)', durationMs: 8000, description: 'Only background audio - any detections are false positives', hasBackground: true },
          { id: 'quiet-section', name: 'Quiet Section', durationMs: 12000, description: 'Low-level background with subtle transients (100 BPM)', hasBackground: true },
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
        const patternId = (args as { pattern: string }).pattern;

        // Ensure connected
        const state = serial.getState();
        if (!state.connected) {
          return {
            content: [
              {
                type: 'text',
                text: JSON.stringify({ error: 'Not connected to device' }, null, 2),
              },
            ],
          };
        }

        // Clear buffers and start recording
        transientBuffer = [];
        audioSampleBuffer = [];

        // Ensure streaming is on
        if (!state.streaming) {
          await serial.startStream();
        }

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

        const recordStartTime = testStartTime;
        testStartTime = null;
        transientBuffer = [];
        audioSampleBuffer = [];

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
        }

        // Calculate stats
        const lowCount = detections.filter(d => d.type === 'low').length;
        const highCount = detections.filter(d => d.type === 'high').length;
        const duration = groundTruth.durationMs || rawDuration;

        // Calculate F1/precision/recall metrics
        // Match detections to expected hits within a timing tolerance
        const TIMING_TOLERANCE_MS = 150; // Allow 150ms timing variance
        const expectedHits = groundTruth.hits || [];

        // Track which expected hits were matched
        const matchedExpected = new Set<number>();
        const matchedDetections = new Set<number>();

        // Match each detection to nearest expected hit (if within tolerance)
        detections.forEach((detection, dIdx) => {
          let bestMatchIdx = -1;
          let bestMatchDist = Infinity;

          expectedHits.forEach((expected, eIdx) => {
            if (matchedExpected.has(eIdx)) return; // Already matched
            if (expected.type !== detection.type) return; // Wrong type

            const dist = Math.abs(detection.timestampMs - expected.timeMs);
            if (dist < bestMatchDist && dist <= TIMING_TOLERANCE_MS) {
              bestMatchDist = dist;
              bestMatchIdx = eIdx;
            }
          });

          if (bestMatchIdx >= 0) {
            matchedExpected.add(bestMatchIdx);
            matchedDetections.add(dIdx);
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

        // Calculate average timing error for matched detections
        let totalTimingError = 0;
        let matchCount = 0;
        detections.forEach((detection, dIdx) => {
          if (!matchedDetections.has(dIdx)) return;
          // Find the expected hit this was matched to
          expectedHits.forEach((expected, eIdx) => {
            if (matchedExpected.has(eIdx) && expected.type === detection.type) {
              const dist = Math.abs(detection.timestampMs - expected.timeMs);
              if (dist <= TIMING_TOLERANCE_MS) {
                totalTimingError += dist;
                matchCount++;
              }
            }
          });
        });
        const avgTimingErrorMs = matchCount > 0 ? totalTimingError / matchCount : null;

        const metrics = {
          f1Score: Math.round(f1Score * 1000) / 1000,
          precision: Math.round(precision * 1000) / 1000,
          recall: Math.round(recall * 1000) / 1000,
          truePositives,
          falsePositives,
          falseNegatives,
          expectedTotal: expectedHits.length,
          avgTimingErrorMs: avgTimingErrorMs !== null ? Math.round(avgTimingErrorMs) : null,
        };

        return {
          content: [
            {
              type: 'text',
              text: JSON.stringify({
                success: true,
                pattern: patternId,
                durationMs: duration,
                timingOffsetMs,
                metrics,
                groundTruth: result.groundTruth,
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
