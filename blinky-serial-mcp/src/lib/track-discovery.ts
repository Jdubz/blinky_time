/**
 * Track discovery for music test validation suites.
 */

import { existsSync, readdirSync } from 'fs';
import { join } from 'path';

/** Discover audio tracks with matching ground truth annotations in a directory. */
export function discoverTracks(dir: string): Array<{ name: string; audioFile: string; groundTruth: string; onsetGroundTruth?: string }> {
  if (!existsSync(dir)) throw new Error(`Track directory does not exist: ${dir}`);
  const files = readdirSync(dir);
  const tracks: Array<{ name: string; audioFile: string; groundTruth: string; onsetGroundTruth?: string }> = [];
  for (const f of files) {
    if (f.endsWith('.mp3') || f.endsWith('.wav') || f.endsWith('.flac')) {
      const base = f.replace(/\.(mp3|wav|flac)$/, '');
      const gtFile = `${base}.beats.json`;
      if (files.includes(gtFile)) {
        const onsetFile = `${base}.onsets_consensus.json`;
        tracks.push({
          name: base,
          audioFile: join(dir, f),
          groundTruth: join(dir, gtFile),
          onsetGroundTruth: files.includes(onsetFile) ? join(dir, onsetFile) : undefined,
        });
      }
    }
  }
  return tracks.sort((a, b) => a.name.localeCompare(b.name));
}
