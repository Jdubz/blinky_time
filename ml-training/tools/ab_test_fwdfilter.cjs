#!/usr/bin/env node
/**
 * Batch A/B test: CBSS baseline vs forward filter across all EDM tracks.
 * Plays each track twice (fwdfilter OFF then ON), records BPM accuracy.
 *
 * Usage: node ab_test_fwdfilter.cjs --port /dev/ttyACM0 --music-dir music/edm
 */

const { SerialPort } = require('serialport');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const portPath = args[args.indexOf('--port') + 1] || '/dev/ttyACM0';
const musicDir = args[args.indexOf('--music-dir') + 1] || 'music/edm';
const durationMs = args.includes('--duration') ? parseInt(args[args.indexOf('--duration') + 1]) : 25000;
const settleMs = 5000;

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
  const beats = data.hits.filter(h => h.expectTrigger).map(h => h.time);
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
            bpmReadings.push({ time: Date.now(), bpm: obj.m.bpm, str: obj.m.str, conf: obj.m.conf });
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

  console.log(`=== Batch BPM A/B Test: CBSS baseline vs Forward Filter ===`);
  console.log(`Port: ${portPath}, Tracks: ${tracks.length}, Duration: ${durationMs}ms\n`);

  const results = [];

  for (let i = 0; i < tracks.length; i++) {
    const track = tracks[i];
    const name = path.basename(track, '.mp3');
    const trueBpm = getGroundTruthBpm(track);
    console.log(`[${i + 1}/${tracks.length}] ${name} (true: ${trueBpm ? trueBpm.toFixed(0) : '?'} BPM)`);

    // Test 1: baseline (fwdfilter OFF — uses CBSS + Bayesian)
    await setSetting(port, 'fwdfilter', 0);
    await sleep(1500);
    const baseReadings = await collectBpm(port, track, durationMs);
    const baseResult = analyzeBpm(baseReadings);
    const baseErr = classifyError(baseResult.mean, trueBpm);
    await sleep(2000);

    // Test 2: forward filter ON
    await setSetting(port, 'fwdfilter', 1);
    await sleep(1500);
    const fwdReadings = await collectBpm(port, track, durationMs);
    const fwdResult = analyzeBpm(fwdReadings);
    const fwdErr = classifyError(fwdResult.mean, trueBpm);
    await sleep(2000);

    // Reset to baseline
    await setSetting(port, 'fwdfilter', 0);

    const row = {
      track: name,
      trueBpm: trueBpm ? trueBpm.toFixed(1) : '?',
      baseBpm: baseResult.mean.toFixed(1),
      baseStd: baseResult.std.toFixed(1),
      baseErr: baseErr.error !== null ? baseErr.error.toFixed(1) : '?',
      baseOctave: baseErr.octave ? 'YES' : 'no',
      fwdBpm: fwdResult.mean.toFixed(1),
      fwdStd: fwdResult.std.toFixed(1),
      fwdErr: fwdErr.error !== null ? fwdErr.error.toFixed(1) : '?',
      fwdOctave: fwdErr.octave ? 'YES' : 'no',
    };
    results.push(row);

    const winner = (fwdErr.error !== null && baseErr.error !== null)
      ? (fwdErr.error < baseErr.error ? 'FWD' : (fwdErr.error > baseErr.error ? 'BASE' : 'TIE'))
      : '?';
    console.log(`  Base: ${row.baseBpm} ±${row.baseStd} (err ${row.baseErr}, oct: ${row.baseOctave})`);
    console.log(`  Fwd:  ${row.fwdBpm} ±${row.fwdStd} (err ${row.fwdErr}, oct: ${row.fwdOctave}) → ${winner}`);
  }

  // Summary table
  console.log('\n' + '='.repeat(115));
  console.log('SUMMARY: CBSS Baseline vs Forward Filter');
  console.log('='.repeat(115));
  const h = 'Track'.padEnd(30) + 'True'.padEnd(7) + 'Base BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5)
    + 'Fwd BPM'.padEnd(12) + 'Err'.padEnd(8) + 'Oct'.padEnd(5) + 'Winner';
  console.log(h);
  console.log('-'.repeat(115));

  let baseWins = 0, fwdWins = 0, ties = 0;
  let baseOctaveErrors = 0, fwdOctaveErrors = 0;
  let baseTotalErr = 0, fwdTotalErr = 0, counted = 0;

  for (const r of results) {
    const winner = (r.fwdErr !== '?' && r.baseErr !== '?')
      ? (parseFloat(r.fwdErr) < parseFloat(r.baseErr) ? 'FWD' : (parseFloat(r.fwdErr) > parseFloat(r.baseErr) ? 'BASE' : 'TIE'))
      : '?';
    if (winner === 'BASE') baseWins++;
    else if (winner === 'FWD') fwdWins++;
    else if (winner === 'TIE') ties++;
    if (r.baseOctave === 'YES') baseOctaveErrors++;
    if (r.fwdOctave === 'YES') fwdOctaveErrors++;
    if (r.baseErr !== '?' && r.fwdErr !== '?') {
      baseTotalErr += parseFloat(r.baseErr);
      fwdTotalErr += parseFloat(r.fwdErr);
      counted++;
    }
    console.log(
      r.track.padEnd(30) + r.trueBpm.padEnd(7) +
      (`${r.baseBpm}±${r.baseStd}`).padEnd(12) + r.baseErr.padEnd(8) + r.baseOctave.padEnd(5) +
      (`${r.fwdBpm}±${r.fwdStd}`).padEnd(12) + r.fwdErr.padEnd(8) + r.fwdOctave.padEnd(5) +
      winner
    );
  }

  console.log('-'.repeat(115));
  console.log(`Wins: Baseline=${baseWins}, ForwardFilter=${fwdWins}, Ties=${ties}`);
  console.log(`Octave errors: Baseline=${baseOctaveErrors}, ForwardFilter=${fwdOctaveErrors}`);
  if (counted > 0) {
    console.log(`Mean error: Baseline=${(baseTotalErr / counted).toFixed(1)}, ForwardFilter=${(fwdTotalErr / counted).toFixed(1)}`);
  }

  // Save results
  const outPath = `tuning-results/ab-fwdfilter-${Date.now()}.json`;
  fs.mkdirSync('tuning-results', { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify({ timestamp: new Date().toISOString(), results }, null, 2));
  console.log(`\nResults saved to ${outPath}`);

  port.close();
}

main().catch(e => { console.error(e); process.exit(1); });
