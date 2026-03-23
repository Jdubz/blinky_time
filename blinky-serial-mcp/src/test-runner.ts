#!/usr/bin/env node
/**
 * Standalone CLI test runner for blinky music tests.
 *
 * Runs music tests against one or more blinky devices independently of the
 * MCP server.  Two subcommands:
 *
 *   run-track   Run a single audio track on N devices, score against ground truth.
 *   validate    Run a full validation suite (multiple tracks) on N devices.
 *
 * JSON results go to stdout. Human-readable progress goes to stderr.
 *
 * Usage:
 *   node dist/test-runner.js run-track --audio <path> --ground-truth <path> --ports <p1,p2,...> [options]
 *   node dist/test-runner.js validate --ports <p1,p2,...> [options]
 */

import { spawn, exec, type ChildProcess } from 'child_process';
import { existsSync, readFileSync, writeFileSync, mkdirSync } from 'fs';
import { dirname, join, resolve as pathResolve } from 'path';
import { fileURLToPath } from 'url';
import { parseArgs } from 'util';

import { DeviceSession } from './device-session.js';
import { scoreDeviceRun, formatScoreSummary, computeStats, roundStats } from './lib/scoring.js';
import { acquireAudioLock, releaseAudioLock, audioLockSigintHandler, audioLockSigtermHandler } from './lib/audio-lock.js';
import { discoverTracks } from './lib/track-discovery.js';
import type { GroundTruth, DeviceRunScore } from './lib/types.js';

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const INTER_RUN_GAP_MS = 5000;
const DEFAULT_TRACK_DIR = join(__dirname, '..', '..', 'blinky-test-player', 'music', 'edm');
const RESULTS_DIR = join(__dirname, '..', '..', 'test-results');

// Active ffplay child so we can kill it on abort.
let activeFFplay: ChildProcess | null = null;

// Connected sessions — tracked at module level for cleanup on signal.
let activeSessions: DeviceSession[] = [];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function log(...args: unknown[]): void {
  console.error(...args);
}

function sleep(ms: number): Promise<void> {
  return new Promise(r => setTimeout(r, ms));
}

function ensureResultsDir(): void {
  if (!existsSync(RESULTS_DIR)) {
    mkdirSync(RESULTS_DIR, { recursive: true });
  }
}

/** Kill any lingering ffplay processes (ours and system-wide orphans). */
async function killOrphanAudio(): Promise<void> {
  if (activeFFplay && !activeFFplay.killed) {
    activeFFplay.kill('SIGKILL');
    activeFFplay = null;
  }
  await new Promise<void>((resolve) => {
    exec('pkill -9 ffplay', () => resolve());
  });
}

/** Play an audio file through speakers via ffplay. Returns on completion. */
function playAudio(
  audioFile: string,
  durationMs?: number,
  seekSec?: number,
): Promise<{ success: boolean; error?: string }> {
  return new Promise((resolve) => {
    const args = ['-nodisp', '-autoexit', '-loglevel', 'error'];
    if (seekSec && seekSec > 0) {
      args.push('-ss', seekSec.toFixed(1));
    }
    args.push(audioFile);
    if (durationMs) {
      args.push('-t', (durationMs / 1000).toString());
    }
    const child = spawn('ffplay', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    activeFFplay = child;

    let stderr = '';
    let settled = false;

    child.stderr?.on('data', (data: Buffer) => { stderr += data.toString(); });

    child.on('error', (err: Error) => {
      activeFFplay = null;
      if (settled) return;
      settled = true;
      resolve({ success: false, error: `Failed to start ffplay: ${err.message}` });
    });

    child.on('close', (code: number | null) => {
      activeFFplay = null;
      if (settled) return;
      settled = true;
      if (code !== 0) {
        resolve({ success: false, error: `ffplay exited with code ${code}: ${stderr.slice(0, 200)}` });
      } else {
        resolve({ success: true, error: stderr ? `ffplay warning: ${stderr.slice(0, 200)}` : undefined });
      }
    });
  });
}

// ---------------------------------------------------------------------------
// Device connection
// ---------------------------------------------------------------------------

/**
 * Connect to all requested ports.  Returns the successfully connected
 * sessions.  Logs but does NOT throw on partial failure — the caller decides
 * whether to continue with a reduced device set.
 */
async function connectDevices(ports: string[]): Promise<DeviceSession[]> {
  const results = await Promise.allSettled(ports.map(async (port) => {
    const session = new DeviceSession(port);
    const info = await session.connect();
    log(`  Connected ${port} — ${info.device} v${info.version} (${info.leds} LEDs)`);
    return session;
  }));

  const sessions: DeviceSession[] = [];
  const failures: string[] = [];

  for (let i = 0; i < results.length; i++) {
    const r = results[i];
    if (r.status === 'fulfilled') {
      sessions.push(r.value);
    } else {
      const reason = r.reason as Error;
      failures.push(`${ports[i]}: ${reason.message}`);
    }
  }

  if (failures.length > 0) {
    log(`WARNING: Failed to connect ${failures.length} port(s):`);
    for (const f of failures) log(`  ${f}`);
  }

  return sessions;
}

/** Disconnect all sessions, ignoring errors. */
async function disconnectAll(sessions: DeviceSession[]): Promise<void> {
  await Promise.all(sessions.map(s => s.disconnect().catch(() => {})));
}

/** Configure gain and send pre-commands on all sessions. */
async function configureDevices(
  sessions: DeviceSession[],
  opts: {
    gain?: number;
    commands?: string[];
    portCommands?: Record<string, string[]>;
  },
): Promise<void> {
  // Lock hardware gain
  if (opts.gain !== undefined) {
    await Promise.all(sessions.map(s =>
      s.serial.sendCommand(`set hwgainlock ${opts.gain}`)
    ));
  }

  // Global pre-commands
  if (opts.commands && opts.commands.length > 0) {
    for (const session of sessions) {
      for (const cmd of opts.commands) {
        await session.serial.sendCommand(cmd);
      }
    }
  }

  // Per-port commands
  if (opts.portCommands) {
    await Promise.all(Object.entries(opts.portCommands).map(async ([port, cmds]) => {
      const session = sessions.find(s => s.port === port);
      if (session) {
        for (const cmd of cmds) {
          await session.serial.sendCommand(cmd);
        }
      }
    }));
  }
}

/** Unlock hardware gain on all connected sessions. */
async function unlockGain(sessions: DeviceSession[]): Promise<void> {
  await Promise.all(sessions.map(s =>
    s.getState().connected
      ? s.serial.sendCommand('set hwgainlock 255').catch(() => {})
      : Promise.resolve()
  ));
}

// ---------------------------------------------------------------------------
// Aggregate helpers
// ---------------------------------------------------------------------------

function buildPerDeviceAggregate(scores: DeviceRunScore[]) {
  return {
    beatF1: roundStats(computeStats(scores.map(s => s.beatTracking.f1))),
    beatPrecision: roundStats(computeStats(scores.map(s => s.beatTracking.precision))),
    beatRecall: roundStats(computeStats(scores.map(s => s.beatTracking.recall))),
    bpmAccuracy: roundStats(computeStats(
      scores.filter(s => s.musicMode.bpmAccuracy !== null).map(s => s.musicMode.bpmAccuracy!),
    )),
    transientF1: roundStats(computeStats(scores.map(s => s.transientTracking.f1))),
    latencyMs: roundStats(computeStats(
      scores.filter(s => s.audioLatencyMs !== null).map(s => s.audioLatencyMs!),
    )),
  };
}

// ---------------------------------------------------------------------------
// run-track subcommand
// ---------------------------------------------------------------------------

interface RunTrackArgs {
  audioFile: string;
  groundTruthFile: string;
  ports: string[];
  durationMs?: number;
  seekSec?: number;
  outputPath?: string;
  gain?: number;
  commands?: string[];
  portCommands?: Record<string, string[]>;
  numRuns: number;
}

async function runTrack(args: RunTrackArgs): Promise<void> {
  const {
    audioFile, groundTruthFile, ports, durationMs, seekSec,
    outputPath, gain, commands, portCommands, numRuns,
  } = args;

  // Validate inputs
  if (!existsSync(audioFile)) {
    log(`ERROR: Audio file not found: ${audioFile}`);
    process.exit(1);
  }
  if (!existsSync(groundTruthFile)) {
    log(`ERROR: Ground truth file not found: ${groundTruthFile}`);
    process.exit(1);
  }

  const gtData = JSON.parse(readFileSync(groundTruthFile, 'utf-8')) as GroundTruth;
  log(`Track: ${gtData.pattern || audioFile}`);
  log(`BPM: ${gtData.bpm || 'unknown'}, Duration: ${durationMs || gtData.durationMs || 'full track'}ms`);
  log(`Ports: ${ports.join(', ')}`);
  log(`Runs: ${numRuns}`);

  // Acquire audio lock
  if (!acquireAudioLock(ports)) {
    log('ERROR: Audio locked by another process');
    process.exit(1);
  }

  try {
    await killOrphanAudio();

    // Connect
    log('Connecting devices...');
    const sessions = await connectDevices(ports);
    activeSessions = sessions;

    if (sessions.length === 0) {
      log('ERROR: No devices connected');
      process.exit(1);
    }

    // Configure
    await configureDevices(sessions, { gain, commands, portCommands });

    // Start streaming on all devices
    for (const s of sessions) {
      await s.serial.sendCommand('stream fast');
    }

    // Per-device per-run scores
    const allRunScores: Map<string, DeviceRunScore[]> = new Map();
    for (const s of sessions) allRunScores.set(s.port, []);

    let lastPlaybackWarning: string | undefined;

    for (let run = 0; run < numRuns; run++) {
      if (numRuns > 1) log(`\n--- Run ${run + 1}/${numRuns} ---`);

      // Start recording on all devices
      for (const s of sessions) s.startTestRecording();

      // Play audio
      const audioStartTime = Date.now();
      log(`Playing audio...`);
      const result = await playAudio(audioFile, durationMs, seekSec);

      // Stop recording on all devices
      const deviceResults: Array<{ port: string; data: ReturnType<DeviceSession['stopTestRecording']> }> = [];
      for (const s of sessions) {
        deviceResults.push({ port: s.port, data: s.stopTestRecording() });
      }

      if (!result.success) {
        log(`ERROR: Audio playback failed: ${result.error}`);
        // Continue with remaining runs if we have data, otherwise abort
        if (run === 0) {
          process.exit(1);
        }
        break;
      }

      if (result.error) lastPlaybackWarning = result.error;

      // Score each device
      for (const { port, data } of deviceResults) {
        const score = scoreDeviceRun(data, audioStartTime, gtData);
        allRunScores.get(port)!.push(score);
        log(`  ${port}: transient F1=${score.transientTracking.f1}, beat F1=${score.beatTracking.f1}, BPM=${score.musicMode.avgBpm} (expected ${score.musicMode.expectedBpm})`);
      }

      // Inter-run gap
      if (run < numRuns - 1) {
        log(`Waiting ${INTER_RUN_GAP_MS / 1000}s between runs...`);
        await sleep(INTER_RUN_GAP_MS);
      }
    }

    // Build output
    const output: Record<string, unknown> = {
      pattern: gtData.pattern,
      timestamp: new Date().toISOString(),
      audioFile,
      ports: sessions.map(s => s.port),
      runs: numRuns,
      durationMs: durationMs || gtData.durationMs || null,
      bpm: gtData.bpm || null,
    };

    if (lastPlaybackWarning) {
      output.playbackWarning = lastPlaybackWarning;
    }

    if (numRuns === 1) {
      // Single run: per-device summaries
      output.perDevice = sessions.map(s => {
        const score = allRunScores.get(s.port)![0];
        return {
          port: s.port,
          ...formatScoreSummary(score),
        };
      });
    } else {
      // Multi-run: per-device aggregates
      output.perDevice = sessions.map(s => {
        const scores = allRunScores.get(s.port)!;
        return {
          port: s.port,
          runs: scores.length,
          aggregate: buildPerDeviceAggregate(scores),
          perRun: scores.map((score, i) => ({
            run: i + 1,
            ...formatScoreSummary(score),
          })),
        };
      });
    }

    // Write output
    const json = JSON.stringify(output, null, 2);
    if (outputPath) {
      ensureResultsDir();
      const resolvedOutput = pathResolve(outputPath);
      writeFileSync(resolvedOutput, json);
      log(`\nResults written to ${resolvedOutput}`);
    }
    console.log(json);

    // Unlock gain and disconnect
    if (gain !== undefined) await unlockGain(sessions);
    await disconnectAll(sessions);
    activeSessions = [];

  } finally {
    releaseAudioLock();
  }
}

// ---------------------------------------------------------------------------
// validate subcommand
// ---------------------------------------------------------------------------

interface ValidateArgs {
  ports: string[];
  trackDir?: string;
  trackNames?: string[];
  durationMs?: number;
  outputPath?: string;
  numRuns: number;
  gain?: number;
  commands?: string[];
  portCommands?: Record<string, string[]>;
}

async function validate(args: ValidateArgs): Promise<void> {
  const {
    ports, trackDir, trackNames, durationMs, outputPath, numRuns,
    gain, commands, portCommands,
  } = args;

  const dir = trackDir || DEFAULT_TRACK_DIR;
  let tracks = discoverTracks(dir);
  if (tracks.length === 0) {
    log(`ERROR: No tracks found in ${dir}`);
    process.exit(1);
  }

  // Load track manifest for seek offsets (skip intros, jump to densest beat region)
  const manifestPath = join(dir, 'track_manifest.json');
  let manifest: Record<string, { seekOffset?: number }> = {};
  if (existsSync(manifestPath)) {
    try {
      manifest = JSON.parse(readFileSync(manifestPath, 'utf-8'));
    } catch { /* ignore parse errors */ }
  }

  // Filter tracks if specified
  if (trackNames && trackNames.length > 0) {
    tracks = tracks.filter(t => trackNames.includes(t.name));
    if (tracks.length === 0) {
      log(`ERROR: None of the requested tracks found in ${dir}`);
      process.exit(1);
    }
  }

  log(`Validation suite: ${tracks.length} track(s), ${numRuns} run(s)/track, ${ports.length} port(s)`);
  log(`Tracks: ${tracks.map(t => t.name).join(', ')}`);

  // Acquire audio lock
  if (!acquireAudioLock(ports)) {
    log('ERROR: Audio locked by another process');
    process.exit(1);
  }

  try {
    await killOrphanAudio();

    // Connect
    log('\nConnecting devices...');
    const sessions = await connectDevices(ports);
    activeSessions = sessions;

    if (sessions.length === 0) {
      log('ERROR: No devices connected');
      process.exit(1);
    }

    // Configure
    await configureDevices(sessions, { gain, commands, portCommands });

    // Start streaming on all devices
    for (const s of sessions) {
      await s.serial.sendCommand('stream fast');
    }

    // Run all tracks
    const suiteStartTime = Date.now();
    const trackResults: Array<Record<string, unknown>> = [];

    for (let trackIdx = 0; trackIdx < tracks.length; trackIdx++) {
      const track = tracks[trackIdx];

      let gtData: GroundTruth;
      try {
        gtData = JSON.parse(readFileSync(track.groundTruth, 'utf-8')) as GroundTruth;
      } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        log(`\n[${trackIdx + 1}/${tracks.length}] ${track.name} — SKIPPED: ${msg}`);
        trackResults.push({ track: track.name, error: `Ground truth error: ${msg}`, perDevice: [] });
        continue;
      }

      // Get seek offset from manifest (skip intro, jump to densest beat region)
      const trackManifest = manifest[track.name];
      const seekSec = trackManifest?.seekOffset || 0;
      log(`\n[${ trackIdx + 1}/${tracks.length}] ${track.name} (${gtData.bpm || '?'} BPM${seekSec > 0 ? `, seek ${seekSec}s` : ''})`);

      // Per-device per-run scores for this track
      const trackDeviceScores: Map<string, DeviceRunScore[]> = new Map();
      for (const s of sessions) trackDeviceScores.set(s.port, []);

      let trackFailed = false;
      let lastPlaybackWarning: string | undefined;

      for (let runIdx = 0; runIdx < numRuns; runIdx++) {
        if (numRuns > 1) log(`  Run ${runIdx + 1}/${numRuns}`);

        // Start recording on all devices
        for (const s of sessions) s.startTestRecording();

        // Play audio
        const audioStartTime = Date.now();
        const result = await playAudio(track.audioFile, durationMs, seekSec);

        if (!result.success) {
          // Stop recording (discard data)
          for (const s of sessions) s.stopTestRecording();
          log(`  ERROR: ${result.error}`);

          trackResults.push({
            track: track.name,
            bpm: gtData.bpm || 0,
            error: `${result.error} (failed on run ${runIdx + 1}/${numRuns})`,
            perDevice: [],
          });
          trackFailed = true;
          break;
        }

        if (result.error) lastPlaybackWarning = result.error;

        // Stop recording and score each device
        for (const s of sessions) {
          const data = s.stopTestRecording();
          const score = scoreDeviceRun(data, audioStartTime, gtData);
          trackDeviceScores.get(s.port)!.push(score);
        }

        // Brief per-device summary for this run
        for (const s of sessions) {
          const scores = trackDeviceScores.get(s.port)!;
          const latest = scores[scores.length - 1];
          log(`    ${s.port}: tF1=${latest.transientTracking.f1} bF1=${latest.beatTracking.f1} BPM=${latest.musicMode.avgBpm}`);
        }

        // Inter-run gap
        if (runIdx < numRuns - 1) await sleep(INTER_RUN_GAP_MS);
      }

      if (trackFailed) {
        // Inter-track gap even for failed tracks
        if (trackIdx < tracks.length - 1) await sleep(INTER_RUN_GAP_MS);
        continue;
      }

      // Build track-level result
      const trackResult: Record<string, unknown> = {
        track: track.name,
        bpm: gtData.bpm || 0,
      };

      if (lastPlaybackWarning) trackResult.playbackWarning = lastPlaybackWarning;

      if (numRuns === 1) {
        trackResult.perDevice = sessions.map(s => {
          const score = trackDeviceScores.get(s.port)![0];
          return {
            port: s.port,
            ...formatScoreSummary(score),
          };
        });
      } else {
        trackResult.perDevice = sessions.map(s => {
          const scores = trackDeviceScores.get(s.port)!;
          return {
            port: s.port,
            runs: scores.length,
            aggregate: buildPerDeviceAggregate(scores),
            perRun: scores.map((score, i) => ({
              run: i + 1,
              ...formatScoreSummary(score),
            })),
          };
        });
      }

      trackResults.push(trackResult);

      // Write incremental results file so interrupted suites don't lose data
      if (outputPath) {
        ensureResultsDir();
        const incrementalOutput = {
          type: 'validation_suite',
          status: 'in_progress',
          timestamp: new Date().toISOString(),
          tracksCompleted: trackIdx + 1,
          tracksTotal: tracks.length,
          ports: sessions.map(s => s.port),
          runs: numRuns,
          tracks: trackResults,
        };
        writeFileSync(pathResolve(outputPath), JSON.stringify(incrementalOutput, null, 2));
      }

      // Inter-track gap
      if (trackIdx < tracks.length - 1) await sleep(INTER_RUN_GAP_MS);
    }

    // Build aggregate summary across all tracks.
    // Use the mean across per-track means (not all individual runs) to avoid
    // bias toward tracks with more completed runs.
    const suiteAggregate: Record<string, unknown> = {};
    for (const s of sessions) {
      const trackMeanBeatF1: number[] = [];
      const trackMeanTransientF1: number[] = [];
      const trackMeanBpmAccuracy: number[] = [];

      for (const trackResult of trackResults) {
        if (trackResult.error) continue;
        const perDevice = trackResult.perDevice as Array<Record<string, unknown>>;
        const deviceEntry = perDevice?.find((d: Record<string, unknown>) => d.port === s.port);
        if (!deviceEntry) continue;

        if (numRuns === 1) {
          // Single-run format: direct scores
          const bt = deviceEntry.beatTracking as { f1: number };
          const tt = deviceEntry.transientTracking as { f1: number };
          const mm = deviceEntry.musicMode as { bpmAccuracy: number | null };
          trackMeanBeatF1.push(bt.f1);
          trackMeanTransientF1.push(tt.f1);
          if (mm.bpmAccuracy !== null) trackMeanBpmAccuracy.push(mm.bpmAccuracy);
        } else {
          // Multi-run format: use aggregate mean
          const agg = deviceEntry.aggregate as {
            beatF1: { mean: number };
            transientF1: { mean: number };
            bpmAccuracy: { mean: number };
          };
          if (agg) {
            trackMeanBeatF1.push(agg.beatF1.mean);
            trackMeanTransientF1.push(agg.transientF1.mean);
            if (typeof agg.bpmAccuracy?.mean === 'number' && !Number.isNaN(agg.bpmAccuracy.mean)) {
              trackMeanBpmAccuracy.push(agg.bpmAccuracy.mean);
            }
          }
        }
      }

      suiteAggregate[s.port] = {
        tracksScored: trackMeanBeatF1.length,
        beatF1: roundStats(computeStats(trackMeanBeatF1)),
        transientF1: roundStats(computeStats(trackMeanTransientF1)),
        bpmAccuracy: roundStats(computeStats(trackMeanBpmAccuracy)),
      };
    }

    const suiteDurationSec = Math.round((Date.now() - suiteStartTime) / 1000);

    // Final output
    const output = {
      type: 'validation_suite',
      status: 'complete',
      timestamp: new Date().toISOString(),
      durationSec: suiteDurationSec,
      tracksTotal: tracks.length,
      tracksFailed: trackResults.filter(t => t.error).length,
      ports: sessions.map(s => s.port),
      runs: numRuns,
      aggregate: suiteAggregate,
      tracks: trackResults,
    };

    const json = JSON.stringify(output, null, 2);

    if (outputPath) {
      ensureResultsDir();
      writeFileSync(pathResolve(outputPath), json);
      log(`\nResults written to ${pathResolve(outputPath)}`);
    }

    console.log(json);

    // Summary to stderr
    log(`\n--- Suite complete ---`);
    log(`Duration: ${suiteDurationSec}s`);
    log(`Tracks: ${tracks.length} total, ${trackResults.filter(t => t.error).length} failed`);
    for (const [port, agg] of Object.entries(suiteAggregate)) {
      const a = agg as { tracksScored: number; beatF1: { mean: number }; transientF1: { mean: number }; bpmAccuracy: { mean: number } };
      log(`  ${port}: beat F1=${a.beatF1.mean}, transient F1=${a.transientF1.mean}, BPM acc=${a.bpmAccuracy.mean} (${a.tracksScored} tracks)`);
    }

    // Cleanup
    if (gain !== undefined) await unlockGain(sessions);
    await disconnectAll(sessions);
    activeSessions = [];

  } finally {
    releaseAudioLock();
  }
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

function printUsage(): void {
  log(`
Usage: node dist/test-runner.js <subcommand> [options]

Subcommands:

  run-track   Run a single audio track on one or more devices.
  validate    Run a full validation suite across multiple tracks.

run-track options:
  --audio <path>            Audio file to play (required)
  --ground-truth <path>     Ground truth .beats.json file (required)
  --ports <p1,p2,...>       Comma-separated list of serial ports (required)
  --duration <ms>           Override playback duration in milliseconds
  --output <path>           Write JSON results to file
  --gain <0-80>             Lock hardware gain for all devices
  --commands <c1,c2,...>    Comma-separated pre-test commands for all devices
  --port-commands <json>    JSON map of port -> command list (e.g. '{"ACM1":["set foo 1"]}')
  --runs <1-10>             Number of runs per track (default: 1)

validate options:
  --ports <p1,p2,...>       Comma-separated list of serial ports (required)
  --track-dir <path>        Directory containing tracks + ground truth
  --tracks <t1,t2,...>      Comma-separated track names to include
  --duration <ms>           Override playback duration per track
  --output <path>           Write JSON results to file
  --runs <1-10>             Number of runs per track (default: 3)
  --gain <0-80>             Lock hardware gain for all devices
  --commands <c1,c2,...>    Comma-separated pre-test commands for all devices
  --port-commands <json>    JSON map of port -> command list
`);
}

function parsePortCommands(raw: string): Record<string, string[]> {
  try {
    const parsed = JSON.parse(raw);
    if (typeof parsed !== 'object' || parsed === null) {
      throw new Error('port-commands must be a JSON object');
    }
    // Validate structure: { port: string[] }
    for (const [key, val] of Object.entries(parsed)) {
      if (!Array.isArray(val) || !val.every(v => typeof v === 'string')) {
        throw new Error(`port-commands["${key}"] must be an array of strings`);
      }
    }
    return parsed as Record<string, string[]>;
  } catch (err) {
    if (err instanceof SyntaxError) {
      log(`ERROR: Invalid JSON for --port-commands: ${raw}`);
    } else {
      log(`ERROR: ${(err as Error).message}`);
    }
    process.exit(1);
  }
}

async function main(): Promise<void> {
  const rawArgs = process.argv.slice(2);

  if (rawArgs.length === 0 || rawArgs[0] === '--help' || rawArgs[0] === '-h') {
    printUsage();
    process.exit(0);
  }

  const subcommand = rawArgs[0];
  const subArgs = rawArgs.slice(1);

  // Parse options (shared across subcommands)
  let parsed: ReturnType<typeof parseArgs>;
  try {
    parsed = parseArgs({
      args: subArgs,
      options: {
        audio: { type: 'string' },
        'ground-truth': { type: 'string' },
        ports: { type: 'string' },
        duration: { type: 'string' },
        output: { type: 'string' },
        gain: { type: 'string' },
        commands: { type: 'string' },
        'port-commands': { type: 'string' },
        runs: { type: 'string' },
        seek: { type: 'string' },
        'track-dir': { type: 'string' },
        tracks: { type: 'string' },
        help: { type: 'boolean', short: 'h' },
      },
      strict: true,
    });
  } catch (err) {
    log(`ERROR: ${(err as Error).message}`);
    printUsage();
    process.exit(1);
  }

  // Extract values with string casts (parseArgs types everything as string|boolean union)
  const v = parsed.values as Record<string, string | boolean | undefined>;

  if (v.help) {
    printUsage();
    process.exit(0);
  }

  // Helper to get string value
  const str = (key: string): string | undefined => {
    const val = v[key];
    return typeof val === 'string' ? val : undefined;
  };

  // Common parsed values
  const portsRaw = str('ports');
  const ports = portsRaw?.split(',').map(p => p.trim()).filter(Boolean);
  const durationRaw = str('duration');
  const durationMs = durationRaw ? parseInt(durationRaw, 10) : undefined;
  const gainRaw = str('gain');
  const gain = gainRaw ? parseInt(gainRaw, 10) : undefined;
  const commandsRaw = str('commands');
  const commandsList = commandsRaw?.split(',').map(c => c.trim()).filter(Boolean);
  const portCommandsRaw = str('port-commands');
  const portCommands = portCommandsRaw ? parsePortCommands(portCommandsRaw) : undefined;
  const runsRaw = str('runs');
  const numRuns = runsRaw ? Math.max(1, Math.min(10, parseInt(runsRaw, 10))) : undefined;

  if (!ports || ports.length === 0) {
    log('ERROR: --ports is required');
    printUsage();
    process.exit(1);
  }

  if (gain !== undefined && (isNaN(gain) || gain < 0 || gain > 80)) {
    log('ERROR: --gain must be 0-80');
    process.exit(1);
  }

  if (durationMs !== undefined && (isNaN(durationMs) || durationMs <= 0)) {
    log('ERROR: --duration must be a positive integer (milliseconds)');
    process.exit(1);
  }

  switch (subcommand) {
    case 'run-track': {
      const audioFile = str('audio');
      const gtFile = str('ground-truth');
      if (!audioFile) {
        log('ERROR: --audio is required for run-track');
        printUsage();
        process.exit(1);
      }
      if (!gtFile) {
        log('ERROR: --ground-truth is required for run-track');
        printUsage();
        process.exit(1);
      }

      const seekRaw = str('seek');
      await runTrack({
        audioFile: pathResolve(audioFile),
        groundTruthFile: pathResolve(gtFile),
        ports,
        durationMs,
        seekSec: seekRaw ? parseFloat(seekRaw) : undefined,
        outputPath: str('output'),
        gain,
        commands: commandsList,
        portCommands,
        numRuns: numRuns ?? 1,
      });
      break;
    }

    case 'validate': {
      const trackDirRaw = str('track-dir');
      const tracksRaw = str('tracks');
      await validate({
        ports,
        trackDir: trackDirRaw ? pathResolve(trackDirRaw) : undefined,
        trackNames: tracksRaw?.split(',').map(t => t.trim()).filter(Boolean),
        durationMs,
        outputPath: str('output'),
        numRuns: numRuns ?? 3,
        gain,
        commands: commandsList,
        portCommands,
      });
      break;
    }

    default:
      log(`ERROR: Unknown subcommand: ${subcommand}`);
      printUsage();
      process.exit(1);
  }
}

// ---------------------------------------------------------------------------
// Signal handling — clean up devices and audio lock on abort
// ---------------------------------------------------------------------------

function handleSignal(signal: string): void {
  log(`\nReceived ${signal}, cleaning up...`);

  // Kill ffplay
  if (activeFFplay && !activeFFplay.killed) {
    activeFFplay.kill('SIGKILL');
    activeFFplay = null;
  }

  // Disconnect devices (best-effort, synchronous-ish)
  // DeviceSession.disconnect() is async, but we need to exit promptly.
  // The audio-lock module's process.on('exit') handler will release the lock.
  for (const s of activeSessions) {
    try {
      // Write stream off directly to the port to avoid async sendCommand
      const state = s.getState();
      if (state.connected) {
        s.serial.emit('disconnected'); // trigger cleanup
      }
    } catch {
      // Ignore errors during signal cleanup
    }
  }
  activeSessions = [];

  // releaseAudioLock is called by the audio-lock module's own exit handlers,
  // so we just need to exit.
  process.exit(128 + (signal === 'SIGINT' ? 2 : 15));
}

// Remove audio-lock.ts's specific signal handlers (they call process.exit() which
// would skip our device cleanup). Our handleSignal() calls process.exit(), which
// triggers audio-lock's 'exit' handler to release the lock file.
process.removeListener('SIGINT', audioLockSigintHandler);
process.removeListener('SIGTERM', audioLockSigtermHandler);

process.on('SIGINT', () => handleSignal('SIGINT'));
process.on('SIGTERM', () => handleSignal('SIGTERM'));

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

main().catch((err) => {
  log(`FATAL: ${err.message || err}`);
  if (err.stack) log(err.stack);
  process.exit(1);
});
