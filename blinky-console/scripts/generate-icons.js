#!/usr/bin/env node
/**
 * PWA Icon Generator
 * Generates all required PWA icons from the source SVG favicon
 */

import sharp from 'sharp';
import { readFileSync, writeFileSync, existsSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const PUBLIC_DIR = join(__dirname, '..', 'public');
const SVG_SOURCE = join(PUBLIC_DIR, 'favicon.svg');

// Icon configurations
const ICONS = [
  { name: 'pwa-192x192.png', size: 192 },
  { name: 'pwa-512x512.png', size: 512 },
  { name: 'pwa-maskable-192x192.png', size: 192, maskable: true },
  { name: 'pwa-maskable-512x512.png', size: 512, maskable: true },
  { name: 'apple-touch-icon.png', size: 180 },
  { name: 'favicon-32x32.png', size: 32 },
  { name: 'favicon-16x16.png', size: 16 },
];

// libvips' resampling rounds differently across CPUs (arm64 vs x86_64), so a
// rebuild can differ from the committed PNG by 1 in a few channels. Keep the
// committed file then: a rewrite would dirty the tree, and deploy_local.py
// refuses --dev builds of a dirty tree.
const MAX_CHANNEL_NOISE = 1;

async function writeIfChanged(pngBuffer, outputPath) {
  if (existsSync(outputPath)) {
    const [next, prev] = await Promise.all([
      sharp(pngBuffer).raw().toBuffer({ resolveWithObject: true }),
      sharp(outputPath).raw().toBuffer({ resolveWithObject: true }),
    ]);
    const sameShape =
      next.info.width === prev.info.width &&
      next.info.height === prev.info.height &&
      next.info.channels === prev.info.channels;
    if (sameShape && next.data.every((v, i) => Math.abs(v - prev.data[i]) <= MAX_CHANNEL_NOISE)) {
      return false;
    }
  }
  writeFileSync(outputPath, pngBuffer);
  return true;
}

// Maskable icons need padding (safe zone is 80% of icon, so 10% padding on each side)
const MASKABLE_PADDING_PERCENT = 0.1;

async function generateIcon(svgBuffer, config) {
  const { name, size, maskable } = config;
  const outputPath = join(PUBLIC_DIR, name);

  let pipeline = sharp(svgBuffer).resize(size, size);

  if (maskable) {
    // For maskable icons, we need to add padding
    // The icon content should be 80% of total size (safe zone)
    const iconSize = Math.floor(size * 0.8);
    const padding = Math.floor(size * MASKABLE_PADDING_PERCENT);

    // Create the icon at 80% size
    const iconBuffer = await sharp(svgBuffer).resize(iconSize, iconSize).png().toBuffer();

    // Composite onto a background with padding
    pipeline = sharp({
      create: {
        width: size,
        height: size,
        channels: 4,
        background: { r: 26, g: 26, b: 46, alpha: 1 }, // #1a1a2e
      },
    }).composite([
      {
        input: iconBuffer,
        top: padding,
        left: padding,
      },
    ]);
  }

  const written = await writeIfChanged(await pipeline.png().toBuffer(), outputPath);
  console.log(
    `${written ? 'Generated' : 'Unchanged'}: ${name} (${size}x${size}${maskable ? ' maskable' : ''})`
  );
}

async function generateFavicon(svgBuffer) {
  // Note: This generates a 32x32 PNG saved as .ico
  // Modern browsers handle this fine; proper multi-size ICO would need additional tooling
  const outputPath = join(PUBLIC_DIR, 'favicon.ico');
  const written = await writeIfChanged(
    await sharp(svgBuffer).resize(32, 32).png().toBuffer(),
    outputPath
  );
  console.log(`${written ? 'Generated' : 'Unchanged'}: favicon.ico (32x32)`);
}

async function main() {
  console.log('Generating PWA icons from favicon.svg...\n');

  if (!existsSync(SVG_SOURCE)) {
    console.error(`Error: SVG source not found at ${SVG_SOURCE}`);
    process.exit(1);
  }

  const svgBuffer = readFileSync(SVG_SOURCE);

  // Generate all PNG icons
  for (const config of ICONS) {
    await generateIcon(svgBuffer, config);
  }

  // Generate favicon.ico
  await generateFavicon(svgBuffer);

  console.log('\nAll icons generated successfully!');
}

main().catch(err => {
  console.error('Error generating icons:', err);
  process.exit(1);
});
