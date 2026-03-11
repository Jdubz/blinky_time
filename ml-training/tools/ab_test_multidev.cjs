#!/usr/bin/env node
/**
 * Multi-device A/B test with proper methodology:
 *
 * 1. Uses track_manifest.json for optimal seek points (densest beat region)
 * 2. Plays audio ONCE, reads ALL devices simultaneously (3 measurements per config)
 * 3. Audio lock prevents concurrent audio playback
 * 4. Supports --pre settings for initial configuration
 *
 * Usage:
 *   # Basic A/B test (toggles setting between 0 and 1)
 *   node ab_test_multidev.cjs --setting fwdfilter
 *
 *   # Custom baseline/test values (e.g., cbsscontrast 1.0 vs 2.0)
 *   node ab_test_multidev.cjs --setting cbsscontrast --baseline-val 1.0 --test-val 2.0
 *
 *   # With pre-configuration
 *   node ab_test_multidev.cjs --setting fwdfilter --pre "fwdbayesbias=0.5,fwdasymmetry=2"
 *
 *   # Custom ports and duration
 *   node ab_test_multidev.cjs --setting fwdfilter --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2 --duration 25000
 *
 *   # Single device (backwards compat)
 *   node ab_test_multidev.cjs --setting fwdfilter --ports /dev/ttyACM0
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
const settingName = getArg('--setting', 'fwdfilter');
const baselineVal = getArg('--baseline-val', '0');
const testVal = getArg('--test-val', '1');
const preSettings = getArg('--pre', '');
// Settle time: OSS buffer fill (5.5s) + autocorrelation convergence (3-5 cycles @ 250ms)
// + Bayesian posterior stabilization (~2s) + audio latency (~0.6s) = ~10s minimum.
// Use 12s for margin. Only BPM readings after this are considered.
const settleMs = 12000;

// Audio lock: prevents multiple instances from playing audio simultaneously.
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
        console.error(`\nERROR: Audio lock held by PID ${info.pid} on ${info.ports} (started ${info.started})`);
        console.error('All devices share the same room — concurrent audio tests are invalid.');
        console.error(`Remove ${AUDIO_LOCK} manually if the process is stuck.\n`);
      } catch (readErr) {
        console.error(`\nERROR: Audio lock exists at ${AUDIO_LOCK}. Another test may be running.\n`);
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
  // Calculate BPM from beat intervals (same method for all tracks)
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
 * Returns: { deviceId: [bpmReadings], ... }
 */
function collectBpmMultiDevice(ports, trackPath, seekOffset, playDurationMs) {
  return new Promise((resolve) => {
    const readings = {};
    const bufs = {};
    const handlers = {};

    // Set up data handlers for each port
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

    // Play audio (once — all devices hear it)
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
  // Acquire audio lock
  if (!acquireAudioLock()) {
    process.exit(1);
  }
  process.on('exit', releaseAudioLock);
  process.on('SIGINT', () => { releaseAudioLock(); process.exit(130); });
  process.on('SIGTERM', () => { releaseAudioLock(); process.exit(143); });

  // Load track manifest
  const manifest = loadManifest();

  // Open all serial ports
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
    console.error('No devices connected. Exiting.');
    releaseAudioLock();
    process.exit(1);
  }
  console.log(`\n${ports.length} device(s) ready.`);
  await sleep(500);

  // Apply pre-settings to all devices
  if (preSettings) {
    const pairs = preSettings.split(',').filter(Boolean);
    for (const pair of pairs) {
      const [key, val] = pair.split('=');
      if (key && val !== undefined) {
        console.log(`Pre-setting: ${key.trim()} = ${val.trim()}`);
        await setAllDevices(ports, key.trim(), val.trim());
      }
    }
    await sleep(500);
  }

  // Find valid tracks
  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => f.replace('.mp3', ''))
    .filter(name => manifest[name] && manifest[name].valid)
    .sort();

  console.log(`\n=== Multi-Device A/B Test: ${settingName} (${baselineVal} vs ${testVal}) ===`);
  console.log(`Devices: ${ports.length}, Tracks: ${tracks.length}, Duration: ${durationMs}ms, Settle: ${settleMs}ms\n`);

  const results = [];

  for (let ti = 0; ti < tracks.length; ti++) {
    const name = tracks[ti];
    const entry = manifest[name];
    const trackPath = path.join(musicDir, name + '.mp3');
    const trueBpm = getGroundTruthBpm(name);

    console.log(`[${ti + 1}/${tracks.length}] ${name} (true: ${trueBpm ? trueBpm.toFixed(0) : '?'} BPM, seek: ${entry.seekOffset}s)`);

    // === BASELINE ===
    await setAllDevices(ports, settingName, baselineVal);
    await sleep(1500);
    const baseMulti = await collectBpmMultiDevice(ports, trackPath, entry.seekOffset, durationMs);
    await sleep(2000);

    // === TEST ===
    await setAllDevices(ports, settingName, testVal);
    await sleep(1500);
    const testMulti = await collectBpmMultiDevice(ports, trackPath, entry.seekOffset, durationMs);
    await sleep(2000);

    // Reset to baseline
    await setAllDevices(ports, settingName, baselineVal);

    // Analyze each device's readings
    const baseDevResults = [];
    const testDevResults = [];
    for (let di = 0; di < ports.length; di++) {
      const devId = `dev${di}`;
      baseDevResults.push(analyzeBpm(baseMulti[devId] || []));
      testDevResults.push(analyzeBpm(testMulti[devId] || []));
    }

    // Aggregate across devices: mean of means, pooled std
    const baseMeans = baseDevResults.filter(r => r.count > 0).map(r => r.mean);
    const testMeans = testDevResults.filter(r => r.count > 0).map(r => r.mean);
    const baseMean = baseMeans.length > 0 ? baseMeans.reduce((a, b) => a + b) / baseMeans.length : 0;
    const testMean = testMeans.length > 0 ? testMeans.reduce((a, b) => a + b) / testMeans.length : 0;
    const baseStd = baseMeans.length > 1
      ? Math.sqrt(baseMeans.reduce((a, b) => a + (b - baseMean) ** 2, 0) / baseMeans.length)
      : (baseDevResults[0] ? baseDevResults[0].std : 0);
    const testStd = testMeans.length > 1
      ? Math.sqrt(testMeans.reduce((a, b) => a + (b - testMean) ** 2, 0) / testMeans.length)
      : (testDevResults[0] ? testDevResults[0].std : 0);

    const baseErr = classifyError(baseMean, trueBpm);
    const testErr = classifyError(testMean, trueBpm);

    const row = {
      track: name,
      trueBpm: trueBpm ? trueBpm.toFixed(1) : '?',
      seekOffset: entry.seekOffset,
      nDevices: ports.length,
      baseMeans: baseMeans.map(b => b.toFixed(1)),
      baseBpm: baseMean.toFixed(1),
      baseStd: baseStd.toFixed(1),
      baseErr: baseErr.error !== null ? baseErr.error.toFixed(1) : '?',
      baseOctave: baseErr.octave ? 'YES' : 'no',
      testMeans: testMeans.map(b => b.toFixed(1)),
      testBpm: testMean.toFixed(1),
      testStd: testStd.toFixed(1),
      testErr: testErr.error !== null ? testErr.error.toFixed(1) : '?',
      testOctave: testErr.octave ? 'YES' : 'no',
    };
    results.push(row);

    const winner = (testErr.error !== null && baseErr.error !== null)
      ? (testErr.error < baseErr.error ? 'TEST' : (testErr.error > baseErr.error ? 'BASE' : 'TIE'))
      : '?';

    // Per-device detail
    for (let di = 0; di < ports.length; di++) {
      const br = baseDevResults[di], tr = testDevResults[di];
      console.log(`  dev${di}: base=${br.mean.toFixed(1)}+/-${br.std.toFixed(1)}  test=${tr.mean.toFixed(1)}+/-${tr.std.toFixed(1)}`);
    }
    console.log(`  AGG:  base=${row.baseBpm}+/-${row.baseStd} (err ${row.baseErr}, oct: ${row.baseOctave})  test=${row.testBpm}+/-${row.testStd} (err ${row.testErr}, oct: ${row.testOctave}) -> ${winner}`);
  }

  // Summary table
  console.log('\n' + '='.repeat(120));
  console.log(`SUMMARY: ${settingName}=${baselineVal} (baseline) vs ${settingName}=${testVal} (test) — ${ports.length} devices × ${tracks.length} tracks`);
  console.log('='.repeat(120));
  const h = 'Track'.padEnd(30) + 'True'.padEnd(7) + 'Seek'.padEnd(6) + 'Base BPM'.padEnd(14) + 'Err'.padEnd(8) + 'Oct'.padEnd(5)
    + 'Test BPM'.padEnd(14) + 'Err'.padEnd(8) + 'Oct'.padEnd(5) + 'Winner';
  console.log(h);
  console.log('-'.repeat(120));

  let baseWins = 0, testWins = 0, ties = 0;
  let baseOctaveErrors = 0, testOctaveErrors = 0;
  let baseTotalErr = 0, testTotalErr = 0, counted = 0;

  for (const r of results) {
    const winner = (r.testErr !== '?' && r.baseErr !== '?')
      ? (parseFloat(r.testErr) < parseFloat(r.baseErr) ? 'TEST' : (parseFloat(r.testErr) > parseFloat(r.baseErr) ? 'BASE' : 'TIE'))
      : '?';
    if (winner === 'BASE') baseWins++;
    else if (winner === 'TEST') testWins++;
    else if (winner === 'TIE') ties++;
    if (r.baseOctave === 'YES') baseOctaveErrors++;
    if (r.testOctave === 'YES') testOctaveErrors++;
    if (r.baseErr !== '?' && r.testErr !== '?') {
      baseTotalErr += parseFloat(r.baseErr);
      testTotalErr += parseFloat(r.testErr);
      counted++;
    }
    console.log(
      r.track.padEnd(30) + r.trueBpm.padEnd(7) +
      `${r.seekOffset}s`.padEnd(6) +
      (`${r.baseBpm}+/-${r.baseStd}`).padEnd(14) + r.baseErr.padEnd(8) + r.baseOctave.padEnd(5) +
      (`${r.testBpm}+/-${r.testStd}`).padEnd(14) + r.testErr.padEnd(8) + r.testOctave.padEnd(5) +
      winner
    );
  }

  console.log('-'.repeat(120));
  console.log(`Wins: Baseline=${baseWins}, ${settingName}=${testWins}, Ties=${ties}`);
  console.log(`Octave errors: Baseline=${baseOctaveErrors}, ${settingName}=${testOctaveErrors}`);
  if (counted > 0) {
    console.log(`Mean error: Baseline=${(baseTotalErr / counted).toFixed(1)}, ${settingName}=${(testTotalErr / counted).toFixed(1)}`);
  }
  console.log(`Devices: ${ports.length}, Measurements per config per track: ${ports.length}`);

  // Save results
  const timestamp = Date.now();
  const outPath = `tuning-results/ab-multidev-${settingName}-${timestamp}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({
    timestamp: new Date().toISOString(),
    setting: settingName,
    baselineVal,
    testVal,
    preSettings: preSettings || null,
    nDevices: ports.length,
    durationMs,
    settleMs,
    results
  }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  releaseAudioLock();
  for (const port of ports) port.close();
}

main().catch(e => { console.error(e); releaseAudioLock(); process.exit(1); });
