#!/usr/bin/env node
/**
 * Parallel multi-device music test runner.
 *
 * Usage: node parallel-music-test.mjs <audio_file> <ground_truth> <port1> [port2] [port3] [port4]
 *
 * Connects all devices, starts test recording, plays audio ONCE through speakers,
 * stops recording on all devices, and computes Beat F1 / Transient F1 / BPM accuracy.
 */

import { spawn } from 'child_process';
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const mcpDist = path.resolve(__dirname, '../../blinky-serial-mcp/dist/device-session.js');
if (!existsSync(mcpDist)) {
  console.error('blinky-serial-mcp not built. Run: cd blinky-serial-mcp && npm run build');
  process.exit(1);
}
const { DeviceSession } = await import(mcpDist);
const BEAT_TOLERANCE_SEC = 0.07;

// Check ffplay is available
import { execSync } from 'child_process';
try { execSync('which ffplay', { stdio: 'ignore' }); } catch {
  console.error('ffplay not found. Install ffmpeg: sudo apt install ffmpeg');
  process.exit(1);
}

const args = process.argv.slice(2);
if (args.length < 3) {
  console.error('Usage: node parallel-music-test.mjs <audio_file> <ground_truth> <port1> [port2] ...');
  process.exit(1);
}

const audioFile = args[0];
const groundTruthFile = args[1];
const ports = args.slice(2);

if (!existsSync(audioFile)) { console.error(`Audio file not found: ${audioFile}`); process.exit(1); }
if (!existsSync(groundTruthFile)) { console.error(`Ground truth not found: ${groundTruthFile}`); process.exit(1); }

const gtData = JSON.parse(readFileSync(groundTruthFile, 'utf-8'));

function computeF1(estimated, reference, toleranceSec) {
  const matchedRef = new Set();
  let tp = 0;
  for (const est of estimated) {
    let bestIdx = -1, bestDist = Infinity;
    for (let i = 0; i < reference.length; i++) {
      if (matchedRef.has(i)) continue;
      const dist = Math.abs(est - reference[i]);
      if (dist < bestDist && dist <= toleranceSec) { bestDist = dist; bestIdx = i; }
    }
    if (bestIdx >= 0) { matchedRef.add(bestIdx); tp++; }
  }
  const precision = estimated.length > 0 ? tp / estimated.length : 0;
  const recall = reference.length > 0 ? tp / reference.length : 0;
  const f1 = (precision + recall) > 0 ? 2 * precision * recall / (precision + recall) : 0;
  return { f1, precision, recall, tp };
}

async function main() {
  const trackName = path.basename(audioFile, path.extname(audioFile));
  console.log(`\nTesting: ${trackName} (expected BPM: ${gtData.bpm || '?'})`);
  console.log(`Devices: ${ports.join(', ')}`);

  // Connect all devices
  const sessions = [];
  for (const port of ports) {
    try {
      const session = new DeviceSession(port);
      const info = await session.serial.connect(port);
      sessions.push({ port, session, info });
      const devName = typeof info?.device === 'object' ? info.device.name : info?.device;
      console.log(`  Connected: ${port} (${devName || 'unknown'})`);
    } catch (e) {
      console.error(`  Failed to connect ${port}: ${e.message}`);
    }
  }

  if (sessions.length === 0) { console.error('No devices connected'); process.exit(1); }

  try {
    // Start streaming on all devices
    for (const s of sessions) {
      await s.session.serial.sendCommand('stream fast');
    }
    await new Promise(r => setTimeout(r, 500));

    // Start test recording on all devices
    for (const s of sessions) {
      s.session.startTestRecording();
    }

    const audioStartTime = Date.now();

    // Play audio ONCE
    console.log(`  Playing audio...`);
    await new Promise((resolve, reject) => {
      const child = spawn('ffplay', ['-nodisp', '-autoexit', '-loglevel', 'error', audioFile], {
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      child.on('close', (code) => code === 0 ? resolve() : reject(new Error(`ffplay exit ${code}`)));
      child.on('error', reject);
    });
    console.log(`  Playback complete. Scoring...`);

    const refBeats = gtData.hits.filter(h => h.expectTrigger !== false).map(h => h.time);
    const expectedBPM = gtData.bpm || 0;
    const results = [];

    for (const s of sessions) {
      const testData = s.session.stopTestRecording();
      const timingOffsetMs = Math.max(0, audioStartTime - testData.startTime);
      const audioDurationSec = (testData.duration - timingOffsetMs) / 1000;
      const windowedRefBeats = refBeats.filter(t => t <= audioDurationSec);

      const detections = testData.transients
        .map(d => ({ ...d, timestampMs: d.timestampMs - timingOffsetMs }))
        .filter(d => d.timestampMs >= 0);
      const beatEvents = testData.beatEvents
        .map(b => ({ ...b, timestampMs: b.timestampMs - timingOffsetMs }))
        .filter(b => b.timestampMs >= 0);
      const musicStates = testData.musicStates
        .map(m => ({ ...m, timestampMs: m.timestampMs - timingOffsetMs }))
        .filter(m => m.timestampMs >= 0);

      // Compute latency offset
      const offsets = [];
      detections.forEach(d => {
        let minDist = Infinity, closestOffset = 0;
        windowedRefBeats.forEach(ref => {
          const offset = d.timestampMs - ref * 1000;
          if (Math.abs(offset) < Math.abs(minDist)) { minDist = offset; closestOffset = offset; }
        });
        if (Math.abs(minDist) < 1000) offsets.push(closestOffset);
      });
      offsets.sort((a, b) => a - b);
      const latencyMs = offsets.length > 0 ? offsets[Math.floor(offsets.length / 2)] : 0;

      // Beat F1
      const estBeatsSec = beatEvents.map(b => (b.timestampMs - latencyMs) / 1000);
      const beatResult = computeF1(estBeatsSec, windowedRefBeats, BEAT_TOLERANCE_SEC);

      // Transient F1
      const estTransientsSec = detections.map(d => (d.timestampMs - latencyMs) / 1000);
      const transientResult = computeF1(estTransientsSec, windowedRefBeats, BEAT_TOLERANCE_SEC);

      // BPM accuracy
      const activeStates = musicStates.filter(m => m.active);
      const avgBpm = activeStates.length > 0
        ? activeStates.reduce((sum, m) => sum + m.bpm, 0) / activeStates.length : 0;
      const bpmAccuracy = expectedBPM > 0 && avgBpm > 0
        ? Math.max(0, 1 - Math.abs(avgBpm - expectedBPM) / expectedBPM) : null;

      results.push({
        port: s.port,
        device: (typeof s.info?.device === 'object' ? s.info.device.name : s.info?.device) || 'unknown',
        beatF1: Math.round(beatResult.f1 * 1000) / 1000,
        transientF1: Math.round(transientResult.f1 * 1000) / 1000,
        avgBpm: Math.round(avgBpm * 10) / 10,
        bpmAccuracy: bpmAccuracy !== null ? Math.round(bpmAccuracy * 1000) / 1000 : null,
        beatCount: beatEvents.length,
        transientCount: detections.length,
        latencyMs: Math.round(latencyMs),
        refBeats: windowedRefBeats.length,
      });
    }

    // Print results table
    console.log(`\n  ${'Port'.padEnd(14)} ${'Device'.padEnd(12)} ${'Beat F1'.padEnd(9)} ${'BPM Acc'.padEnd(9)} ${'Avg BPM'.padEnd(9)} ${'Trans F1'.padEnd(9)} ${'Beats'.padEnd(7)} ${'Trans'.padEnd(7)}`);
    console.log('  ' + '-'.repeat(80));
    for (const r of results) {
      console.log(`  ${r.port.padEnd(14)} ${r.device.padEnd(12)} ${String(r.beatF1).padEnd(9)} ${String(r.bpmAccuracy ?? 'N/A').padEnd(9)} ${String(r.avgBpm).padEnd(9)} ${String(r.transientF1).padEnd(9)} ${String(r.beatCount).padEnd(7)} ${String(r.transientCount).padEnd(7)}`);
    }
    console.log('  ' + '-'.repeat(80));
    const avgBeatF1 = results.reduce((s, r) => s + r.beatF1, 0) / results.length;
    const avgTransF1 = results.reduce((s, r) => s + r.transientF1, 0) / results.length;
    console.log(`  ${'AVERAGE'.padEnd(26)} ${(Math.round(avgBeatF1 * 1000) / 1000 + '').padEnd(9)} ${''.padEnd(9)} ${''.padEnd(9)} ${(Math.round(avgTransF1 * 1000) / 1000 + '').padEnd(9)}`);

    // Save JSON results
    const output = {
      track: trackName,
      expectedBpm: expectedBPM,
      devices: results,
      averages: { beatF1: Math.round(avgBeatF1 * 1000) / 1000, transientF1: Math.round(avgTransF1 * 1000) / 1000 },
    };
    const resultsDir = path.resolve(__dirname, '../test-results');
    if (!existsSync(resultsDir)) mkdirSync(resultsDir, { recursive: true });
    const resultsFile = path.join(resultsDir, `v25-${trackName}.json`);
    writeFileSync(resultsFile, JSON.stringify(output, null, 2));

    console.log('\nJSON_RESULT:' + JSON.stringify(output));

  } finally {
    for (const s of sessions) {
      try { await s.session.serial.sendCommand('stream off'); } catch {}
      try { await s.session.serial.disconnect(); } catch {}
    }
  }
}

main().catch(e => { console.error('Error:', e.message); process.exit(1); });
