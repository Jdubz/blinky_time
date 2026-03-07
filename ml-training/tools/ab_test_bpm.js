#!/usr/bin/env node
/**
 * A/B test BPM accuracy with octave disambiguation settings.
 * Plays audio via ffplay, streams BPM from device, compares to ground truth.
 *
 * Usage: node ab_test_bpm.js --port /dev/ttyACM0 --track music/edm/dubstep-edm-halftime.mp3
 */

const { SerialPort } = require('serialport');
const { execSync, spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const portPath = args[args.indexOf('--port') + 1] || '/dev/ttyACM0';
const trackPath = args[args.indexOf('--track') + 1];
const settleMs = 5000; // wait for BPM to settle before measuring
const durationMs = args.includes('--duration') ? parseInt(args[args.indexOf('--duration') + 1]) : 30000;

if (!trackPath) {
  console.error('Usage: node ab_test_bpm.js --port /dev/ttyACM0 --track <path_to_mp3>');
  process.exit(1);
}

// Calculate ground truth BPM from beat annotations
const beatsPath = trackPath.replace('.mp3', '.beats.json');
let trueBpm = null;
if (fs.existsSync(beatsPath)) {
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  const beats = data.hits.filter(h => h.expectTrigger).map(h => h.time);
  if (beats.length > 2) {
    const ibis = [];
    for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
    trueBpm = 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
  }
}

function setSetting(port, name, value) {
  return new Promise((resolve) => {
    port.write(`set ${name} ${value}\n`);
    setTimeout(resolve, 200);
  });
}

function getSetting(port, name) {
  return new Promise((resolve) => {
    let buf = '';
    const handler = (d) => { buf += d.toString(); };
    port.on('data', handler);
    port.write(`get ${name}\n`);
    setTimeout(() => {
      port.removeListener('data', handler);
      const match = buf.match(new RegExp(`${name}\\s*=\\s*(\\S+)`));
      resolve(match ? match[1] : null);
    }, 300);
  });
}

function collectBpm(port, playDurationMs) {
  return new Promise((resolve) => {
    const bpmReadings = [];
    let buf = '';

    const handler = (d) => {
      buf += d.toString();
      const lines = buf.split('\n');
      buf = lines.pop(); // keep incomplete line
      for (const line of lines) {
        try {
          const obj = JSON.parse(line);
          if (obj.m && obj.m.bpm) {
            bpmReadings.push({
              time: Date.now(),
              bpm: obj.m.bpm,
              str: obj.m.str,
              conf: obj.m.conf,
            });
          }
        } catch (e) { /* skip non-JSON lines */ }
      }
    };

    port.on('data', handler);
    port.write('stream on\n');

    // Play audio via ffplay
    const ffplay = spawn('ffplay', ['-nodisp', '-autoexit', '-loglevel', 'quiet', trackPath]);

    const totalMs = Math.min(playDurationMs, durationMs);
    setTimeout(() => {
      ffplay.kill('SIGTERM');
      port.write('stream off\n');
      port.removeListener('data', handler);
      setTimeout(() => resolve(bpmReadings), 500);
    }, totalMs);
  });
}

function analyzeBpm(readings, settleMs) {
  if (readings.length === 0) return { mean: 0, std: 0, settled: [] };
  const startTime = readings[0].time + settleMs;
  const settled = readings.filter(r => r.time >= startTime);
  if (settled.length === 0) return { mean: 0, std: 0, settled: [] };
  const bpms = settled.map(r => r.bpm);
  const mean = bpms.reduce((a, b) => a + b) / bpms.length;
  const variance = bpms.reduce((a, b) => a + (b - mean) ** 2, 0) / bpms.length;
  return { mean, std: Math.sqrt(variance), settled, count: settled.length };
}

function bpmError(detected, actual) {
  // Check octave relationships
  const ratios = [0.5, 2/3, 1.0, 3/2, 2.0];
  let bestError = Math.abs(detected - actual);
  let bestRatio = 1.0;
  for (const r of ratios) {
    const err = Math.abs(detected - actual * r);
    if (err < bestError) { bestError = err; bestRatio = r; }
  }
  return { error: bestError, ratio: bestRatio, isOctaveError: bestRatio !== 1.0 };
}

async function runTest(port, label, config) {
  // Apply settings
  for (const [k, v] of Object.entries(config)) {
    await setSetting(port, k, v);
  }
  // Wait for settings to take effect
  await new Promise(r => setTimeout(r, 1000));

  console.log(`\n--- ${label} ---`);
  console.log(`Settings: ${JSON.stringify(config)}`);

  const readings = await collectBpm(port, durationMs);
  const result = analyzeBpm(readings, settleMs);

  console.log(`  Readings: ${result.count} (after ${settleMs}ms settle)`);
  console.log(`  Detected BPM: ${result.mean.toFixed(1)} +/- ${result.std.toFixed(1)}`);

  if (trueBpm) {
    const err = bpmError(result.mean, trueBpm);
    console.log(`  True BPM: ${trueBpm.toFixed(1)}`);
    console.log(`  Error: ${err.error.toFixed(1)} BPM (ratio: ${err.ratio})`);
    if (err.isOctaveError) {
      console.log(`  ** OCTAVE ERROR ** (locked at ${err.ratio}x true tempo)`);
    }
  }

  return { label, config, ...result, trueBpm };
}

async function main() {
  const port = new SerialPort({ path: portPath, baudRate: 115200 });
  await new Promise(r => port.on('open', r));
  // Drain any startup messages
  await new Promise(r => setTimeout(r, 1000));

  const trackName = path.basename(trackPath, '.mp3');
  console.log(`=== BPM A/B Test: ${trackName} ===`);
  console.log(`Port: ${portPath}`);
  if (trueBpm) console.log(`Ground truth BPM: ${trueBpm.toFixed(1)}`);
  console.log(`Duration: ${durationMs}ms, Settle: ${settleMs}ms`);

  const configs = [
    { label: 'Baseline (current)', config: {} },
    { label: 'templatecheck ON', config: { templatecheck: 1 } },
    { label: 'subbeatcheck ON', config: { subbeatcheck: 1 } },
    { label: 'Both ON', config: { templatecheck: 1, subbeatcheck: 1 } },
  ];

  const results = [];
  for (const { label, config } of configs) {
    const result = await runTest(port, label, config);
    results.push(result);
    // Reset to baseline between tests
    await setSetting(port, 'templatecheck', 0);
    await setSetting(port, 'subbeatcheck', 0);
    // Wait between tests for device to settle
    await new Promise(r => setTimeout(r, 2000));
  }

  console.log('\n=== Summary ===');
  console.log('Config'.padEnd(25) + 'BPM'.padEnd(12) + 'Std'.padEnd(8) + 'Error'.padEnd(10) + 'Octave?');
  for (const r of results) {
    const err = trueBpm ? bpmError(r.mean, trueBpm) : { error: 0, isOctaveError: false };
    console.log(
      r.label.padEnd(25) +
      r.mean.toFixed(1).padEnd(12) +
      r.std.toFixed(1).padEnd(8) +
      (trueBpm ? err.error.toFixed(1) : 'N/A').toString().padEnd(10) +
      (err.isOctaveError ? 'YES' : 'no')
    );
  }

  port.close();
}

main().catch(e => { console.error(e); process.exit(1); });
