#!/usr/bin/env node
/**
 * Blinky Serial MCP Server — Thin HTTP client wrapper.
 *
 * All device interaction goes through blinky-server (localhost:8420).
 * This MCP server translates tool calls to REST API requests.
 */

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import { spawn } from 'child_process';
import { existsSync, readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

import { del, get, monitorWs, post, put, resolveDeviceId } from './http-client.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Default track directory for validation suites
const DEFAULT_TRACK_DIR = join(__dirname, '..', '..', 'blinky-test-player', 'music', 'edm');

// JSON response helper
function ok(data: unknown): { content: Array<{ type: 'text'; text: string }> } {
  return { content: [{ type: 'text', text: JSON.stringify(data, null, 2) }] };
}

// Create MCP server
const server = new Server(
  { name: 'blinky-serial-mcp', version: '2.0.0' },
  { capabilities: { tools: {} } },
);

// ── Tool Definitions ──

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: 'list_ports',
      description: 'List available devices managed by blinky-server',
      inputSchema: { type: 'object', properties: {} },
    },
    {
      name: 'connect',
      description: 'Connect to a blinky device (server manages connections automatically)',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port path or device ID' },
        },
        required: ['port'],
      },
    },
    {
      name: 'disconnect',
      description: 'Disconnect from a device (server manages connections automatically)',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'status',
      description: 'Get device status. Shows all devices if no port specified.',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional — omit for all)' },
        },
      },
    },
    {
      name: 'send_command',
      description: 'Send a raw command to the device and get the response',
      inputSchema: {
        type: 'object',
        properties: {
          command: { type: 'string', description: 'Command to send' },
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
        required: ['command'],
      },
    },
    {
      name: 'stream_start',
      description: 'Start audio streaming from the device',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'stream_stop',
      description: 'Stop audio streaming',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'get_audio',
      description: 'Get the most recent audio sample data (requires streaming)',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'get_settings',
      description: 'Get all device settings as JSON',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'set_setting',
      description: 'Set a device setting value',
      inputSchema: {
        type: 'object',
        properties: {
          name: { type: 'string', description: 'Setting name' },
          value: { type: 'number', description: 'New value' },
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
        required: ['name', 'value'],
      },
    },
    {
      name: 'save_settings',
      description: 'Save current settings to device flash memory',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'reset_defaults',
      description: 'Reset all settings to factory defaults',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'monitor_audio',
      description: 'Monitor audio for a specified duration. Returns transient count, level stats, and rhythm tracking status.',
      inputSchema: {
        type: 'object',
        properties: {
          duration_ms: { type: 'number', description: 'Duration in milliseconds (default: 1000)' },
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'monitor_transients',
      description: 'Monitor transient detections for a duration. Returns count, rate, and strength distribution.',
      inputSchema: {
        type: 'object',
        properties: {
          duration_ms: { type: 'number', description: 'Duration in milliseconds (default: 3000)' },
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'get_music_status',
      description: 'Get current rhythm tracking status (confidence, phase). Requires streaming.',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID (optional)' },
        },
      },
    },
    {
      name: 'list_patterns',
      description: 'List available test tracks',
      inputSchema: {
        type: 'object',
        properties: {
          directory: { type: 'string', description: 'Track directory (default: blinky-test-player/music/edm/)' },
        },
      },
    },
    {
      name: 'run_test',
      description: 'Run a validation test: play audio tracks and score onset + PLP metrics against ground truth. Returns job_id — poll with check_test_result.',
      inputSchema: {
        type: 'object',
        properties: {
          port: { type: 'string', description: 'Serial port or device ID' },
          track_dir: { type: 'string', description: 'Directory with audio + .beats.json (default: music/edm/)' },
          tracks: { type: 'array', items: { type: 'string' }, description: 'Specific track names (default: all)' },
          duration_ms: { type: 'number', description: 'Playback duration per track (default: 35000)' },
          runs: { type: 'number', description: 'Runs per track (default: 1)' },
          commands: { type: 'array', items: { type: 'string' }, description: 'Setup commands to send before test' },
        },
        required: ['port'],
      },
    },
    {
      name: 'run_validation_suite',
      description: 'Launch full validation suite across multiple devices. Returns job_id — poll with check_test_result.',
      inputSchema: {
        type: 'object',
        properties: {
          ports: { type: 'array', items: { type: 'string' }, description: 'Serial ports or device IDs' },
          track_dir: { type: 'string', description: 'Track directory (default: music/edm/)' },
          tracks: { type: 'array', items: { type: 'string' }, description: 'Specific tracks (default: all)' },
          runs: { type: 'number', description: 'Runs per track (default: 3)' },
          duration_ms: { type: 'number', description: 'Duration per track in ms' },
          commands: { type: 'array', items: { type: 'string' }, description: 'Setup commands' },
          per_device_commands: { type: 'object', description: 'Per-device setup commands', additionalProperties: { type: 'array', items: { type: 'string' } } },
        },
        required: ['ports'],
      },
    },
    {
      name: 'check_test_result',
      description: 'Poll for test job status and results. Use the job_id returned by run_test or run_validation_suite.',
      inputSchema: {
        type: 'object',
        properties: {
          job_id: { type: 'string', description: 'Job ID from a test submission' },
        },
        required: ['job_id'],
      },
    },
    {
      name: 'render_preview',
      description: 'Render a visual preview of an LED effect to animated GIFs. Runs actual firmware generator code in simulation.',
      inputSchema: {
        type: 'object',
        properties: {
          generator: { type: 'string', description: 'Generator: fire, water, lightning', enum: ['fire', 'water', 'lightning'] },
          effect: { type: 'string', description: 'Effect: none, hue', enum: ['none', 'hue'] },
          pattern: { type: 'string', description: 'Audio pattern (default: steady-120bpm)' },
          device: { type: 'string', description: 'Device config: bucket, tube, hat', enum: ['bucket', 'tube', 'hat'] },
          duration_ms: { type: 'number', description: 'Duration in ms (default: 3000)' },
          fps: { type: 'number', description: 'Frames per second (default: 30)' },
          hue_shift: { type: 'number', description: 'Hue shift for hue effect (0.0-1.0)' },
          params: { type: 'string', description: 'Parameter overrides (e.g., "baseSpawnChance=0.15,gravity=-12")' },
          output_dir: { type: 'string', description: 'Output directory' },
        },
      },
    },
  ],
}));

// ── Tool Handlers ──

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      // ── Device Management ──

      case 'list_ports': {
        const devices = await get('/devices');
        return ok(devices);
      }

      case 'connect': {
        const { port } = args as { port: string };
        // Server manages connections automatically. Just verify device exists.
        try {
          const id = await resolveDeviceId(port);
          const device = await get(`/devices/${id}`);
          return ok({ status: 'connected', device });
        } catch {
          return ok({ status: 'not_found', message: `Device not found: ${port}. Server manages connections automatically — device may not be plugged in.` });
        }
      }

      case 'disconnect': {
        const { port } = args as { port?: string };
        if (port) {
          try {
            const id = await resolveDeviceId(port);
            await post(`/devices/${id}/release`);
            return ok({ status: 'released', device_id: id });
          } catch {
            return ok({ status: 'ok', message: 'Device not found or already disconnected' });
          }
        }
        return ok({ status: 'ok', message: 'Server manages connections automatically' });
      }

      case 'status': {
        const { port } = args as { port?: string };
        if (port) {
          const id = await resolveDeviceId(port);
          return ok(await get(`/devices/${id}`));
        }
        return ok(await get('/devices'));
      }

      // ── Commands ──

      case 'send_command': {
        const { command, port } = args as { command: string; port?: string };
        const id = await resolveDeviceId(port);
        const result = await post(`/devices/${id}/command`, { command });
        return ok(result);
      }

      // ── Streaming ──

      case 'stream_start': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        await post(`/devices/${id}/stream/fast`);
        return ok({ status: 'streaming', device_id: id });
      }

      case 'stream_stop': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        await post(`/devices/${id}/stream/off`);
        return ok({ status: 'stopped', device_id: id });
      }

      case 'get_audio': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        // Collect one audio frame from WebSocket
        let sample: unknown = null;
        await monitorWs(id, 500, (msg) => {
          if (msg.type === 'audio' && !sample) {
            sample = msg.data;
          }
        });
        if (!sample) {
          return ok({ error: 'No audio data received. Is streaming active? Use stream_start first.' });
        }
        return ok(sample);
      }

      // ── Settings ──

      case 'get_settings': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        return ok(await get(`/devices/${id}/settings`));
      }

      case 'set_setting': {
        const { name: settingName, value, port } = args as { name: string; value: number; port?: string };
        const id = await resolveDeviceId(port);
        return ok(await put(`/devices/${id}/settings/${settingName}`, { value }));
      }

      case 'save_settings': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        return ok(await post(`/devices/${id}/settings/save`));
      }

      case 'reset_defaults': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        return ok(await post(`/devices/${id}/settings/defaults`));
      }

      // ── Monitoring ──

      case 'monitor_audio': {
        const { duration_ms = 1000, port } = args as { duration_ms?: number; port?: string };
        const id = await resolveDeviceId(port);
        // Ensure streaming is active
        await post(`/devices/${id}/stream/fast`);
        const samples: Array<Record<string, unknown>> = [];
        let transientCount = 0;
        await monitorWs(id, duration_ms, (msg) => {
          if (msg.type === 'audio') samples.push(msg.data as Record<string, unknown>);
          if (msg.type === 'transient') transientCount++;
        });
        // Compute stats
        const levels = samples
          .map((s) => (s as Record<string, Record<string, number>>)?.a?.l ?? 0)
          .filter((l): l is number => typeof l === 'number');
        const stats = levels.length > 0
          ? { min: Math.min(...levels), max: Math.max(...levels), avg: +(levels.reduce((a, b) => a + b, 0) / levels.length).toFixed(3) }
          : { min: 0, max: 0, avg: 0 };
        return ok({ duration_ms, samples: samples.length, transientCount, level: stats });
      }

      case 'monitor_transients': {
        const { duration_ms = 3000, port } = args as { duration_ms?: number; port?: string };
        const id = await resolveDeviceId(port);
        await post(`/devices/${id}/stream/fast`);
        // Also enable transient debug channel
        await post(`/devices/${id}/command`, { command: 'debug transient on' });
        const transients: Array<{ strength: number; timestampMs: number }> = [];
        await monitorWs(id, duration_ms, (msg) => {
          if (msg.type === 'transient') {
            const d = msg.data as Record<string, unknown>;
            transients.push({
              strength: (d.strength as number) ?? 0,
              timestampMs: (d.timestampMs as number) ?? 0,
            });
          }
        });
        await post(`/devices/${id}/command`, { command: 'debug transient off' });
        const strengths = transients.map((t) => t.strength);
        return ok({
          duration_ms,
          count: transients.length,
          rate: +((transients.length / duration_ms) * 1000).toFixed(1),
          strengths: strengths.length > 0
            ? { min: +Math.min(...strengths).toFixed(3), max: +Math.max(...strengths).toFixed(3), avg: +(strengths.reduce((a, b) => a + b, 0) / strengths.length).toFixed(3) }
            : null,
        });
      }

      case 'get_music_status': {
        const { port } = args as { port?: string };
        const id = await resolveDeviceId(port);
        // Get latest music state from one WebSocket frame
        let musicState: unknown = null;
        await monitorWs(id, 500, (msg) => {
          if (msg.type === 'audio' && !musicState) {
            const data = msg.data as Record<string, unknown>;
            musicState = data.m ?? null;
          }
        });
        if (!musicState) {
          return ok({ error: 'No music data received. Is streaming active?' });
        }
        return ok(musicState);
      }

      // ── Track Discovery ──

      case 'list_patterns': {
        const { directory } = args as { directory?: string };
        const dir = directory || DEFAULT_TRACK_DIR;
        return ok(await get(`/test/tracks?directory=${encodeURIComponent(dir)}`));
      }

      // ── Testing ──

      case 'run_test': {
        const { port, track_dir, tracks, duration_ms, runs, commands } = args as {
          port: string;
          track_dir?: string;
          tracks?: string[];
          duration_ms?: number;
          runs?: number;
          commands?: string[];
        };
        const id = await resolveDeviceId(port);
        const result = await post('/test/validate', {
          device_ids: [id],
          track_dir: track_dir || DEFAULT_TRACK_DIR,
          track_names: tracks,
          duration_ms: duration_ms ?? 35000,
          num_runs: runs ?? 1,
          commands,
        });
        return ok(result);
      }

      case 'run_validation_suite': {
        const { ports, track_dir, tracks, runs, duration_ms, commands, per_device_commands } = args as {
          ports: string[];
          track_dir?: string;
          tracks?: string[];
          runs?: number;
          duration_ms?: number;
          commands?: string[];
          per_device_commands?: Record<string, string[]>;
        };
        const deviceIds = await Promise.all(ports.map((p) => resolveDeviceId(p)));
        const body: Record<string, unknown> = {
          device_ids: deviceIds,
          track_dir: track_dir || DEFAULT_TRACK_DIR,
          track_names: tracks,
          num_runs: runs ?? 3,
          commands,
          per_device_commands,
        };
        if (duration_ms) body.duration_ms = duration_ms;
        const result = await post('/test/validate', body);
        return ok(result);
      }

      case 'check_test_result': {
        // Accept both job_id (new server API) and output_path (legacy file-based)
        const { job_id, output_path } = args as { job_id?: string; output_path?: string };
        if (job_id) {
          return ok(await get(`/test/jobs/${job_id}`));
        }
        // Legacy: read result from file (backward compat with old test runner)
        if (output_path) {
          try {
            const data = readFileSync(output_path, 'utf-8');
            return ok(JSON.parse(data));
          } catch {
            return ok({ status: 'pending', message: `Result file not ready: ${output_path}` });
          }
        }
        return ok({ error: 'Provide job_id (preferred) or output_path' });
      }

      // ── Simulator Preview ──

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

        const SIMULATOR_DIR = join(__dirname, '..', '..', 'blinky-simulator');
        const SIMULATOR_PATH = join(SIMULATOR_DIR, 'build', 'blinky-simulator');

        if (!existsSync(SIMULATOR_PATH)) {
          return ok({ error: 'Simulator not built. Run: cd blinky-simulator && ./build.sh' });
        }

        const cliArgs = ['-g', generator, '-e', effect, '-p', pattern, '-d', device, '-t', duration_ms.toString(), '-f', fps.toString()];
        if (output_dir) cliArgs.push('-o', output_dir);
        if (effect === 'hue' && hue_shift > 0) cliArgs.push('--hue', hue_shift.toString());
        if (params) cliArgs.push('--params', params);

        const result = await new Promise<{ success: boolean; output?: string; error?: string }>((resolve) => {
          const child = spawn(SIMULATOR_PATH, cliArgs, {
            stdio: ['ignore', 'pipe', 'pipe'],
            cwd: SIMULATOR_DIR,
          });
          let stdout = '';
          let stderr = '';
          child.stdout?.on('data', (chunk: Buffer) => { stdout += chunk.toString(); });
          child.stderr?.on('data', (chunk: Buffer) => { stderr += chunk.toString(); });
          child.on('close', (code: number | null) => {
            resolve(code === 0 ? { success: true, output: stdout } : { success: false, error: stderr || stdout });
          });
          child.on('error', (err: Error) => resolve({ success: false, error: err.message }));
        });

        if (!result.success) {
          return ok({ error: 'Simulator failed', details: result.error });
        }

        // Read output files
        const outputBase = output_dir || join(SIMULATOR_DIR, 'previews');
        const response: Record<string, unknown> = { status: 'ok', generator, effect, pattern, device };
        for (const file of ['metrics.json', 'params.json']) {
          const p = join(outputBase, file);
          if (existsSync(p)) {
            try { response[file.replace('.json', '')] = JSON.parse(readFileSync(p, 'utf-8')); } catch { /* skip */ }
          }
        }
        response.gif = join(outputBase, 'high-res.gif');
        response.gif_lowres = join(outputBase, 'low-res.gif');
        return ok(response);
      }

      default:
        return ok({ error: `Unknown tool: ${name}` });
    }
  } catch (error) {
    return ok({ error: error instanceof Error ? error.message : String(error) });
  }
});

// ── Start Server ──

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((error) => {
  console.error('MCP server error:', error);
  process.exit(1);
});
