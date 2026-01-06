/**
 * blinky-simulator - LED effect visualization library
 *
 * Exports all components for programmatic use.
 */

export * from './types';
export * from './generators';
export * from './effects';
export * from './audioPatterns';
export * from './renderer';
export * from './gifEncoder';

import { createGenerator, GeneratorType } from './generators';
import { createEffect, EffectType, HueRotationEffect } from './effects';
import { createPattern, PatternType } from './audioPatterns';
import { LEDRenderer } from './renderer';
import { GifWriter } from './gifEncoder';
import { DEVICE_CONFIGS, DeviceConfig } from './types';

export interface RenderOptions {
  generator: GeneratorType;
  effect?: EffectType;
  pattern?: PatternType | string;
  device?: string;
  durationMs?: number;
  fps?: number;
  ledSize?: number;
  hueShift?: number;
  outputPath: string;
}

/**
 * Render an LED effect preview to a GIF file
 */
export async function renderPreview(options: RenderOptions): Promise<{ success: boolean; path: string; frames: number; size: number }> {
  const {
    generator: genType,
    effect: effectType = 'none',
    pattern: patternType = 'steady-120bpm',
    device = 'tube',
    durationMs = 3000,
    fps = 30,
    ledSize = 16,
    hueShift = 0,
    outputPath
  } = options;

  // Get device config
  const deviceConfig: DeviceConfig = DEVICE_CONFIGS[device] || DEVICE_CONFIGS.tube;

  // Create generator
  const generator = createGenerator(genType);
  generator.begin(deviceConfig.width, deviceConfig.height);

  // Create effect
  const effect = createEffect(effectType);
  effect.begin(deviceConfig.width, deviceConfig.height);

  if (effectType === 'hue' && hueShift !== 0) {
    (effect as HueRotationEffect).setParams({ hueShift });
  }

  // Create audio pattern
  const pattern = createPattern(patternType as PatternType, durationMs);

  // Create renderer
  const renderer = new LEDRenderer({ ledSize });
  renderer.configure(deviceConfig.width, deviceConfig.height);

  // Create GIF encoder
  const gif = new GifWriter(renderer.getWidth(), renderer.getHeight(), fps);

  // Calculate frames
  const frameIntervalMs = Math.round(1000 / fps);
  const totalFrames = Math.floor(durationMs / frameIntervalMs);

  // Render frames
  for (let frame = 0; frame < totalFrames; frame++) {
    const timeMs = frame * frameIntervalMs;

    // Get audio state
    const audio = pattern.getAudioAt(timeMs);

    // Update generator
    generator.update(audio, timeMs);

    // Get matrix
    const matrix = generator.getMatrix();

    // Apply effect
    effect.apply(matrix);

    // Render to image
    renderer.render(matrix);

    // Add to GIF
    gif.addFrame(renderer.getBuffer());
  }

  // Save GIF
  const path = require('path');
  const fs = require('fs');

  const absPath = path.resolve(outputPath);
  gif.save(absPath);

  // Get file size
  const stats = fs.statSync(absPath);

  return {
    success: true,
    path: absPath,
    frames: totalFrames,
    size: stats.size
  };
}
