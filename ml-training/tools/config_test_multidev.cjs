#!/usr/bin/env node
/**
 * Multi-config multi-device test runner.
 *
 * Tests a set of parameter configurations (e.g., from a screening design)
 * by playing each track once per config and reading all devices simultaneously.
 *
 * Input: JSON file with array of configs, each being an object of {paramName: value}.
 *
 * Usage:
 *   # Run a screening design
 *   node config_test_multidev.cjs --configs screening-design.json
 *
 *   # With custom ports and duration
 *   node config_test_multidev.cjs --configs screening-design.json --ports /dev/ttyACM0,/dev/ttyACM1
 *
 * Config file format:
 *   {
 *     "enable": "fwdfilter=1",          // settings applied before each config
 *     "configs": [
 *       {"fwdfiltlambda": 4, "fwdfiltcontrast": 1, "fwdtranssigma": 0.5, ...},
 *       {"fwdfiltlambda": 10, "fwdfiltcontrast": 5, "fwdtranssigma": 3, ...},
 *       ...
 *     ]
 *   }
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
const configsFile = getArg('--configs', '');
const settleMs = 12000;

if (!configsFile) {
  console.error('ERROR: --configs <file.json> is required');
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

  // Load config
  const configData = JSON.parse(fs.readFileSync(configsFile, 'utf-8'));
  const enableSettings = configData.enable || '';
  const configs = configData.configs || [];

  if (configs.length === 0) {
    console.error('ERROR: No configs found in file');
    releaseAudioLock();
    process.exit(1);
  }

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

  // Valid tracks
  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => f.replace('.mp3', ''))
    .filter(name => manifest[name] && manifest[name].valid)
    .sort();

  const totalRuns = configs.length * tracks.length;
  const estimatedMin = Math.round(totalRuns * (durationMs / 1000 + 4) / 60);
  console.log(`\n=== Multi-Config Test ===`);
  console.log(`Configs: ${configs.length}, Tracks: ${tracks.length}, Devices: ${ports.length}`);
  console.log(`Duration: ${durationMs}ms, Settle: ${settleMs}ms`);
  console.log(`Estimated time: ~${estimatedMin} min\n`);

  // Results: configResults[ci] = { config, tracks: [{track, trueBpm, aggMean, error, octave, perDevice}] }
  const configResults = [];

  for (let ci = 0; ci < configs.length; ci++) {
    const config = configs[ci];
    console.log(`\n--- Config ${ci + 1}/${configs.length} ---`);

    // Apply enable settings
    if (enableSettings) {
      for (const pair of enableSettings.split(',').filter(Boolean)) {
        const [key, val] = pair.split('=');
        if (key && val !== undefined) {
          await setAllDevices(ports, key.trim(), val.trim());
        }
      }
    }

    // Apply this config's settings
    const paramNames = Object.keys(config);
    for (const key of paramNames) {
      console.log(`  ${key} = ${config[key]}`);
      await setAllDevices(ports, key, config[key]);
    }
    await sleep(1000);

    const trackResults = [];

    for (let ti = 0; ti < tracks.length; ti++) {
      const name = tracks[ti];
      const entry = manifest[name];
      const trackPath = path.join(musicDir, name + '.mp3');
      const trueBpm = getGroundTruthBpm(name);

      const multi = await collectBpmMultiDevice(ports, trackPath, entry.seekOffset, durationMs);
      await sleep(2000);

      const devResults = [];
      for (let di = 0; di < ports.length; di++) {
        devResults.push(analyzeBpm(multi[`dev${di}`] || []));
      }

      const means = devResults.filter(r => r.count > 0).map(r => r.mean);
      const aggMean = means.length > 0 ? means.reduce((a, b) => a + b) / means.length : 0;
      const err = classifyError(aggMean, trueBpm);

      trackResults.push({
        track: name, trueBpm, aggMean, perDevice: means,
        error: err.error, octave: err.octave
      });

      console.log(`  [${ti + 1}/${tracks.length}] ${name.substring(0, 25).padEnd(27)} BPM: ${means.map(m => m.toFixed(0)).join('/')} avg=${aggMean.toFixed(1)} err=${err.error !== null ? err.error.toFixed(1) : '?'} oct=${err.octave ? 'YES' : 'no'}`);
    }

    configResults.push({ config, tracks: trackResults });
  }

  // Summary
  console.log('\n' + '='.repeat(100));
  console.log('CONFIG COMPARISON SUMMARY');
  console.log('='.repeat(100));

  const paramNames = Object.keys(configs[0]);
  const hdr = 'Cfg'.padEnd(5) + paramNames.map(n => n.substring(0, 12).padEnd(14)).join('') +
    'Mean Err'.padEnd(10) + 'Oct Err'.padEnd(10) + '<10 BPM'.padEnd(10) + 'Score';
  console.log(hdr);
  console.log('-'.repeat(100));

  let bestScore = Infinity, bestIdx = 0;
  for (let ci = 0; ci < configResults.length; ci++) {
    const cr = configResults[ci];
    const errors = cr.tracks.filter(t => t.error !== null).map(t => t.error);
    const octaveCount = cr.tracks.filter(t => t.octave).length;
    const meanErr = errors.length > 0 ? errors.reduce((a, b) => a + b) / errors.length : Infinity;
    const tracksOk = cr.tracks.filter(t => !t.octave && t.error !== null && t.error < 10).length;
    const score = meanErr + octaveCount * 10;

    if (score < bestScore) { bestScore = score; bestIdx = ci; }

    let line = `${ci + 1}`.padEnd(5);
    for (const pn of paramNames) {
      line += `${cr.config[pn]}`.padEnd(14);
    }
    line += `${meanErr.toFixed(1)}`.padEnd(10) +
      `${octaveCount}/${cr.tracks.length}`.padEnd(10) +
      `${tracksOk}/${cr.tracks.length}`.padEnd(10) +
      `${score.toFixed(1)}`;
    if (ci === bestIdx) line += ' *';
    console.log(line);
  }

  console.log('-'.repeat(100));
  console.log(`BEST: Config ${bestIdx + 1} (score ${bestScore.toFixed(1)})`);
  for (const pn of paramNames) {
    console.log(`  ${pn} = ${configResults[bestIdx].config[pn]}`);
  }

  // Save results
  const timestamp = Date.now();
  const outPath = `tuning-results/config-test-${timestamp}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({
    timestamp: new Date().toISOString(),
    configsFile,
    enableSettings,
    nDevices: ports.length,
    nTracks: tracks.length,
    durationMs, settleMs,
    configs,
    results: configResults
  }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  releaseAudioLock();
  for (const port of ports) port.close();
}

main().catch(e => { console.error(e); releaseAudioLock(); process.exit(1); });
