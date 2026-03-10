#!/usr/bin/env node
/**
 * Multi-device model comparison test.
 *
 * Each device runs a different NN model (baked into firmware).
 * Plays each track ONCE and reads all devices simultaneously,
 * then compares per-device BPM accuracy against ground truth.
 *
 * Usage:
 *   node model_compare_multidev.cjs --labels v6,v7,v8
 *   node model_compare_multidev.cjs --labels v6,v7,v8 --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2
 *   node model_compare_multidev.cjs --labels v6,v7,v8 --duration 35000
 */

const { SerialPort } = require('serialport');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
function getArg(name, defaultValue) {
  const idx = args.indexOf(name);
  if (idx === -1 || idx + 1 >= args.length) return defaultValue;
  return args[idx + 1];
}

const portsArg = getArg('--ports', '/dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2');
const portPaths = portsArg.split(',').map(s => s.trim());
const labelsArg = getArg('--labels', portPaths.map((_, i) => `dev${i}`).join(','));
const labels = labelsArg.split(',').map(s => s.trim());
const musicDir = getArg('--music-dir', 'music/edm');
const durationMs = parseInt(getArg('--duration', '35000'));
const settleMs = 12000;

const AUDIO_LOCK = '/tmp/blinky-audio.lock';

function acquireAudioLock() {
  try {
    const fd = fs.openSync(AUDIO_LOCK, fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_WRONLY);
    fs.writeSync(fd, JSON.stringify({ pid: process.pid, ports: portPaths, started: new Date().toISOString() }));
    fs.closeSync(fd);
    return true;
  } catch (e) {
    if (e.code === 'EEXIST') {
      try {
        const info = JSON.parse(fs.readFileSync(AUDIO_LOCK, 'utf-8'));
        try { process.kill(info.pid, 0); } catch (killErr) {
          if (killErr.code === 'ESRCH') {
            fs.unlinkSync(AUDIO_LOCK);
            return acquireAudioLock();
          }
        }
        console.error(`\nERROR: Audio lock held by PID ${info.pid} (started ${info.started})`);
        console.error(`Remove ${AUDIO_LOCK} manually if the process is stuck.\n`);
      } catch (readErr) {
        console.error(`\nERROR: Audio lock exists at ${AUDIO_LOCK}.\n`);
      }
      return false;
    }
    throw e;
  }
}

function releaseAudioLock() {
  try { fs.unlinkSync(AUDIO_LOCK); } catch (e) { /* ignore */ }
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function openPort(portPath) {
  const port = new SerialPort({ path: portPath, baudRate: 115200 });
  await new Promise((resolve, reject) => {
    port.on('open', resolve);
    port.on('error', reject);
  });
  await sleep(500);
  return port;
}

function loadManifest() {
  const manifestPath = path.join(musicDir, 'track_manifest.json');
  if (!fs.existsSync(manifestPath)) {
    console.error(`ERROR: No track manifest found at ${manifestPath}`);
    process.exit(1);
  }
  return JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
}

function getGroundTruthBpm(trackName) {
  const beatsPath = path.join(musicDir, trackName + '.beats.json');
  if (!fs.existsSync(beatsPath)) return null;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  const beats = data.hits.filter(h => h.expectTrigger !== false).map(h => h.time);
  if (beats.length < 3) return null;
  const ibis = [];
  for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
  return 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
}

/**
 * Play audio once, collect BPM readings from ALL devices simultaneously.
 */
function collectBpmMultiDevice(ports, trackPath, seekOffset, playDurationMs) {
  return new Promise((resolve) => {
    const readings = {};
    const bufs = {};
    const handlers = {};

    for (let i = 0; i < ports.length; i++) {
      const deviceId = `dev${i}`;
      readings[deviceId] = [];
      bufs[deviceId] = '';

      handlers[deviceId] = (d) => {
        bufs[deviceId] += d.toString();
        const lines = bufs[deviceId].split('\n');
        bufs[deviceId] = lines.pop();
        for (const line of lines) {
          try {
            const obj = JSON.parse(line);
            if (obj.m && obj.m.bpm) {
              readings[deviceId].push({ time: Date.now(), bpm: obj.m.bpm });
            }
          } catch (e) { /* skip */ }
        }
      };

      ports[i].on('data', handlers[deviceId]);
      ports[i].write('stream on\n');
    }

    const ffplay = spawn('ffplay', [
      '-nodisp', '-autoexit', '-loglevel', 'quiet',
      '-ss', seekOffset.toFixed(1),
      trackPath
    ]);

    setTimeout(() => {
      ffplay.kill('SIGTERM');
      for (let i = 0; i < ports.length; i++) {
        const deviceId = `dev${i}`;
        ports[i].write('stream off\n');
        ports[i].removeListener('data', handlers[deviceId]);
      }
      setTimeout(() => resolve(readings), 500);
    }, playDurationMs);
  });
}

function analyzeBpm(readings) {
  if (readings.length === 0) return { mean: 0, std: 0, count: 0 };
  const startTime = readings[0].time + settleMs;
  const settled = readings.filter(r => r.time >= startTime);
  if (settled.length === 0) return { mean: 0, std: 0, count: 0 };
  const bpms = settled.map(r => r.bpm);
  const mean = bpms.reduce((a, b) => a + b) / bpms.length;
  const variance = bpms.reduce((a, b) => a + (b - mean) ** 2, 0) / bpms.length;
  return { mean, std: Math.sqrt(variance), count: settled.length };
}

function classifyError(detected, actual) {
  if (!actual) return { error: null, ratio: null, octave: false };
  const ratios = [0.5, 2 / 3, 1.0, 3 / 2, 2.0];
  let bestError = Infinity, bestRatio = 1.0;
  for (const r of ratios) {
    const err = Math.abs(detected - actual * r);
    if (err < bestError) { bestError = err; bestRatio = r; }
  }
  return { error: bestError, ratio: bestRatio, octave: bestRatio !== 1.0 };
}

async function main() {
  if (!acquireAudioLock()) process.exit(1);
  process.on('exit', releaseAudioLock);
  process.on('SIGINT', () => { releaseAudioLock(); process.exit(130); });
  process.on('SIGTERM', () => { releaseAudioLock(); process.exit(143); });

  if (labels.length !== portPaths.length) {
    console.error(`ERROR: ${labels.length} labels but ${portPaths.length} ports. Must match.`);
    releaseAudioLock();
    process.exit(1);
  }

  const manifest = loadManifest();

  console.log(`Opening ${portPaths.length} device(s)...`);
  const ports = [];
  for (let i = 0; i < portPaths.length; i++) {
    try {
      const port = await openPort(portPaths[i]);
      ports.push(port);
      console.log(`  ${portPaths[i]} [${labels[i]}]: connected`);
    } catch (e) {
      console.error(`  ${portPaths[i]} [${labels[i]}]: FAILED (${e.message})`);
      releaseAudioLock();
      process.exit(1);
    }
  }
  await sleep(500);

  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => f.replace('.mp3', ''))
    .filter(name => manifest[name] && manifest[name].valid)
    .sort();

  console.log(`\n=== Model Comparison: ${labels.join(' vs ')} ===`);
  console.log(`Tracks: ${tracks.length}, Duration: ${durationMs}ms, Settle: ${settleMs}ms\n`);

  const results = [];

  for (let ti = 0; ti < tracks.length; ti++) {
    const name = tracks[ti];
    const entry = manifest[name];
    const trackPath = path.join(musicDir, name + '.mp3');
    const trueBpm = getGroundTruthBpm(name);

    console.log(`[${ti + 1}/${tracks.length}] ${name} (true: ${trueBpm ? trueBpm.toFixed(0) : '?'} BPM, seek: ${entry.seekOffset}s)`);

    const multi = await collectBpmMultiDevice(ports, trackPath, entry.seekOffset, durationMs);
    await sleep(2000);

    const row = { track: name, trueBpm, seekOffset: entry.seekOffset, models: {} };

    for (let di = 0; di < ports.length; di++) {
      const devId = `dev${di}`;
      const label = labels[di];
      const stats = analyzeBpm(multi[devId] || []);
      const err = classifyError(stats.mean, trueBpm);
      row.models[label] = {
        bpm: stats.mean,
        std: stats.std,
        count: stats.count,
        error: err.error,
        octave: err.octave,
        ratio: err.ratio,
      };
      console.log(`  ${label}: ${stats.mean.toFixed(1)} +/- ${stats.std.toFixed(1)} BPM  err=${err.error !== null ? err.error.toFixed(1) : '?'}  oct=${err.octave ? 'YES' : 'no'}`);
    }
    results.push(row);
  }

  // --- Summary table ---
  const modelNames = labels;
  const colW = 20;
  const trackW = 28;

  console.log('\n' + '='.repeat(trackW + 7 + modelNames.length * colW));
  console.log(`MODEL COMPARISON: ${modelNames.join(' vs ')} (${tracks.length} tracks)`);
  console.log('='.repeat(trackW + 7 + modelNames.length * colW));

  let header = 'Track'.padEnd(trackW) + 'True'.padEnd(7);
  for (const m of modelNames) header += m.padEnd(colW);
  header += 'Best';
  console.log(header);
  console.log('-'.repeat(trackW + 7 + modelNames.length * colW + 6));

  const totals = {};
  for (const m of modelNames) totals[m] = { wins: 0, totalErr: 0, octaveErrors: 0, counted: 0 };

  for (const r of results) {
    let line = r.track.padEnd(trackW) + (r.trueBpm ? r.trueBpm.toFixed(0) : '?').padEnd(7);

    let bestLabel = '?';
    let bestErr = Infinity;

    for (const m of modelNames) {
      const md = r.models[m];
      const errStr = md.error !== null ? md.error.toFixed(1) : '?';
      const octStr = md.octave ? '*' : '';
      const cell = `${md.bpm.toFixed(1)} e${errStr}${octStr}`;
      line += cell.padEnd(colW);

      if (md.error !== null) {
        totals[m].totalErr += md.error;
        totals[m].counted++;
        if (md.octave) totals[m].octaveErrors++;
        if (md.error < bestErr) { bestErr = md.error; bestLabel = m; }
      }
    }
    line += bestLabel;
    console.log(line);

    if (bestLabel !== '?') totals[bestLabel].wins++;
  }

  console.log('-'.repeat(trackW + 7 + modelNames.length * colW + 6));

  // Summary stats
  let summaryLine = 'WINS'.padEnd(trackW + 7);
  for (const m of modelNames) summaryLine += `${totals[m].wins}`.padEnd(colW);
  console.log(summaryLine);

  let errLine = 'Mean error'.padEnd(trackW + 7);
  for (const m of modelNames) {
    const me = totals[m].counted > 0 ? (totals[m].totalErr / totals[m].counted).toFixed(1) : '?';
    errLine += me.padEnd(colW);
  }
  console.log(errLine);

  let octLine = 'Octave errors'.padEnd(trackW + 7);
  for (const m of modelNames) octLine += `${totals[m].octaveErrors}/${tracks.length}`.padEnd(colW);
  console.log(octLine);

  // Score: mean error + 10 * octave_error_rate (penalize octave errors heavily)
  let scoreLine = 'Score (lower=better)'.padEnd(trackW + 7);
  for (const m of modelNames) {
    const me = totals[m].counted > 0 ? totals[m].totalErr / totals[m].counted : 999;
    const octRate = totals[m].octaveErrors / tracks.length;
    const score = me + 10 * octRate * 100;  // 10 points per % octave error rate
    scoreLine += score.toFixed(1).padEnd(colW);
  }
  console.log(scoreLine);

  console.log('  * = octave error (half/double/3:2 time)');

  // Save results
  const timestamp = Date.now();
  const outPath = `tuning-results/model-compare-${labels.join('-')}-${timestamp}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({
    timestamp: new Date().toISOString(),
    labels,
    ports: portPaths,
    nTracks: tracks.length,
    durationMs,
    settleMs,
    results,
    summary: totals,
  }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  releaseAudioLock();
  for (const port of ports) port.close();
}

main().catch(e => { console.error(e); releaseAudioLock(); process.exit(1); });
