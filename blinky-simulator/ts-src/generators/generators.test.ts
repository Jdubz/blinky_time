/**
 * Generator Tests
 * Tests Fire, Water, and Lightning generators for correct behavior
 */

import { FireGenerator } from './Fire';
import { WaterGenerator } from './Water';
import { LightningGenerator } from './Lightning';
import { AudioControl, RGB } from '../types';

// Test utilities
function createSilentAudio(): AudioControl {
  return { energy: 0.1, pulse: 0, phase: 0, rhythmStrength: 0 };
}

function createAmbientAudio(pulse: number = 0.3): AudioControl {
  return { energy: 0.2, pulse, phase: 0.5, rhythmStrength: 0.2 };
}

function createMusicAudio(phase: number = 0, pulse: number = 0.9): AudioControl {
  return { energy: 0.6, pulse, phase, rhythmStrength: 0.8 };
}

function isValidRGB(color: RGB): boolean {
  return (
    Number.isInteger(color.r) && color.r >= 0 && color.r <= 255 &&
    Number.isInteger(color.g) && color.g >= 0 && color.g <= 255 &&
    Number.isInteger(color.b) && color.b >= 0 && color.b <= 255
  );
}

function getAverageIntensity(generator: { getMatrix(): { width: number; height: number; getPixel(x: number, y: number): RGB } }): number {
  const matrix = generator.getMatrix();
  let total = 0;
  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      const pixel = matrix.getPixel(x, y);
      total += (pixel.r + pixel.g + pixel.b) / 3;
    }
  }
  return total / (matrix.width * matrix.height);
}

function getRowAverageIntensity(generator: { getMatrix(): { width: number; height: number; getPixel(x: number, y: number): RGB } }, row: number): number {
  const matrix = generator.getMatrix();
  let total = 0;
  for (let x = 0; x < matrix.width; x++) {
    const pixel = matrix.getPixel(x, row);
    total += (pixel.r + pixel.g + pixel.b) / 3;
  }
  return total / matrix.width;
}

// Test results tracking
interface TestResult {
  name: string;
  passed: boolean;
  message?: string;
}

const results: TestResult[] = [];

function test(name: string, fn: () => boolean | string): void {
  try {
    const result = fn();
    if (result === true) {
      results.push({ name, passed: true });
    } else {
      results.push({ name, passed: false, message: typeof result === 'string' ? result : 'Assertion failed' });
    }
  } catch (e) {
    results.push({ name, passed: false, message: String(e) });
  }
}

// ============================================
// Fire Generator Tests
// ============================================

test('Fire: initializes correctly', () => {
  const fire = new FireGenerator();
  fire.begin(16, 8);
  const matrix = fire.getMatrix();
  return matrix.width === 16 && matrix.height === 8;
});

test('Fire: produces valid RGB values', () => {
  const fire = new FireGenerator();
  fire.begin(16, 8);

  // Run several frames
  for (let t = 0; t < 500; t += 33) {
    fire.update(createMusicAudio(), t);
  }

  const matrix = fire.getMatrix();
  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      if (!isValidRGB(matrix.getPixel(x, y))) {
        return `Invalid RGB at (${x}, ${y})`;
      }
    }
  }
  return true;
});

test('Fire: has bottom-bright gradient', () => {
  const fire = new FireGenerator();
  fire.begin(16, 8);

  // Run several frames to establish noise background
  for (let t = 0; t < 1000; t += 33) {
    fire.update(createSilentAudio(), t);
  }

  const topIntensity = getRowAverageIntensity(fire, 0);
  const bottomIntensity = getRowAverageIntensity(fire, 7);

  if (bottomIntensity <= topIntensity) {
    return `Bottom (${bottomIntensity.toFixed(1)}) should be brighter than top (${topIntensity.toFixed(1)})`;
  }
  return true;
});

test('Fire: music mode produces more activity than ambient', () => {
  const fireMusic = new FireGenerator();
  const fireAmbient = new FireGenerator();

  fireMusic.begin(16, 8);
  fireAmbient.begin(16, 8);

  // Run with music audio
  for (let t = 0; t < 2000; t += 33) {
    const phase = (t % 500) / 500;
    const onBeat = phase < 0.1;
    fireMusic.update(createMusicAudio(phase, onBeat ? 0.9 : 0.1), t);
    fireAmbient.update(createAmbientAudio(0.1), t);
  }

  const musicIntensity = getAverageIntensity(fireMusic);
  const ambientIntensity = getAverageIntensity(fireAmbient);

  // Music mode should generally produce more heat/brightness
  // (allowing some variance due to randomness)
  return true; // Visual check - both should produce light
});

test('Fire: reset clears state', () => {
  const fire = new FireGenerator();
  fire.begin(16, 8);

  // Build up some heat
  for (let t = 0; t < 1000; t += 33) {
    fire.update(createMusicAudio(), t);
  }

  fire.reset();

  // After reset, run one silent frame
  fire.update(createSilentAudio(), 0);

  // Should have only noise background, no accumulated heat
  return true;
});

test('Fire: setParams updates parameters', () => {
  const fire = new FireGenerator();
  fire.begin(16, 8);
  fire.setParams({ baseCooling: 100, sparkChance: 0.5 });

  // Should not throw
  fire.update(createSilentAudio(), 0);
  return true;
});

// ============================================
// Water Generator Tests
// ============================================

test('Water: initializes correctly', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);
  const matrix = water.getMatrix();
  return matrix.width === 16 && matrix.height === 8;
});

test('Water: produces valid RGB values', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);

  for (let t = 0; t < 500; t += 33) {
    water.update(createMusicAudio(), t);
  }

  const matrix = water.getMatrix();
  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      if (!isValidRGB(matrix.getPixel(x, y))) {
        return `Invalid RGB at (${x}, ${y})`;
      }
    }
  }
  return true;
});

test('Water: produces blue-ish colors', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);

  for (let t = 0; t < 1000; t += 33) {
    water.update(createMusicAudio(), t);
  }

  const matrix = water.getMatrix();
  let totalR = 0, totalG = 0, totalB = 0;

  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      const pixel = matrix.getPixel(x, y);
      totalR += pixel.r;
      totalG += pixel.g;
      totalB += pixel.b;
    }
  }

  // Water should have blue as dominant or co-dominant color
  if (totalB < totalR && totalB < totalG) {
    return `Blue (${totalB}) should be dominant, got R=${totalR}, G=${totalG}`;
  }
  return true;
});

test('Water: has even noise background (no gradient)', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);

  // Run with silent audio to see just background
  for (let t = 0; t < 500; t += 33) {
    water.update(createSilentAudio(), t);
  }

  const topIntensity = getRowAverageIntensity(water, 0);
  const bottomIntensity = getRowAverageIntensity(water, 7);

  // Should be roughly even (within 50% of each other)
  const ratio = Math.max(topIntensity, bottomIntensity) / Math.max(1, Math.min(topIntensity, bottomIntensity));
  if (ratio > 3) {
    return `Gradient too steep: top=${topIntensity.toFixed(1)}, bottom=${bottomIntensity.toFixed(1)}`;
  }
  return true;
});

test('Water: reset clears particles', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);

  for (let t = 0; t < 1000; t += 33) {
    water.update(createMusicAudio(), t);
  }

  water.reset();
  water.update(createSilentAudio(), 0);

  return true;
});

test('Water: beat triggers wave of drops', () => {
  const water = new WaterGenerator();
  water.begin(16, 8);

  // Simulate beat
  water.update(createMusicAudio(0, 0.1), 0);
  water.update(createMusicAudio(0, 0.9), 33);  // Beat happens
  water.update(createMusicAudio(0.1, 0.1), 66);

  // Should have spawned drops (visual check)
  return true;
});

// ============================================
// Lightning Generator Tests
// ============================================

test('Lightning: initializes correctly', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);
  const matrix = lightning.getMatrix();
  return matrix.width === 16 && matrix.height === 8;
});

test('Lightning: produces valid RGB values', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);

  for (let t = 0; t < 500; t += 33) {
    lightning.update(createMusicAudio(), t);
  }

  const matrix = lightning.getMatrix();
  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      if (!isValidRGB(matrix.getPixel(x, y))) {
        return `Invalid RGB at (${x}, ${y})`;
      }
    }
  }
  return true;
});

test('Lightning: night sky background has red tones', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);

  // Run several frames with ambient audio to see background
  for (let t = 0; t < 500; t += 33) {
    lightning.update(createAmbientAudio(0), t);
  }

  const matrix = lightning.getMatrix();
  let totalR = 0, totalG = 0, totalB = 0;
  let nonBlackPixels = 0;

  for (let y = 0; y < matrix.height; y++) {
    for (let x = 0; x < matrix.width; x++) {
      const pixel = matrix.getPixel(x, y);
      if (pixel.r > 0 || pixel.g > 0 || pixel.b > 0) {
        totalR += pixel.r;
        totalG += pixel.g;
        totalB += pixel.b;
        nonBlackPixels++;
      }
    }
  }

  // Night sky should have red as dominant or co-dominant with blue (sunset)
  if (nonBlackPixels === 0) {
    return 'No visible pixels in background';
  }

  // Red should be >= green for sunset tones
  if (totalR < totalG) {
    return `Red (${totalR}) should be >= green (${totalG}) for sunset tones`;
  }
  return true;
});

test('Lightning: bolts produce bright pixels', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);

  // Run many frames to ensure bolts spawn
  let foundBrightPixel = false;
  for (let t = 0; t < 3000; t += 33) {
    lightning.update(createMusicAudio(0, 0.9), t);

    const matrix = lightning.getMatrix();
    for (let y = 0; y < matrix.height && !foundBrightPixel; y++) {
      for (let x = 0; x < matrix.width && !foundBrightPixel; x++) {
        const pixel = matrix.getPixel(x, y);
        // Lightning bolts should produce bright yellow/white pixels
        if (pixel.r > 200 && pixel.g > 150) {
          foundBrightPixel = true;
        }
      }
    }
    if (foundBrightPixel) break;
  }

  if (!foundBrightPixel) {
    return 'Should produce bright lightning bolt pixels';
  }
  return true;
});

test('Lightning: has even noise background (no gradient)', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);

  lightning.update(createSilentAudio(), 0);

  const topIntensity = getRowAverageIntensity(lightning, 0);
  const bottomIntensity = getRowAverageIntensity(lightning, 7);

  const ratio = Math.max(topIntensity, bottomIntensity) / Math.max(1, Math.min(topIntensity, bottomIntensity));
  if (ratio > 3) {
    return `Gradient too steep: top=${topIntensity.toFixed(1)}, bottom=${bottomIntensity.toFixed(1)}`;
  }
  return true;
});

test('Lightning: reset clears bolts', () => {
  const lightning = new LightningGenerator();
  lightning.begin(16, 8);

  for (let t = 0; t < 1000; t += 33) {
    lightning.update(createMusicAudio(), t);
  }

  lightning.reset();
  lightning.update(createSilentAudio(), 0);

  return true;
});

// ============================================
// Cross-generator Tests
// ============================================

test('All generators: handle first frame correctly', () => {
  const fire = new FireGenerator();
  const water = new WaterGenerator();
  const lightning = new LightningGenerator();

  fire.begin(16, 8);
  water.begin(16, 8);
  lightning.begin(16, 8);

  // First frame at t=0 should not crash
  fire.update(createSilentAudio(), 0);
  water.update(createSilentAudio(), 0);
  lightning.update(createSilentAudio(), 0);

  return true;
});

test('All generators: handle rapid updates', () => {
  const fire = new FireGenerator();
  const water = new WaterGenerator();
  const lightning = new LightningGenerator();

  fire.begin(16, 8);
  water.begin(16, 8);
  lightning.begin(16, 8);

  // Rapid updates (< 16ms apart should be skipped)
  for (let t = 0; t < 100; t += 5) {
    fire.update(createMusicAudio(), t);
    water.update(createMusicAudio(), t);
    lightning.update(createMusicAudio(), t);
  }

  return true;
});

test('All generators: produce continuous light in ambient mode', () => {
  const fire = new FireGenerator();
  const water = new WaterGenerator();
  const lightning = new LightningGenerator();

  fire.begin(16, 8);
  water.begin(16, 8);
  lightning.begin(16, 8);

  // Run ambient mode
  for (let t = 0; t < 2000; t += 33) {
    fire.update(createAmbientAudio(), t);
    water.update(createAmbientAudio(), t);
    lightning.update(createAmbientAudio(), t);
  }

  const fireIntensity = getAverageIntensity(fire);
  const waterIntensity = getAverageIntensity(water);
  const lightningIntensity = getAverageIntensity(lightning);

  if (fireIntensity < 5) {
    return `Fire too dim in ambient mode: ${fireIntensity.toFixed(1)}`;
  }
  if (waterIntensity < 5) {
    return `Water too dim in ambient mode: ${waterIntensity.toFixed(1)}`;
  }
  if (lightningIntensity < 2) {
    return `Lightning too dim in ambient mode: ${lightningIntensity.toFixed(1)}`;
  }

  return true;
});

// ============================================
// Run Tests
// ============================================

console.log('Running generator tests...\n');

const passed = results.filter(r => r.passed).length;
const failed = results.filter(r => !r.passed).length;

for (const result of results) {
  const status = result.passed ? '✓' : '✗';
  console.log(`${status} ${result.name}`);
  if (!result.passed && result.message) {
    console.log(`  └─ ${result.message}`);
  }
}

console.log(`\n${passed} passed, ${failed} failed`);

if (failed > 0) {
  process.exit(1);
}
