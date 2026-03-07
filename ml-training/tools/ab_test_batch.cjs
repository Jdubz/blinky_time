#!/usr/bin/env node
/**
 * Batch A/B test: baseline vs subbeatcheck across all EDM tracks.
 * Plays each track twice (OFF then ON), records BPM accuracy.
 *
 * Usage: node ab_test_batch.cjs --port /dev/ttyACM0 --music-dir music/edm
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

const portPath = getArg('--port', '/dev/ttyACM0');
const musicDir = getArg('--music-dir', 'music/edm');
const durationMs = parseInt(getArg('--duration', '20000'));
const settleMs = 4000;

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

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
  const beats = data.hits.filter(h => h.expectTrigger !== false).map(h => h.time);
  if (beats.length < 3) return null;
  const ibis = [];
  for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
  return 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
}

function collectBpm(port, trackPath, playDurationMs) {
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
            bpmReadings.push({ time: Date.now(), bpm: obj.m.bpm });
          }
        } catch (e) { /* skip */ }
      }
    };

    port.on('data', handler);
    port.write('stream on\n');

    const ffplay = spawn('ffplay', ['-nodisp', '-autoexit', '-loglevel', 'quiet', trackPath]);

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

  // Find all MP3 tracks
  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => path.join(musicDir, f))
    .sort();

  console.log(`=== Batch BPM A/B Test: subbeatcheck ===`);
  console.log(`Port: ${portPath}, Tracks: ${tracks.length}, Duration: ${durationMs}ms\n`);

  const results = [];

  for (let i = 0; i < tracks.length; i++) {
    const track = tracks[i];
    const name = path.basename(track, '.mp3');
    const trueBpm = getGroundTruthBpm(track);
    console.log(`[${i + 1}/${tracks.length}] ${name} (true: ${trueBpm ? trueBpm.toFixed(0) : '?'} BPM)`);

    // Test 1: baseline (subbeatcheck OFF)
    await setSetting(port, 'subbeatcheck', 0);
    await setSetting(port, 'templatecheck', 0);
    await sleep(1500);
    const baseReadings = await collectBpm(port, track, durationMs);
    const baseResult = analyzeBpm(baseReadings);
    const baseErr = classifyError(baseResult.mean, trueBpm);
    await sleep(2000);

    // Test 2: subbeatcheck ON
    await setSetting(port, 'subbeatcheck', 1);
    await sleep(1500);
    const subReadings = await collectBpm(port, track, durationMs);
    const subResult = analyzeBpm(subReadings);
    const subErr = classifyError(subResult.mean, trueBpm);
    await sleep(2000);

    // Reset
    await setSetting(port, 'subbeatcheck', 0);

    const row = {
      track: name,
      trueBpm: trueBpm ? trueBpm.toFixed(1) : '?',
      baseBpm: baseResult.mean.toFixed(1),
      baseStd: baseResult.std.toFixed(1),
      baseErr: baseErr.error !== null ? baseErr.error.toFixed(1) : '?',
      baseOctave: baseErr.octave ? 'YES' : 'no',
      subBpm: subResult.mean.toFixed(1),
      subStd: subResult.std.toFixed(1),
      subErr: subErr.error !== null ? subErr.error.toFixed(1) : '?',
      subOctave: subErr.octave ? 'YES' : 'no',
    };
    results.push(row);

    const winner = (subErr.error !== null && baseErr.error !== null)
      ? (subErr.error < baseErr.error ? 'SUB' : (subErr.error > baseErr.error ? 'BASE' : 'TIE'))
      : '?';
    console.log(`  Base: ${row.baseBpm} ±${row.baseStd} (err ${row.baseErr}, oct: ${row.baseOctave})`);
    console.log(`  Sub:  ${row.subBpm} ±${row.subStd} (err ${row.subErr}, oct: ${row.subOctave}) → ${winner}`);
  }

  // Summary table
  console.log('\n' + '='.repeat(110));
  console.log('SUMMARY: Baseline vs subbeatcheck ON');
  console.log('='.repeat(110));
  const h = 'Track'.padEnd(30) + 'True'.padEnd(7) + 'Base BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5)
    + 'Sub BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5) + 'Winner';
  console.log(h);
  console.log('-'.repeat(110));

  let baseWins = 0, subWins = 0, ties = 0;
  let baseOctaveErrors = 0, subOctaveErrors = 0;
  let baseTotalErr = 0, subTotalErr = 0, counted = 0;

  for (const r of results) {
    const winner = (r.subErr !== '?' && r.baseErr !== '?')
      ? (parseFloat(r.subErr) < parseFloat(r.baseErr) ? 'SUB' : (parseFloat(r.subErr) > parseFloat(r.baseErr) ? 'BASE' : 'TIE'))
      : '?';
    if (winner === 'BASE') baseWins++;
    else if (winner === 'SUB') subWins++;
    else if (winner === 'TIE') ties++;
    if (r.baseOctave === 'YES') baseOctaveErrors++;
    if (r.subOctave === 'YES') subOctaveErrors++;
    if (r.baseErr !== '?' && r.subErr !== '?') {
      baseTotalErr += parseFloat(r.baseErr);
      subTotalErr += parseFloat(r.subErr);
      counted++;
    }
    console.log(
      r.track.padEnd(30) + r.trueBpm.padEnd(7) +
      (`${r.baseBpm}±${r.baseStd}`).padEnd(12) + r.baseErr.padEnd(8) + r.baseOctave.padEnd(5) +
      (`${r.subBpm}±${r.subStd}`).padEnd(12) + r.subErr.padEnd(8) + r.subOctave.padEnd(5) +
      winner
    );
  }

  console.log('-'.repeat(110));
  console.log(`Wins: Baseline=${baseWins}, Subbeat=${subWins}, Ties=${ties}`);
  console.log(`Octave errors: Baseline=${baseOctaveErrors}, Subbeat=${subOctaveErrors}`);
  if (counted > 0) {
    console.log(`Mean error: Baseline=${(baseTotalErr / counted).toFixed(1)}, Subbeat=${(subTotalErr / counted).toFixed(1)}`);
  }

  // Save results
  const outPath = `tuning-results/ab-subbeatcheck-${Date.now()}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({ timestamp: new Date().toISOString(), results }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  port.close();
}

main().catch(e => { console.error(e); process.exit(1); });
