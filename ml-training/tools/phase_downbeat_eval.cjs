#!/usr/bin/env node
/**
 * Phase & Downbeat Accuracy Evaluation
 *
 * Plays audio tracks, captures firmware beat events, and evaluates:
 * - Beat timing accuracy (F1 at 70ms tolerance)
 * - Phase alignment error (how far from 0.0 at beat events)
 * - Downbeat discrimination (does NN downbeat activation distinguish bar starts?)
 *
 * Uses the multi-device pattern from param_sweep_multidev.cjs.
 * All devices test the SAME config (no parameter sweep — pure evaluation).
 *
 * Usage:
 *   node phase_downbeat_eval.cjs
 *   node phase_downbeat_eval.cjs --ports /dev/ttyACM0 --music-dir music/edm
 *   node phase_downbeat_eval.cjs --duration 40000 --settle 15000
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
const settleMs = parseInt(getArg('--settle', '12000'));
const beatTolerance = parseFloat(getArg('--tolerance', '0.070')); // 70ms

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

function loadManifest() {
  const manifestPath = path.join(musicDir, 'track_manifest.json');
  if (!fs.existsSync(manifestPath)) {
    console.error(`ERROR: No track manifest at ${manifestPath}`);
    process.exit(1);
  }
  return JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
}

function loadGroundTruth(trackName, seekOffset, windowDuration) {
  const beatsPath = path.join(musicDir, trackName + '.beats.json');
  if (!fs.existsSync(beatsPath)) return null;
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));

  // Filter hits to the seek window, convert to relative time
  const windowEnd = seekOffset + windowDuration;
  const beats = [];
  const downbeats = [];
  for (const h of data.hits) {
    if (h.time < seekOffset || h.time > windowEnd) continue;
    if (h.expectTrigger === false) continue;
    const relTime = h.time - seekOffset;
    beats.push(relTime);
    if (h.isDownbeat) downbeats.push(relTime);
  }

  // Compute BPM from IBIs
  let bpm = data.bpm;
  if (beats.length >= 3) {
    const ibis = [];
    for (let i = 1; i < beats.length; i++) ibis.push(beats[i] - beats[i - 1]);
    bpm = 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
  }

  return { beats, downbeats, bpm };
}

/**
 * Collect beat events, phase, and downbeat from all devices while playing audio.
 * Returns per-device arrays of beat event data.
 */
function collectBeatData(ports, trackPath, seekOffset, playDurationMs) {
  return new Promise((resolve) => {
    const data = {};
    const bufs = {};
    const handlers = {};
    const startTime = Date.now();

    for (let i = 0; i < ports.length; i++) {
      const devId = `dev${i}`;
      data[devId] = { beats: [], phases: [], allPhases: [] };
      bufs[devId] = '';
      handlers[devId] = (d) => {
        bufs[devId] += d.toString();
        const lines = bufs[devId].split('\n');
        bufs[devId] = lines.pop();
        for (const line of lines) {
          try {
            const obj = JSON.parse(line);
            if (!obj.m) continue;
            const elapsed = (Date.now() - startTime) / 1000; // seconds since audio start

            // Record all phase samples for interpolation
            data[devId].allPhases.push({
              time: elapsed,
              phase: obj.m.ph,
              bpm: obj.m.bpm,
              str: obj.m.str,
              downbeat: obj.m.db !== undefined ? obj.m.db : 0,
            });

            // Record beat events
            if (obj.m.q === 1) {
              data[devId].beats.push({
                time: elapsed,
                firmwareMs: obj.m.bt || 0,
                phase: obj.m.ph,
                bpm: obj.m.bpm,
                downbeat: obj.m.db !== undefined ? obj.m.db : 0,
                str: obj.m.str,
                predicted: obj.m.bp === 1,
              });
            }
          } catch (e) { /* skip malformed */ }
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
      setTimeout(() => resolve(data), 500);
    }, playDurationMs);
  });
}

/**
 * Find optimal latency offset between detected beats and ground truth.
 * Returns offset in seconds (positive = detected is late).
 */
function findLatencyOffset(detectedBeats, gtBeats, searchRange = 1.0, step = 0.01) {
  let bestOffset = 0;
  let bestMatches = 0;

  for (let offset = -searchRange; offset <= searchRange; offset += step) {
    let matches = 0;
    for (const db of detectedBeats) {
      const adjusted = db - offset;
      for (const gb of gtBeats) {
        if (Math.abs(adjusted - gb) < beatTolerance) {
          matches++;
          break;
        }
      }
    }
    if (matches > bestMatches) {
      bestMatches = matches;
      bestOffset = offset;
    }
  }

  return bestOffset;
}

/**
 * Evaluate beat accuracy (F1 with latency correction).
 */
function evaluateBeats(detectedTimes, gtBeats, latencyOffset) {
  const adjusted = detectedTimes.map(t => t - latencyOffset);
  const gtMatched = new Set();
  let truePositives = 0;

  for (const dt of adjusted) {
    let bestDist = Infinity;
    let bestIdx = -1;
    for (let i = 0; i < gtBeats.length; i++) {
      if (gtMatched.has(i)) continue;
      const dist = Math.abs(dt - gtBeats[i]);
      if (dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    if (bestDist < beatTolerance && bestIdx >= 0) {
      truePositives++;
      gtMatched.add(bestIdx);
    }
  }

  const precision = adjusted.length > 0 ? truePositives / adjusted.length : 0;
  const recall = gtBeats.length > 0 ? truePositives / gtBeats.length : 0;
  const f1 = (precision + recall > 0) ? 2 * precision * recall / (precision + recall) : 0;

  return { f1, precision, recall, truePositives, detected: adjusted.length, expected: gtBeats.length };
}

/**
 * Evaluate phase accuracy at beat events.
 * Phase should be near 0.0 at detected beats. Measures circular distance from 0.
 */
function evaluatePhase(beatEvents) {
  if (beatEvents.length === 0) return { meanError: NaN, medianError: NaN, stdError: NaN, samples: 0 };

  const errors = beatEvents.map(b => {
    // Phase = 0.0 at beat, 1.0 just before. Circular distance from 0.
    const phase = b.phase;
    return Math.min(phase, 1.0 - phase);
  });

  errors.sort((a, b) => a - b);
  const mean = errors.reduce((a, b) => a + b) / errors.length;
  const median = errors[Math.floor(errors.length / 2)];
  const variance = errors.reduce((a, b) => a + (b - mean) ** 2, 0) / errors.length;

  return { meanError: mean, medianError: median, stdError: Math.sqrt(variance), samples: errors.length };
}

/**
 * Evaluate downbeat discrimination.
 * At each detected beat matched to a ground truth beat, check:
 * - If GT says downbeat, what's the mean NN downbeat activation?
 * - If GT says non-downbeat, what's the mean NN downbeat activation?
 * Good discrimination = downbeat activation is much higher on actual downbeats.
 */
function evaluateDownbeat(beatEvents, gtBeats, gtDownbeats, latencyOffset) {
  const downbeatActivations = [];
  const nonDownbeatActivations = [];
  let dbTP = 0, dbFP = 0, dbFN = 0;
  const DB_THRESHOLD = 0.5;

  for (const b of beatEvents) {
    const adjusted = b.time - latencyOffset;

    // Find nearest GT beat
    let nearestGT = Infinity;
    let nearestIdx = -1;
    for (let i = 0; i < gtBeats.length; i++) {
      const dist = Math.abs(adjusted - gtBeats[i]);
      if (dist < nearestGT) { nearestGT = dist; nearestIdx = i; }
    }

    if (nearestGT > beatTolerance) continue; // No match

    // Check if this GT beat is a downbeat
    const gtTime = gtBeats[nearestIdx];
    const isGTDownbeat = gtDownbeats.some(db => Math.abs(db - gtTime) < 0.01);

    if (isGTDownbeat) {
      downbeatActivations.push(b.downbeat);
      if (b.downbeat >= DB_THRESHOLD) dbTP++;
      else dbFN++;
    } else {
      nonDownbeatActivations.push(b.downbeat);
      if (b.downbeat >= DB_THRESHOLD) dbFP++;
    }
  }

  const dbTN = nonDownbeatActivations.length - dbFP;
  const dbPrecision = (dbTP + dbFP > 0) ? dbTP / (dbTP + dbFP) : 0;
  const dbRecall = (dbTP + dbFN > 0) ? dbTP / (dbTP + dbFN) : 0;
  const dbF1 = (dbPrecision + dbRecall > 0) ? 2 * dbPrecision * dbRecall / (dbPrecision + dbRecall) : 0;

  const meanDbOnDownbeat = downbeatActivations.length > 0
    ? downbeatActivations.reduce((a, b) => a + b) / downbeatActivations.length : NaN;
  const meanDbOnNonDownbeat = nonDownbeatActivations.length > 0
    ? nonDownbeatActivations.reduce((a, b) => a + b) / nonDownbeatActivations.length : NaN;

  return {
    downbeatF1: dbF1,
    downbeatPrecision: dbPrecision,
    downbeatRecall: dbRecall,
    meanActivOnDownbeat: meanDbOnDownbeat,
    meanActivOnNonDownbeat: meanDbOnNonDownbeat,
    discrimination: (meanDbOnDownbeat - meanDbOnNonDownbeat) || 0, // Higher = better
    nDownbeats: downbeatActivations.length,
    nNonDownbeats: nonDownbeatActivations.length,
  };
}

/**
 * Evaluate BPM accuracy (same as param_sweep but from stream data).
 */
function evaluateBpm(beatEvents, gtBpm) {
  if (beatEvents.length === 0 || !gtBpm) return { detectedBpm: NaN, error: NaN, octave: false };

  // Use median BPM from settled readings
  const bpms = beatEvents.map(b => b.bpm).filter(b => b > 0);
  if (bpms.length === 0) return { detectedBpm: NaN, error: NaN, octave: false };
  bpms.sort((a, b) => a - b);
  const median = bpms[Math.floor(bpms.length / 2)];

  const ratios = [0.5, 2 / 3, 1.0, 3 / 2, 2.0];
  let bestError = Infinity, bestRatio = 1.0;
  for (const r of ratios) {
    const err = Math.abs(median - gtBpm * r);
    if (err < bestError) { bestError = err; bestRatio = r; }
  }

  return { detectedBpm: median, error: bestError, octave: bestRatio !== 1.0, ratio: bestRatio };
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

  // Valid tracks
  const tracks = fs.readdirSync(musicDir)
    .filter(f => f.endsWith('.mp3'))
    .map(f => f.replace('.mp3', ''))
    .filter(name => manifest[name] && manifest[name].valid)
    .sort();

  console.log(`\n=== Phase & Downbeat Evaluation ===`);
  console.log(`Devices: ${ports.length}, Tracks: ${tracks.length}, Duration: ${durationMs}ms, Settle: ${settleMs}ms`);
  console.log(`Beat tolerance: ${beatTolerance * 1000}ms\n`);

  const allResults = [];

  for (let ti = 0; ti < tracks.length; ti++) {
    const name = tracks[ti];
    const entry = manifest[name];
    const trackPath = path.join(musicDir, name + '.mp3');
    const windowDuration = durationMs / 1000;
    const gt = loadGroundTruth(name, entry.seekOffset, windowDuration);
    if (!gt) {
      console.log(`  [${ti + 1}/${tracks.length}] ${name}: no ground truth, skipping`);
      continue;
    }

    // Filter GT to settled window
    const settledStart = settleMs / 1000;
    const gtBeatsSettled = gt.beats.filter(t => t >= settledStart);
    const gtDownbeatsSettled = gt.downbeats.filter(t => t >= settledStart);

    const multi = await collectBeatData(ports, trackPath, entry.seekOffset, durationMs);
    await sleep(2000);

    // Evaluate each device
    const devResults = [];
    for (let di = 0; di < ports.length; di++) {
      const devId = `dev${di}`;
      const devData = multi[devId];

      // Filter to settled beats only
      const settledBeats = devData.beats.filter(b => b.time >= settledStart);
      const settledBeatTimes = settledBeats.map(b => b.time);

      // Find latency offset
      const latency = findLatencyOffset(settledBeatTimes, gtBeatsSettled);

      // Beat accuracy
      const beatResult = evaluateBeats(settledBeatTimes, gtBeatsSettled, latency);

      // Phase accuracy
      const phaseResult = evaluatePhase(settledBeats);

      // Downbeat discrimination
      const dbResult = evaluateDownbeat(settledBeats, gtBeatsSettled, gtDownbeatsSettled, latency);

      // BPM
      const bpmResult = evaluateBpm(settledBeats, gt.bpm);

      devResults.push({ beatResult, phaseResult, dbResult, bpmResult, latency, nBeats: settledBeats.length });
    }

    // Aggregate across devices (average)
    const avgBeatF1 = devResults.reduce((s, d) => s + d.beatResult.f1, 0) / devResults.length;
    const avgPhaseErr = devResults.reduce((s, d) => s + (isNaN(d.phaseResult.meanError) ? 0 : d.phaseResult.meanError), 0) / devResults.length;
    const avgDbF1 = devResults.reduce((s, d) => s + d.dbResult.downbeatF1, 0) / devResults.length;
    const avgDbDisc = devResults.reduce((s, d) => s + d.dbResult.discrimination, 0) / devResults.length;
    const avgLatency = devResults.reduce((s, d) => s + d.latency, 0) / devResults.length;
    const avgBpm = devResults.reduce((s, d) => s + (isNaN(d.bpmResult.detectedBpm) ? 0 : d.bpmResult.detectedBpm), 0) / devResults.length;

    const trackResult = {
      track: name,
      trueBpm: gt.bpm,
      detectedBpm: avgBpm,
      beatF1: avgBeatF1,
      phaseError: avgPhaseErr,
      downbeatF1: avgDbF1,
      downbeatDiscrimination: avgDbDisc,
      latencyMs: avgLatency * 1000,
      perDevice: devResults,
      gtBeats: gtBeatsSettled.length,
      gtDownbeats: gtDownbeatsSettled.length,
    };
    allResults.push(trackResult);

    // Log compact summary
    const octaveMarker = devResults.some(d => d.bpmResult.octave) ? '!' : '';
    console.log(
      `  [${ti + 1}/${tracks.length}] ${name.substring(0, 28).padEnd(30)}` +
      ` BPM:${avgBpm.toFixed(0)}${octaveMarker}` +
      ` BeatF1:${avgBeatF1.toFixed(3)}` +
      ` Phase:${(avgPhaseErr * 100).toFixed(1)}%` +
      ` DbF1:${avgDbF1.toFixed(3)}` +
      ` Disc:${avgDbDisc.toFixed(3)}` +
      ` Lat:${(avgLatency * 1000).toFixed(0)}ms`
    );
  }

  // Summary
  console.log('\n' + '='.repeat(110));
  console.log('SUMMARY');
  console.log('='.repeat(110));

  const valid = allResults.filter(r => !isNaN(r.beatF1));
  if (valid.length === 0) {
    console.log('No valid results.');
  } else {
    const meanBeatF1 = valid.reduce((s, r) => s + r.beatF1, 0) / valid.length;
    const meanPhaseErr = valid.reduce((s, r) => s + r.phaseError, 0) / valid.length;
    const meanDbF1 = valid.reduce((s, r) => s + r.downbeatF1, 0) / valid.length;
    const meanDbDisc = valid.reduce((s, r) => s + r.downbeatDiscrimination, 0) / valid.length;
    const meanLatency = valid.reduce((s, r) => s + r.latencyMs, 0) / valid.length;
    const octaveErrors = valid.filter(r => r.perDevice.some(d => d.bpmResult.octave)).length;

    console.log(`Tracks evaluated:   ${valid.length}`);
    console.log(`Mean Beat F1:       ${meanBeatF1.toFixed(3)}`);
    console.log(`Mean Phase Error:   ${(meanPhaseErr * 100).toFixed(1)}% of beat period (${(meanPhaseErr * 60000 / 128).toFixed(0)}ms @ 128 BPM)`);
    console.log(`Mean Downbeat F1:   ${meanDbF1.toFixed(3)}`);
    console.log(`Mean DB Discrim:    ${meanDbDisc.toFixed(3)} (higher = better separation)`);
    console.log(`Mean Latency:       ${meanLatency.toFixed(0)}ms`);
    console.log(`Octave Errors:      ${octaveErrors}/${valid.length}`);

    // Phase distribution
    const allPhaseErrors = valid.map(r => r.phaseError).sort((a, b) => a - b);
    console.log(`\nPhase Error Distribution:`);
    console.log(`  Best:   ${(allPhaseErrors[0] * 100).toFixed(1)}%  (${valid.find(r => r.phaseError === allPhaseErrors[0]).track})`);
    console.log(`  Worst:  ${(allPhaseErrors[allPhaseErrors.length - 1] * 100).toFixed(1)}%  (${valid.find(r => r.phaseError === allPhaseErrors[allPhaseErrors.length - 1]).track})`);
    console.log(`  Median: ${(allPhaseErrors[Math.floor(allPhaseErrors.length / 2)] * 100).toFixed(1)}%`);

    // Beat F1 distribution
    console.log(`\nBeat F1 Distribution:`);
    const sortedF1 = valid.map(r => ({ track: r.track, f1: r.beatF1 })).sort((a, b) => b.f1 - a.f1);
    console.log(`  Top 3:    ${sortedF1.slice(0, 3).map(r => `${r.track.substring(0, 20)}=${r.f1.toFixed(3)}`).join(', ')}`);
    console.log(`  Bottom 3: ${sortedF1.slice(-3).map(r => `${r.track.substring(0, 20)}=${r.f1.toFixed(3)}`).join(', ')}`);

    // Downbeat analysis
    console.log(`\nDownbeat Analysis:`);
    const dbValid = valid.filter(r => r.gtDownbeats > 0);
    if (dbValid.length > 0) {
      const sortedDb = dbValid.sort((a, b) => b.downbeatF1 - a.downbeatF1);
      console.log(`  Best DB F1:  ${sortedDb[0].downbeatF1.toFixed(3)} (${sortedDb[0].track})`);
      console.log(`  Worst DB F1: ${sortedDb[sortedDb.length - 1].downbeatF1.toFixed(3)} (${sortedDb[sortedDb.length - 1].track})`);

      // Mean activations on downbeats vs non-downbeats
      const allDbAct = dbValid.flatMap(r => r.perDevice.map(d => d.dbResult.meanActivOnDownbeat)).filter(v => !isNaN(v));
      const allNonDbAct = dbValid.flatMap(r => r.perDevice.map(d => d.dbResult.meanActivOnNonDownbeat)).filter(v => !isNaN(v));
      if (allDbAct.length > 0) {
        console.log(`  Mean activation on downbeats:     ${(allDbAct.reduce((a, b) => a + b) / allDbAct.length).toFixed(3)}`);
      }
      if (allNonDbAct.length > 0) {
        console.log(`  Mean activation on non-downbeats:  ${(allNonDbAct.reduce((a, b) => a + b) / allNonDbAct.length).toFixed(3)}`);
      }
    } else {
      console.log('  No tracks with downbeat ground truth.');
    }
  }

  // Save results
  const timestamp = Date.now();
  const outDir = 'tuning-results';
  fs.mkdirSync(outDir, { recursive: true });
  const outPath = path.join(outDir, `phase-downbeat-eval-${timestamp}.json`);
  fs.writeFileSync(outPath, JSON.stringify({
    timestamp: new Date().toISOString(),
    config: { durationMs, settleMs, beatTolerance, nDevices: ports.length },
    results: allResults,
  }, null, 2));
  console.log(`\nFull results saved to ${outPath}`);

  releaseAudioLock();
  for (const port of ports) port.close();
}

main().catch(e => { console.error(e); releaseAudioLock(); process.exit(1); });
