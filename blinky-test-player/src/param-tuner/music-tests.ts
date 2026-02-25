/**
 * Music test file discovery
 *
 * Scans the music/ directory for audio files with matching .beats.json ground truth.
 */

import { readdirSync, existsSync } from 'fs';
import { join, basename } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';
import type { MusicTestFile } from './multi-device-runner.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const PROJECT_ROOT = join(__dirname, '..', '..');
const MUSIC_DIR = join(PROJECT_ROOT, 'music');

const AUDIO_EXTENSIONS = ['.mp3', '.wav', '.ogg', '.flac'];

/**
 * Discover all music test files (audio + ground truth pairs) in the music/ directory.
 */
export function discoverMusicTests(): MusicTestFile[] {
  const tests: MusicTestFile[] = [];

  if (!existsSync(MUSIC_DIR)) {
    return tests;
  }

  // Scan subdirectories (e.g., music/edm/)
  for (const subdir of readdirSync(MUSIC_DIR, { withFileTypes: true })) {
    if (!subdir.isDirectory()) continue;

    const subdirPath = join(MUSIC_DIR, subdir.name);

    for (const file of readdirSync(subdirPath)) {
      const ext = AUDIO_EXTENSIONS.find(e => file.endsWith(e));
      if (!ext) continue;

      const id = basename(file, ext);
      const groundTruthFile = join(subdirPath, `${id}.beats.json`);

      if (existsSync(groundTruthFile)) {
        tests.push({
          id,
          audioFile: join(subdirPath, file),
          groundTruthFile,
        });
      }
    }
  }

  return tests.sort((a, b) => a.id.localeCompare(b.id));
}
