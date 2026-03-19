#!/usr/bin/env node
/**
 * Pattern Memory Test Suite
 *
 * Plays full tracks through real speakers, collects pattern memory telemetry
 * from firmware via serial (stream debug + json pattern polling), and analyzes
 * against ground truth derived from beat/onset labels.
 *
 * Test metrics:
 *   1. IOI precision: |ioiBpm - gtBpm| < 5 BPM (octave-tolerant)
 *   2. Cold start: bars until pc > 0.3, should be ≤ 4
 *   3. Fill tolerance: min pc during fill windows ≥ 0.2
 *   4. Cache save: cache entry count increases when pc crosses 0.6
 *   5. Cache restore: bars until pc > 0.4 after gap/section-change ≤ 8
 *   6. Template stability: % snapshots with same tmpl during steady sections > 70%
 *
 * Usage:
 *   cd blinky-test-player
 *   NODE_PATH=node_modules node ../ml-training/tools/pattern_memory_test.cjs \
 *       --ports /dev/ttyACM0 --music-dir music/edm
 *
 *   # Subset:
 *   NODE_PATH=node_modules node ../ml-training/tools/pattern_memory_test.cjs \
 *       --ports /dev/ttyACM0 --tracks dnb-energetic-breakbeat,techno-minimal-emotion
 */

const { SerialPort } = require('serialport');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

// ============================================================================
// CLI argument parsing
// ============================================================================

const args = process.argv.slice(2);
function getArg(name, defaultValue) {
    const idx = args.indexOf(name);
    if (idx === -1 || idx + 1 >= args.length) return defaultValue;
    return args[idx + 1];
}

const portsArg = getArg('--ports', '/dev/ttyACM0');
const portPaths = portsArg.split(',').map(s => s.trim());
const musicDir = getArg('--music-dir', 'music/edm');
const tracksArg = getArg('--tracks', null);

// Default test tracks (8 tracks, ~18 min total)
const DEFAULT_TRACKS = [
    'techno-minimal-emotion',
    'amapiano-vibez',
    'dnb-energetic-breakbeat',
    'dubstep-edm-halftime',
    'techno-dub-groove',
    'reggaeton-fuego-lento',
    'techno-minimal-01',
    'breakbeat-background',
];

// Which tracks test which metrics
const COLD_START_TRACKS = ['techno-minimal-emotion', 'amapiano-vibez', 'breakbeat-background'];
const FILL_TRACKS = ['dubstep-edm-halftime', 'reggaeton-fuego-lento', 'techno-dub-groove'];
const CACHE_RESTORE_TRACKS = ['dnb-energetic-breakbeat', 'amapiano-vibez'];
const TEMPLATE_TRACKS = ['techno-minimal-emotion', 'breakbeat-background', 'reggaeton-fuego-lento'];

// Template name lookup
const TEMPLATE_NAMES = ['4otf', 'backbeat', 'halftime', 'breakbeat', '8thnote', 'dnb', 'dembow', 'sparse'];

// Audio lock
const AUDIO_LOCK = '/tmp/blinky-audio.lock';

// ============================================================================
// Utilities
// ============================================================================

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

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
            } catch (readErr) {
                console.error(`\nERROR: Audio lock exists at ${AUDIO_LOCK}.`);
            }
            return false;
        }
        throw e;
    }
}

function releaseAudioLock() {
    try { fs.unlinkSync(AUDIO_LOCK); } catch (e) { /* ignore */ }
}

function loadManifest() {
    const manifestPath = path.join(musicDir, 'track_manifest.json');
    if (!fs.existsSync(manifestPath)) {
        console.error(`ERROR: No track manifest at ${manifestPath}`);
        process.exit(1);
    }
    return JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
}

function loadGroundTruth(trackName) {
    const gtPath = path.join(musicDir, `${trackName}.pattern_gt.json`);
    if (!fs.existsSync(gtPath)) return null;
    return JSON.parse(fs.readFileSync(gtPath, 'utf-8'));
}

// ============================================================================
// Serial port management
// ============================================================================

async function openPort(portPath) {
    const port = new SerialPort({ path: portPath, baudRate: 115200 });
    await new Promise((resolve, reject) => {
        port.on('open', resolve);
        port.on('error', reject);
    });
    await sleep(500);
    return port;
}

function sendCommand(port, cmd) {
    return new Promise((resolve) => {
        port.write(cmd + '\n');
        setTimeout(resolve, 200);
    });
}

/**
 * Send command and wait for a JSON response line (with timeout).
 */
function sendAndReceive(port, cmd, timeoutMs = 2000) {
    return new Promise((resolve) => {
        let buf = '';
        const timer = setTimeout(() => {
            port.removeListener('data', onData);
            resolve(null);
        }, timeoutMs);

        function onData(d) {
            buf += d.toString();
            const lines = buf.split('\n');
            buf = lines.pop();
            for (const line of lines) {
                const trimmed = line.trim();
                if (trimmed.startsWith('{')) {
                    try {
                        const obj = JSON.parse(trimmed);
                        // Distinguish pattern response (has "pc" key, no "m" key)
                        if ('pc' in obj && !('m' in obj)) {
                            clearTimeout(timer);
                            port.removeListener('data', onData);
                            resolve(obj);
                            return;
                        }
                    } catch (e) { /* not valid JSON */ }
                }
            }
        }
        port.on('data', onData);
        port.write(cmd + '\n');
    });
}

// ============================================================================
// Track playback + data collection
// ============================================================================

async function runTrackTest(port, trackName, manifest, groundTruth) {
    const entry = manifest[trackName];
    const trackPath = path.join(musicDir, entry.file);
    const duration = entry.duration;
    const gtBpm = entry.groundTruthBpm;

    // Data arrays
    const streamReadings = [];
    const patternSnapshots = [];

    // 1. Reset pattern memory
    await sendCommand(port, 'reset pattern');
    await sleep(500);

    // 2. Start debug stream
    await sendCommand(port, 'stream debug');
    await sleep(200);

    // 3. Set up stream data handler
    let buf = '';
    const playbackStart = Date.now();

    function onData(d) {
        buf += d.toString();
        const lines = buf.split('\n');
        buf = lines.pop();
        for (const line of lines) {
            const trimmed = line.trim();
            if (!trimmed.startsWith('{')) continue;
            try {
                const obj = JSON.parse(trimmed);
                const trackTime = (Date.now() - playbackStart) / 1000.0;

                // Stream line (has "m" key with music telemetry)
                if (obj.m) {
                    streamReadings.push({
                        trackTime,
                        bpm: obj.m.bpm || 0,
                        pc: obj.m.pc || 0,
                        ic: obj.m.ic || 0,
                        ib: obj.m.ib || 0,
                        be: obj.m.be || 0,
                        pulse: obj.m.p || 0,
                        energy: obj.m.e || 0,
                    });
                }

                // Pattern snapshot response (has "pc" key, no "m" key)
                if ('pc' in obj && !('m' in obj)) {
                    patternSnapshots.push({
                        trackTime,
                        ...obj,
                    });
                }
            } catch (e) { /* skip */ }
        }
    }

    port.on('data', onData);

    // 4. Spawn ffplay (full track from t=0, -autoexit)
    const ffplay = spawn('ffplay', [
        '-nodisp', '-autoexit', '-loglevel', 'quiet',
        trackPath,
    ]);

    // 5. Poll json pattern every 2s during playback
    const pollInterval = setInterval(() => {
        port.write('json pattern\n');
    }, 2000);

    // 6. Wait for ffplay to exit (or timeout at duration + 5s)
    await new Promise((resolve) => {
        const maxTimeout = setTimeout(() => {
            ffplay.kill('SIGTERM');
            resolve();
        }, (duration + 5) * 1000);

        ffplay.on('close', () => {
            clearTimeout(maxTimeout);
            resolve();
        });
    });

    // 7. Cleanup
    clearInterval(pollInterval);
    await sleep(1000);
    await sendCommand(port, 'stream off');

    // Final pattern snapshot
    const finalSnapshot = await sendAndReceive(port, 'json pattern');
    if (finalSnapshot) {
        patternSnapshots.push({
            trackTime: (Date.now() - playbackStart) / 1000.0,
            ...finalSnapshot,
        });
    }

    port.removeListener('data', onData);
    await sleep(500);

    return { streamReadings, patternSnapshots, gtBpm, duration };
}

// ============================================================================
// Analysis
// ============================================================================

function analyzeIOI(data) {
    // Median |ib - gtBpm| after 10s settle (octave-tolerant)
    const settleTime = 10.0;
    const settled = data.streamReadings.filter(r => r.trackTime > settleTime && r.ib > 0);
    if (settled.length === 0) return { error: Infinity, pass: false, readings: 0 };

    const errors = settled.map(r => {
        const ib = r.ib;
        const gt = data.gtBpm;
        return Math.min(
            Math.abs(ib - gt),
            Math.abs(ib - gt * 2),
            Math.abs(ib - gt / 2),
        );
    });
    errors.sort((a, b) => a - b);
    const median = errors[Math.floor(errors.length / 2)];

    // Also get the median ib for display
    const ibs = settled.map(r => r.ib).sort((a, b) => a - b);
    const medianIb = ibs[Math.floor(ibs.length / 2)];

    return {
        medianError: Math.round(median * 10) / 10,
        medianIb: Math.round(medianIb * 10) / 10,
        pass: median < 5.0,
        readings: settled.length,
    };
}

function analyzeColdStart(data) {
    // Bars until pc > 0.3 (from snapshots)
    const snapshots = data.patternSnapshots;
    for (const s of snapshots) {
        if (s.pc > 0.3) {
            return { bars: s.bars || 0, pass: (s.bars || 0) <= 4 };
        }
    }
    // Never crossed 0.3
    const lastBars = snapshots.length > 0 ? snapshots[snapshots.length - 1].bars : 0;
    return { bars: lastBars, pass: false };
}

function analyzeFillTolerance(data, groundTruth) {
    if (!groundTruth || !groundTruth.fills || groundTruth.fills.length === 0) {
        return { minPc: 1.0, pass: true, fillCount: 0 };
    }

    // Find min pc during fill bar time windows
    let minPc = 1.0;
    for (const fill of groundTruth.fills) {
        const fillStart = fill.startTime;
        const fillEnd = fill.endTime;
        // Look at stream readings during fill window
        const fillReadings = data.streamReadings.filter(
            r => r.trackTime >= fillStart && r.trackTime <= fillEnd + 2.0
        );
        for (const r of fillReadings) {
            if (r.pc < minPc) minPc = r.pc;
        }
    }

    return {
        minPc: Math.round(minPc * 100) / 100,
        pass: minPc >= 0.2,
        fillCount: groundTruth.fills.length,
    };
}

function analyzeCacheSave(data) {
    // Cache entry count should increase at some point when pc > 0.6
    const snapshots = data.patternSnapshots;
    let maxCache = 0;
    let pcReached06 = false;
    for (const s of snapshots) {
        if (s.pc > 0.6) pcReached06 = true;
        if (s.cache > maxCache) maxCache = s.cache;
    }

    return {
        maxCacheEntries: maxCache,
        pass: !pcReached06 || maxCache >= 1,  // pass if pc never reached 0.6 (N/A) or cache saved
        pcReached06,
    };
}

function analyzeCacheRestore(data) {
    // After gap/section-change where pc < 0.3: bars until pc > 0.4
    const snapshots = data.patternSnapshots;
    let droppedBelow03 = false;
    let barsAtDrop = 0;

    for (const s of snapshots) {
        if (s.pc < 0.3 && !droppedBelow03) {
            droppedBelow03 = true;
            barsAtDrop = s.bars || 0;
        }
        if (droppedBelow03 && s.pc > 0.4) {
            const barsToRecover = (s.bars || 0) - barsAtDrop;
            return { barsToRecover, pass: barsToRecover <= 8, triggered: true };
        }
    }

    return { barsToRecover: -1, pass: true, triggered: false };  // N/A
}

function analyzeTemplateStability(data) {
    // % of snapshots with same tmpl during steady 8+ bar sections
    const snapshots = data.patternSnapshots.filter(s => (s.bars || 0) >= 8);
    if (snapshots.length === 0) return { stability: 0, pass: false, dominantTemplate: -1 };

    // Count template occurrences
    const counts = {};
    for (const s of snapshots) {
        const t = s.tmpl;
        counts[t] = (counts[t] || 0) + 1;
    }

    // Find dominant template
    let maxCount = 0;
    let dominant = -1;
    for (const [t, c] of Object.entries(counts)) {
        if (c > maxCount) { maxCount = c; dominant = parseInt(t); }
    }

    const stability = maxCount / snapshots.length;
    return {
        stability: Math.round(stability * 100),
        pass: stability > 0.70,
        dominantTemplate: dominant,
        templateName: dominant >= 0 && dominant < TEMPLATE_NAMES.length ? TEMPLATE_NAMES[dominant] : 'none',
    };
}

// ============================================================================
// Main
// ============================================================================

async function main() {
    // Acquire audio lock
    if (!acquireAudioLock()) {
        process.exit(1);
    }
    process.on('exit', releaseAudioLock);
    process.on('SIGINT', () => { releaseAudioLock(); process.exit(130); });
    process.on('SIGTERM', () => { releaseAudioLock(); process.exit(143); });

    // Load manifest
    const manifest = loadManifest();

    // Select tracks
    let trackList;
    if (tracksArg) {
        trackList = tracksArg.split(',').map(s => s.trim());
    } else {
        trackList = DEFAULT_TRACKS.filter(t => manifest[t]);
    }

    console.log(`\n=== Pattern Memory Test Suite ===`);
    console.log(`Devices: ${portPaths.length}, Tracks: ${trackList.length}`);
    console.log(`Music dir: ${musicDir}\n`);

    // Open port(s)
    const ports = [];
    for (const pp of portPaths) {
        try {
            const port = await openPort(pp);
            ports.push({ path: pp, port });
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
    console.log();

    const allResults = [];
    let passCount = 0;
    let totalTests = 0;

    for (let ti = 0; ti < trackList.length; ti++) {
        const trackName = trackList[ti];
        const entry = manifest[trackName];
        if (!entry) {
            console.log(`  SKIP ${trackName}: not in manifest`);
            continue;
        }

        const gtBpm = entry.groundTruthBpm;
        const dur = Math.round(entry.duration);
        const groundTruth = loadGroundTruth(trackName);

        console.log(`[${ti + 1}/${trackList.length}] ${trackName} (${gtBpm} BPM, ${dur}s)`);

        // Run test on first port (single device for now)
        const { port } = ports[0];
        const data = await runTrackTest(port, trackName, manifest, groundTruth);

        // Analyze
        const ioi = analyzeIOI(data);
        const coldStart = analyzeColdStart(data);
        const fill = analyzeFillTolerance(data, groundTruth);
        const cacheSave = analyzeCacheSave(data);
        const cacheRestore = analyzeCacheRestore(data);
        const template = analyzeTemplateStability(data);

        // Display results
        let line = '  ';
        line += `IOI: ${ioi.medianIb} BPM (err=${ioi.medianError}) ${ioi.pass ? '✓' : '✗'}`;

        if (COLD_START_TRACKS.includes(trackName)) {
            line += `  Cold: ${coldStart.bars} bars ${coldStart.pass ? '✓' : '✗'}`;
            totalTests++;
            if (coldStart.pass) passCount++;
        }

        if (FILL_TRACKS.includes(trackName)) {
            line += `  Fill: min pc=${fill.minPc} ${fill.pass ? '✓' : '✗'}`;
            totalTests++;
            if (fill.pass) passCount++;
        }

        if (TEMPLATE_TRACKS.includes(trackName)) {
            line += `  Template: ${template.templateName} (${template.stability}% stable) ${template.pass ? '✓' : '✗'}`;
            totalTests++;
            if (template.pass) passCount++;
        }

        line += `  Cache: ${cacheSave.maxCacheEntries} saves`;

        if (CACHE_RESTORE_TRACKS.includes(trackName)) {
            if (cacheRestore.triggered) {
                line += `, restore ${cacheRestore.barsToRecover} bars ${cacheRestore.pass ? '✓' : '✗'}`;
                totalTests++;
                if (cacheRestore.pass) passCount++;
            } else {
                line += `, restore N/A`;
            }
        }

        // IOI and cache save count for all tracks
        totalTests++;  // IOI
        if (ioi.pass) passCount++;
        if (cacheSave.pcReached06) {
            totalTests++;  // Cache save
            if (cacheSave.pass) passCount++;
        }

        console.log(line);

        allResults.push({
            track: trackName,
            gtBpm,
            duration: dur,
            ioi,
            coldStart,
            fill,
            cacheSave,
            cacheRestore,
            template,
            streamReadingCount: data.streamReadings.length,
            snapshotCount: data.patternSnapshots.length,
            streamReadings: data.streamReadings,
            patternSnapshots: data.patternSnapshots,
        });
    }

    // Summary
    console.log(`\n=== Summary ===`);
    console.log(`Tests passed: ${passCount}/${totalTests} (${Math.round(passCount / totalTests * 100)}%)`);

    // Write results JSON
    const resultsDir = path.join(musicDir, '..', '..', 'tuning-results');
    if (!fs.existsSync(resultsDir)) {
        fs.mkdirSync(resultsDir, { recursive: true });
    }
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const resultsPath = path.join(resultsDir, `pattern-memory-${timestamp}.json`);

    // Strip raw readings from the saved results to keep file manageable
    const savedResults = allResults.map(r => ({
        ...r,
        streamReadings: undefined,
        patternSnapshots: undefined,
        streamReadingSummary: {
            count: r.streamReadingCount,
            firstTime: r.streamReadings.length > 0 ? r.streamReadings[0].trackTime : null,
            lastTime: r.streamReadings.length > 0 ? r.streamReadings[r.streamReadings.length - 1].trackTime : null,
        },
        snapshotSummary: {
            count: r.snapshotCount,
            snapshots: r.patternSnapshots,
        },
    }));

    fs.writeFileSync(resultsPath, JSON.stringify({
        timestamp: new Date().toISOString(),
        ports: portPaths,
        tracks: trackList,
        passRate: `${passCount}/${totalTests}`,
        results: savedResults,
    }, null, 2));

    console.log(`Results: ${resultsPath}\n`);

    // Close ports
    for (const { port: p } of ports) {
        p.close();
    }

    releaseAudioLock();
    process.exit(passCount === totalTests ? 0 : 1);
}

main().catch(e => {
    console.error('Fatal error:', e);
    releaseAudioLock();
    process.exit(2);
});
