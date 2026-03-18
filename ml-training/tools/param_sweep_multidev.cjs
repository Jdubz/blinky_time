#!/usr/bin/env node
/**
 * Multi-device parameter sweep: test a feature across its parameter range.
 *
 * Each device tests a DIFFERENT parameter value simultaneously (3x throughput).
 * Sweep values are batched: with 3 devices and 6 values, only 2 audio passes needed.
 *
 * Usage:
 *   # Sweep fwdasymmetry from 0 to 5 in 6 steps, with fwdfilter enabled
 *   node param_sweep_multidev.cjs --param fwdasymmetry --min 0 --max 5 --steps 6 \
 *     --enable "fwdfilter=1" --pre "fwdbayesbias=0.5"
 *
 *   # Sweep fwdbayesbias from 0 to 1 in 5 steps
 *   node param_sweep_multidev.cjs --param fwdbayesbias --min 0 --max 1 --steps 5 \
 *     --enable "fwdfilter=1,fwdasymmetry=2"
 *
 *   # Custom ports
 *   node param_sweep_multidev.cjs --param fwdasymmetry --min 0 --max 5 --steps 6 \
 *     --ports /dev/ttyACM0,/dev/ttyACM1
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
const musicDir = getArg('--music-dir', 'music/edm');
const durationMs = parseInt(getArg('--duration', '35000'));
const paramName = getArg('--param', '');
const paramMin = parseFloat(getArg('--min', '0'));
const paramMax = parseFloat(getArg('--max', '1'));
const paramSteps = parseInt(getArg('--steps', '5'));
const enableSettings = getArg('--enable', '');  // settings to enable before sweep (e.g., fwdfilter=1)
const preSettings = getArg('--pre', '');         // additional pre-settings
// Settle time: OSS buffer fill (5.5s) + autocorrelation convergence (~2s)
// + Bayesian posterior stabilization (~2s) + audio latency (~0.6s) = ~10s minimum.
// Use 12s for margin.
const settleMs = 12000;

if (!paramName) {
  console.error('ERROR: --param is required');
  console.error('Usage: node param_sweep_multidev.cjs --param <name> --min <v> --max <v> --steps <n> --enable "feature=1"');
  process.exit(1);
}

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
          if (killErr.code === 'ESRCH') { fs.unlinkSync(AUDIO_LOCK); return acquireAudioLock(); }
        }
        console.error(`ERROR: Audio lock held by PID ${info.pid} on ${info.ports} (started ${info.started})`);
      } catch (readErr) {
        console.error(`ERROR: Audio lock exists at ${AUDIO_LOCK}.`);
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

function setSetting(port, name, value) {
  return new Promise((resolve) => {
    port.write(`set ${name} ${value}\n`);
    setTimeout(resolve, 200);
  });
}

async function setAllDevices(ports, name, value) {
  await Promise.all(ports.map(p => setSetting(p, name, value)));
}

function loadManifest() {
  const manifestPath = path.join(musicDir, 'track_manifest.json');
  if (!fs.existsSync(manifestPath)) {
    console.error(`ERROR: No track manifest found at ${manifestPath}`);
    console.error('Run: node generate_track_manifest.cjs --music-dir ' + musicDir);
    process.exit(1);
  }
  return JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
}

function getGroundTruthBpm(trackName) {
  const beatsPath = path.join(musicDir, trackName + '.beats.json');
  if (!fs.existsSync(beatsPath)) return null;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));

  // Use explicit bpm field if available (manually verified ground truth).
  // Falls back to mean IBI computation from consensus hits.
  if (data.bpm && typeof data.bpm === 'number') return data.bpm;

  const beats = data.hits.filter(h => h.expectTrigger !== false).map(h => h.time);
  if (beats.length < 3) return null;
  const ibis = [];
  for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
  return 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
}

function isAmbiguousTrack(trackName) {
  const beatsPath = path.join(musicDir, trackName + '.beats.json');
  if (!fs.existsSync(beatsPath)) return false;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  return !!data.bpm_ambiguous;
}

function isLowQualityTrack(trackName) {
  const beatsPath = path.join(musicDir, trackName + '.beats.json');
  if (!fs.existsSync(beatsPath)) return false;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  return (data.quality_score || 1.0) < 0.5;
}

function collectBpmMultiDevice(ports, trackPath, seekOffset, playDurationMs) {
  return new Promise((resolve) => {
    const readings = {};
    const bufs = {};
    const handlers = {};

    for (let i = 0; i < ports.length; i++) {
      const devId = `dev${i}`;
      readings[devId] = [];
      bufs[devId] = '';
      handlers[devId] = (d) => {
        bufs[devId] += d.toString();
        const lines = bufs[devId].split('\n');
        bufs[devId] = lines.pop();
        for (const line of lines) {
          try {
            const obj = JSON.parse(line);
            if (obj.m && obj.m.bpm) {
              readings[devId].push({ time: Date.now(), bpm: obj.m.bpm });
            }
          } catch (e) { /* skip */ }
        }
      };
      ports[i].on('data', handlers[devId]);
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
        const devId = `dev${i}`;
        ports[i].write('stream off\n');
        ports[i].removeListener('data', handlers[devId]);
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

  const manifest = loadManifest();

  // Open ports
  console.log(`Opening ${portPaths.length} device(s)...`);
  const ports = [];
  for (const pp of portPaths) {
    try {
      const port = await openPort(pp);
      ports.push(port);
      console.log(`  ${pp}: connected`);
    } catch (e) {
      console.error(`  ${pp}: FAILED (${e.message})`);
    }
  }
  if (ports.length === 0) {
    console.error('No devices connected.');
    releaseAudioLock();
    process.exit(1);
  }

  // Apply enable-settings and pre-settings
  const allPreSettings = [enableSettings, preSettings].filter(Boolean).join(',');
  if (allPreSettings) {
    for (const pair of allPreSettings.split(',').filter(Boolean)) {
      const [key, val] = pair.split('=');
      if (key && val !== undefined) {
        console.log(`Setting: ${key.trim()} = ${val.trim()}`);
        await setAllDevices(ports, key.trim(), val.trim());
      }
    }
    await sleep(500);
  }

  // Generate sweep values
  const sweepValues = [];
  for (let i = 0; i < paramSteps; i++) {
    const value = paramMin + (paramMax - paramMin) * i / Math.max(1, paramSteps - 1);
    sweepValues.push(Math.round(value * 1000) / 1000);
  }

  // Valid tracks
  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => f.replace('.mp3', ''))
    .filter(name => manifest[name] && manifest[name].valid)
    .sort();

  // Batch sweep values across devices: each device tests a DIFFERENT value per track.
  // With 3 devices and 6 values, we need only 2 audio passes (batches) instead of 6.
  const nDev = ports.length;
  const batches = [];
  for (let i = 0; i < sweepValues.length; i += nDev) {
    batches.push(sweepValues.slice(i, i + nDev));
  }
  const totalPasses = batches.length * tracks.length;
  const estimatedMin = Math.round(totalPasses * (durationMs / 1000 + 4) / 60);

  console.log(`\n=== Parameter Sweep: ${paramName} ===`);
  console.log(`Values: [${sweepValues.join(', ')}]`);
  console.log(`Devices: ${nDev}, Tracks: ${tracks.length}, Duration: ${durationMs}ms`);
  console.log(`Batches: ${batches.length} (${nDev} values/batch), Estimated: ~${estimatedMin} min\n`);

  // Pre-allocate results indexed by sweep value
  const sweepResultsMap = {};
  for (const v of sweepValues) {
    sweepResultsMap[v] = [];
  }

  for (let bi = 0; bi < batches.length; bi++) {
    const batch = batches[bi];
    console.log(`\n--- Batch ${bi + 1}/${batches.length}: ${paramName} = [${batch.join(', ')}] ---`);

    // Set each device to a different value in this batch
    for (let di = 0; di < batch.length; di++) {
      await setSetting(ports[di], paramName, batch[di]);
      console.log(`  dev${di}: ${paramName} = ${batch[di]}`);
    }
    await sleep(1000);

    for (let ti = 0; ti < tracks.length; ti++) {
      const name = tracks[ti];
      const entry = manifest[name];
      const trackPath = path.join(musicDir, name + '.mp3');
      const trueBpm = getGroundTruthBpm(name);
      const ambiguous = isAmbiguousTrack(name);
      const lowQuality = isLowQualityTrack(name);

      const multi = await collectBpmMultiDevice(ports, trackPath, entry.seekOffset, durationMs);
      await sleep(2000);

      // Each device's reading belongs to a DIFFERENT sweep value
      let logParts = [];
      for (let di = 0; di < batch.length; di++) {
        const value = batch[di];
        const devResult = analyzeBpm(multi[`dev${di}`] || []);
        const err = classifyError(devResult.mean, trueBpm);

        // For ambiguous tracks (half-time/double-time equally valid),
        // don't count half/double as octave errors
        if (ambiguous && err.octave && (err.ratio === 0.5 || err.ratio === 2.0)) {
          err.octave = false;
        }

        const skip = lowQuality;  // exclude from aggregate stats
        sweepResultsMap[value].push({
          track: name, trueBpm, aggMean: devResult.mean, perDevice: [devResult.mean],
          error: err.error, octave: err.octave, skip, ambiguous
        });

        const suffix = skip ? '~' : (err.octave ? '!' : '');
        logParts.push(`v=${value}:${devResult.mean.toFixed(0)}${suffix}`);
      }

      console.log(`  [${ti + 1}/${tracks.length}] ${name.substring(0, 25).padEnd(27)} ${logParts.join('  ')}`);
    }
  }

  // Convert map to ordered array
  const sweepResults = sweepValues.map(v => ({ value: v, tracks: sweepResultsMap[v] }));

  // Summary: for each sweep value, aggregate stats
  console.log('\n' + '='.repeat(100));
  console.log(`SWEEP SUMMARY: ${paramName} [${paramMin} → ${paramMax}]`);
  console.log('='.repeat(100));
  console.log('Value'.padEnd(10) + 'Mean Err'.padEnd(12) + 'Octave Errs'.padEnd(14) + 'Tracks OK'.padEnd(12) + 'Best Tracks');
  console.log('-'.repeat(100));

  let bestValue = sweepValues[0], bestMeanErr = Infinity;

  for (let vi = 0; vi < sweepValues.length; vi++) {
    const value = sweepValues[vi];
    const vr = sweepResults[vi].tracks;
    // Exclude low-quality tracks from aggregate stats
    const valid = vr.filter(t => !t.skip);
    const errors = valid.filter(t => t.error !== null).map(t => t.error);
    const octaveCount = valid.filter(t => t.octave).length;
    const meanErr = errors.length > 0 ? errors.reduce((a, b) => a + b) / errors.length : Infinity;
    const tracksOk = valid.filter(t => !t.octave && t.error !== null && t.error < 10).length;

    // Score: prioritize low octave errors, then low mean error
    const score = meanErr + octaveCount * 5;  // 5 BPM penalty per octave error
    if (score < bestMeanErr + (sweepResults[sweepValues.indexOf(bestValue)] ?
        sweepResults[sweepValues.indexOf(bestValue)].tracks.filter(t => !t.skip && t.octave).length * 5 : 0)) {
      bestValue = value;
      bestMeanErr = meanErr;
    }

    const skipped = vr.filter(t => t.skip).length;
    const suffix = skipped > 0 ? ` (${skipped} skipped)` : '';
    console.log(
      `${value}`.padEnd(10) +
      `${meanErr.toFixed(1)}`.padEnd(12) +
      `${octaveCount}/${valid.length}`.padEnd(14) +
      `${tracksOk}/${valid.length}`.padEnd(12) +
      valid.filter(t => !t.octave && t.error < 5).map(t => t.track.substring(0, 15)).join(', ') +
      suffix
    );
  }

  console.log('-'.repeat(100));
  console.log(`Recommended: ${paramName} = ${bestValue} (lowest combined score)`);

  // Save full results
  const timestamp = Date.now();
  const outPath = `tuning-results/sweep-${paramName}-${timestamp}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({
    timestamp: new Date().toISOString(),
    param: paramName,
    min: paramMin, max: paramMax, steps: paramSteps,
    enableSettings, preSettings,
    nDevices: ports.length,
    durationMs, settleMs,
    sweepValues,
    results: sweepResults
  }, null, 2));
  console.log(`\nFull results saved to ${outPath}`);

  releaseAudioLock();
  for (const port of ports) port.close();
}

main().catch(e => { console.error(e); releaseAudioLock(); process.exit(1); });
