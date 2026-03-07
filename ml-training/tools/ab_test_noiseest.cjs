#!/usr/bin/env node
/**
 * Batch A/B test: baseline (no spectral noise subtraction) vs noiseest ON.
 * Plays each track twice (noiseest OFF then ON), records BPM accuracy.
 *
 * Usage: node ab_test_noiseest.cjs --port /dev/ttyACM0 --music-dir music/edm
 */

const { SerialPort } = require('serialport');
const { spawn, execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const portPath = args[args.indexOf('--port') + 1] || '/dev/ttyACM0';
const musicDir = args[args.indexOf('--music-dir') + 1] || 'music/edm';
const durationMs = args.includes('--duration') ? parseInt(args[args.indexOf('--duration') + 1]) : 25000;
const settleMs = 5000;

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function getTrackDuration(trackPath) {
  try {
    const out = execSync(`ffprobe -v quiet -show_entries format=duration -of csv=p=0 "${trackPath}"`, { encoding: 'utf-8' });
    return parseFloat(out.trim());
  } catch (e) { return 0; }
}

function getSeekPosition(trackPath, playDurationSec) {
  const dur = getTrackDuration(trackPath);
  if (dur <= 0) return 0;
  const center = dur / 2;
  const seekTo = Math.max(0, center - playDurationSec / 2);
  return seekTo;
}

function setSetting(port, name, value) {
  return new Promise((resolve) => {
    port.write(`set ${name} ${value}\n`);
    setTimeout(resolve, 200);
  });
}

function getGroundTruthBpm(trackPath) {
  const beatsPath = trackPath.replace('.mp3', '.beats.json');
  if (!fs.existsSync(beatsPath)) return null;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  const beats = data.hits.filter(h => h.expectTrigger).map(h => h.time);
  if (beats.length < 3) return null;
  const ibis = [];
  for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
  return 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
}

function collectBpm(port, trackPath, playDurationMs, seekSec) {
  return new Promise((resolve) => {
    const bpmReadings = [];
    let buf = '';

    const handler = (d) => {
      buf += d.toString();
      const lines = buf.split('\n');
      buf = lines.pop();
      for (const line of lines) {
        try {
          const obj = JSON.parse(line);
          if (obj.m && obj.m.bpm) {
            bpmReadings.push({ time: Date.now(), bpm: obj.m.bpm, str: obj.m.str, conf: obj.m.conf });
          }
        } catch (e) { /* skip */ }
      }
    };

    port.on('data', handler);
    port.write('stream on\n');

    const ffplayArgs = ['-nodisp', '-autoexit', '-loglevel', 'quiet'];
    if (seekSec > 0) ffplayArgs.push('-ss', seekSec.toFixed(1));
    ffplayArgs.push(trackPath);
    const ffplay = spawn('ffplay', ffplayArgs);

    setTimeout(() => {
      ffplay.kill('SIGTERM');
      port.write('stream off\n');
      port.removeListener('data', handler);
      setTimeout(() => resolve(bpmReadings), 500);
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
  const ratios = [0.5, 2/3, 1.0, 3/2, 2.0];
  let bestError = Infinity, bestRatio = 1.0;
  for (const r of ratios) {
    const err = Math.abs(detected - actual * r);
    if (err < bestError) { bestError = err; bestRatio = r; }
  }
  return { error: bestError, ratio: bestRatio, octave: bestRatio !== 1.0 };
}

async function main() {
  const port = new SerialPort({ path: portPath, baudRate: 115200 });
  await new Promise(r => port.on('open', r));
  await sleep(1000);

  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => path.join(musicDir, f))
    .sort();

  console.log(`=== Batch BPM A/B Test: Baseline vs Spectral Noise Subtraction ===`);
  console.log(`Port: ${portPath}, Tracks: ${tracks.length}, Duration: ${durationMs}ms\n`);

  const results = [];

  for (let i = 0; i < tracks.length; i++) {
    const track = tracks[i];
    const name = path.basename(track, '.mp3');
    const trueBpm = getGroundTruthBpm(track);
    const seekSec = getSeekPosition(track, durationMs / 1000);
    console.log(`[${i + 1}/${tracks.length}] ${name} (true: ${trueBpm ? trueBpm.toFixed(0) : '?'} BPM, seek: ${seekSec.toFixed(0)}s)`);

    // Test 1: baseline (noiseest OFF)
    await setSetting(port, 'noiseest', 0);
    await sleep(1500);
    const baseReadings = await collectBpm(port, track, durationMs, seekSec);
    const baseResult = analyzeBpm(baseReadings);
    const baseErr = classifyError(baseResult.mean, trueBpm);
    await sleep(2000);

    // Test 2: noise estimation ON
    await setSetting(port, 'noiseest', 1);
    await sleep(1500);
    const neReadings = await collectBpm(port, track, durationMs, seekSec);
    const neResult = analyzeBpm(neReadings);
    const neErr = classifyError(neResult.mean, trueBpm);
    await sleep(2000);

    // Reset to baseline
    await setSetting(port, 'noiseest', 0);

    const row = {
      track: name,
      trueBpm: trueBpm ? trueBpm.toFixed(1) : '?',
      baseBpm: baseResult.mean.toFixed(1),
      baseStd: baseResult.std.toFixed(1),
      baseErr: baseErr.error !== null ? baseErr.error.toFixed(1) : '?',
      baseOctave: baseErr.octave ? 'YES' : 'no',
      neBpm: neResult.mean.toFixed(1),
      neStd: neResult.std.toFixed(1),
      neErr: neErr.error !== null ? neErr.error.toFixed(1) : '?',
      neOctave: neErr.octave ? 'YES' : 'no',
    };
    results.push(row);

    const winner = (neErr.error !== null && baseErr.error !== null)
      ? (neErr.error < baseErr.error ? 'NOISE' : (neErr.error > baseErr.error ? 'BASE' : 'TIE'))
      : '?';
    console.log(`  Base:  ${row.baseBpm} +/-${row.baseStd} (err ${row.baseErr}, oct: ${row.baseOctave})`);
    console.log(`  Noise: ${row.neBpm} +/-${row.neStd} (err ${row.neErr}, oct: ${row.neOctave}) -> ${winner}`);
  }

  // Summary table
  console.log('\n' + '='.repeat(115));
  console.log('SUMMARY: Baseline vs Spectral Noise Subtraction');
  console.log('='.repeat(115));
  const h = 'Track'.padEnd(30) + 'True'.padEnd(7) + 'Base BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5)
    + 'NE BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5) + 'Winner';
  console.log(h);
  console.log('-'.repeat(115));

  let baseWins = 0, neWins = 0, ties = 0;
  let baseOctaveErrors = 0, neOctaveErrors = 0;
  let baseTotalErr = 0, neTotalErr = 0, counted = 0;

  for (const r of results) {
    const winner = (r.neErr !== '?' && r.baseErr !== '?')
      ? (parseFloat(r.neErr) < parseFloat(r.baseErr) ? 'NOISE' : (parseFloat(r.neErr) > parseFloat(r.baseErr) ? 'BASE' : 'TIE'))
      : '?';
    if (winner === 'BASE') baseWins++;
    else if (winner === 'NOISE') neWins++;
    else if (winner === 'TIE') ties++;
    if (r.baseOctave === 'YES') baseOctaveErrors++;
    if (r.neOctave === 'YES') neOctaveErrors++;
    if (r.baseErr !== '?' && r.neErr !== '?') {
      baseTotalErr += parseFloat(r.baseErr);
      neTotalErr += parseFloat(r.neErr);
      counted++;
    }
    console.log(
      r.track.padEnd(30) + r.trueBpm.padEnd(7) +
      (`${r.baseBpm}+/-${r.baseStd}`).padEnd(12) + r.baseErr.padEnd(8) + r.baseOctave.padEnd(5) +
      (`${r.neBpm}+/-${r.neStd}`).padEnd(12) + r.neErr.padEnd(8) + r.neOctave.padEnd(5) +
      winner
    );
  }

  console.log('-'.repeat(115));
  console.log(`Wins: Baseline=${baseWins}, NoiseEst=${neWins}, Ties=${ties}`);
  console.log(`Octave errors: Baseline=${baseOctaveErrors}, NoiseEst=${neOctaveErrors}`);
  if (counted > 0) {
    console.log(`Mean error: Baseline=${(baseTotalErr / counted).toFixed(1)}, NoiseEst=${(neTotalErr / counted).toFixed(1)}`);
  }

  const outPath = `tuning-results/ab-noiseest-${Date.now()}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({ timestamp: new Date().toISOString(), results }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  port.close();
}

main().catch(e => { console.error(e); process.exit(1); });
