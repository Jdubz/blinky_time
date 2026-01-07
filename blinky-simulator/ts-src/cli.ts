#!/usr/bin/env node
/**
 * blinky-simulator CLI
 *
 * Renders LED effects to animated GIF files.
 */

import { Command } from 'commander';
import * as fs from 'fs';
import * as path from 'path';
import { createGenerator, GeneratorType } from './generators';
import { createEffect, EffectType, HueRotationEffect } from './effects';
import { createPattern, PatternType } from './audioPatterns';
import { LEDRenderer } from './renderer';
import { GifWriter } from './gifEncoder';
import { DEVICE_CONFIGS } from './types';

// Output directory for generated previews (gitignored)
const PREVIEWS_DIR = path.join(__dirname, '..', 'previews');

interface SimulatorOptions {
  generator: GeneratorType;
  effect: EffectType;
  pattern: PatternType | string;
  output: string;
  device: string;
  duration: number;
  fps: number;
  ledSize: number;
  hue: number;
  verbose: boolean;
}

function run(options: SimulatorOptions): void {
  const {
    generator: genType,
    effect: effectType,
    pattern: patternType,
    output,
    device,
    duration,
    fps,
    ledSize,
    hue,
    verbose
  } = options;

  // Ensure previews directory exists
  if (!fs.existsSync(PREVIEWS_DIR)) {
    fs.mkdirSync(PREVIEWS_DIR, { recursive: true });
  }

  // Get device config
  const deviceConfig = DEVICE_CONFIGS[device] || DEVICE_CONFIGS.bucket;

  if (verbose) {
    console.log('blinky-simulator v1.0 (Node.js)');
    console.log(`  Generator: ${genType}`);
    console.log(`  Effect: ${effectType}`);
    console.log(`  Pattern: ${patternType}`);
    console.log(`  Device: ${deviceConfig.name} (${deviceConfig.width}x${deviceConfig.height})`);
    console.log(`  Duration: ${duration} ms`);
    console.log(`  FPS: ${fps}`);
    console.log(`  Output: ${output}`);
  }

  // Create generator
  const generator = createGenerator(genType);
  generator.begin(deviceConfig.width, deviceConfig.height);

  // Create effect
  const effect = createEffect(effectType);
  effect.begin(deviceConfig.width, deviceConfig.height);

  if (effectType === 'hue' && hue !== 0) {
    (effect as HueRotationEffect).setParams({ hueShift: hue });
  }

  // Create audio pattern
  const pattern = createPattern(patternType as PatternType, duration);

  if (verbose) {
    console.log(`  Audio pattern: ${pattern.name} (${pattern.duration} ms)`);
  }

  // Create renderer
  const renderer = new LEDRenderer({ ledSize });
  renderer.configure(deviceConfig.width, deviceConfig.height);

  if (verbose) {
    console.log(`  Image size: ${renderer.getWidth()}x${renderer.getHeight()}`);
  }

  // Create GIF encoder
  const gif = new GifWriter(renderer.getWidth(), renderer.getHeight(), fps);

  // Calculate frames
  const frameIntervalMs = Math.round(1000 / fps);
  const totalFrames = Math.floor(duration / frameIntervalMs);

  if (verbose) {
    console.log(`  Rendering ${totalFrames} frames...`);
  }

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

    // Progress
    if (verbose && frame % 30 === 0) {
      const pct = Math.round(100 * frame / totalFrames);
      console.log(`  Frame ${frame}/${totalFrames} (${pct}%)`);
    }
  }

  // Resolve output path (relative paths go to previews/)
  const outputPath = path.isAbsolute(output)
    ? output
    : path.join(PREVIEWS_DIR, output);

  // Save GIF
  gif.save(outputPath);

  // Get file size
  const stats = fs.statSync(outputPath);

  // Show relative path from simulator root
  const relativePath = path.relative(path.join(__dirname, '..'), outputPath);
  console.log(`Created ${relativePath} (${stats.size} bytes, ${totalFrames} frames)`);
}

// CLI setup
const program = new Command();

program
  .name('blinky-simulator')
  .description('LED effect visualization simulator')
  .version('1.0.0');

program
  .option('-g, --generator <type>', 'Generator: fire, water, lightning', 'fire')
  .option('-e, --effect <type>', 'Effect: none, hue', 'none')
  .option('-p, --pattern <type>', 'Audio pattern: steady-120bpm, ambient, silence, burst, complex', 'steady-120bpm')
  .option('-o, --output <file>', 'Output GIF filename (saved to previews/)', 'preview.gif')
  .option('-d, --device <type>', 'Device: tube, hat, bucket', 'bucket')
  .option('-t, --duration <ms>', 'Duration in milliseconds', '3000')
  .option('-f, --fps <num>', 'Frames per second', '30')
  .option('--led-size <pixels>', 'LED circle size in pixels', '16')
  .option('--hue <value>', 'Hue shift for hue effect (0.0-1.0)', '0')
  .option('-v, --verbose', 'Verbose output', false);

program.parse();

const opts = program.opts();

run({
  generator: opts.generator as GeneratorType,
  effect: opts.effect as EffectType,
  pattern: opts.pattern,
  output: opts.output,
  device: opts.device,
  duration: parseInt(opts.duration, 10),
  fps: parseInt(opts.fps, 10),
  ledSize: parseInt(opts.ledSize, 10),
  hue: parseFloat(opts.hue),
  verbose: opts.verbose
});
