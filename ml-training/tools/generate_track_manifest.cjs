#!/usr/bin/env node
/**
 * Generate a track manifest with optimal seek offsets for testing.
 *
 * Analyzes .beats.json ground truth to find the densest rhythmic section
 * of each track. This ensures A/B tests play content with strong periodicity,
 * not intros/breakdowns/outros.
 *
 * Output: music/edm/track_manifest.json
 *
 * Usage:
 *   node generate_track_manifest.cjs --music-dir music/edm --window 25
 */

const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
function getArg(name, defaultValue) {
  const idx = args.indexOf(name);
  if (idx === -1 || idx + 1 >= args.length) return defaultValue;
  return args[idx + 1];
}

const musicDir = getArg('--music-dir', 'music/edm');
const windowSec = parseInt(getArg('--window', '25'));
const minBeatsInWindow = parseInt(getArg('--min-beats', '30'));

function getTrackDuration(trackPath) {
  try {
    const { execSync } = require('child_process');
    const out = execSync(
      `ffprobe -v quiet -show_entries format=duration -of csv=p=0 "${trackPath}"`,
      { encoding: 'utf-8' }
    );
    return parseFloat(out.trim());
  } catch (e) { return null; }
}

const tracks = fs.readdirSync(musicDir)
  .filter(f => f.endsWith('.mp3'))
  .sort();

const manifest = {};

console.log(`Analyzing ${tracks.length} tracks in ${musicDir} (window=${windowSec}s, min=${minBeatsInWindow} beats)\n`);
console.log('Track'.padEnd(35) + 'Duration'.padEnd(10) + 'Best Window'.padEnd(15) + 'Beats'.padEnd(8) + 'BPM'.padEnd(8) + 'Seek');
console.log('-'.repeat(90));

for (const trackFile of tracks) {
  const name = trackFile.replace('.mp3', '');
  const trackPath = path.join(musicDir, trackFile);
  const beatsPath = path.join(musicDir, name + '.beats.json');

  if (!fs.existsSync(beatsPath)) {
    console.log(`${name.padEnd(35)} NO BEATS FILE — SKIPPED`);
    continue;
  }

  const duration = getTrackDuration(trackPath);
  const data = JSON.parse(fs.readFileSync(beatsPath, 'utf-8'));
  const beats = data.hits
    .filter(h => h.expectTrigger !== false)
    .map(h => h.time);

  if (beats.length < 3) {
    console.log(`${name.padEnd(35)} TOO FEW BEATS (${beats.length}) — SKIPPED`);
    continue;
  }

  // Find densest window (1-second resolution)
  const maxStart = Math.max(0, Math.floor(beats[beats.length - 1]) - windowSec);
  let bestStart = 0;
  let bestCount = 0;

  for (let start = 0; start <= maxStart; start++) {
    const count = beats.filter(b => b >= start && b < start + windowSec).length;
    if (count > bestCount) {
      bestCount = count;
      bestStart = start;
    }
  }

  // Calculate BPM in the best window
  const windowBeats = beats.filter(b => b >= bestStart && b < bestStart + windowSec);
  let windowBpm = 0;
  if (windowBeats.length >= 3) {
    const ibis = [];
    for (let i = 1; i < windowBeats.length; i++) ibis.push(windowBeats[i] - windowBeats[i - 1]);
    windowBpm = 60.0 / (ibis.reduce((a, b) => a + b) / ibis.length);
  }

  const valid = bestCount >= minBeatsInWindow;
  const entry = {
    file: trackFile,
    duration: duration,
    seekOffset: bestStart,
    windowEnd: bestStart + windowSec,
    beatsInWindow: bestCount,
    windowBpm: Math.round(windowBpm * 10) / 10,
    groundTruthBpm: data.bpm || windowBpm,
    valid: valid,
  };
  manifest[name] = entry;

  const status = valid ? '' : ' *** LOW DENSITY';
  console.log(
    name.padEnd(35) +
    (duration ? `${duration.toFixed(0)}s` : '?').padEnd(10) +
    `${bestStart}-${bestStart + windowSec}s`.padEnd(15) +
    `${bestCount}`.padEnd(8) +
    `${windowBpm.toFixed(0)}`.padEnd(8) +
    `${bestStart}s${status}`
  );
}

// Write manifest
const outPath = path.join(musicDir, 'track_manifest.json');
fs.writeFileSync(outPath, JSON.stringify(manifest, null, 2));
console.log(`\nManifest written to ${outPath} (${Object.keys(manifest).length} tracks)`);

// Summary
const valid = Object.values(manifest).filter(e => e.valid);
const invalid = Object.values(manifest).filter(e => !e.valid);
console.log(`Valid: ${valid.length}, Insufficient density: ${invalid.length}`);
if (invalid.length > 0) {
  console.log(`Skipped tracks: ${invalid.map(e => e.file).join(', ')}`);
}
