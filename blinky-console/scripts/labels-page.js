#!/usr/bin/env node
// Put the drum-label reviewer page into the site at /labels/ (blinky-console-deploy.yml runs this
// before the build). The page is this repo's labels-service/index.html, one self-contained file, so
// page and service come from the same commit. This writes two values into its meta tags:
//   drum-reviewer-api      the hosted labels API's public URL (LABELS_API_URL);
//   drum-reviewer-version  the git tree hash of labels-service/, which the deployed service reports
//                          too: the page asks for a reload when they differ.
//
//   LABELS_API_URL=https://... node scripts/labels-page.js
//
// Output: public/labels/index.html (gitignored), which Vite copies into dist/.
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const api = (process.env.LABELS_API_URL || '').replace(/\/+$/, '');
if (!/^https:\/\/[a-z0-9.-]+$/i.test(api)) {
  console.error(`LABELS_API_URL must be https://<host>, got ${JSON.stringify(api)}`);
  process.exit(1);
}
const version = execFileSync('git', ['rev-parse', 'HEAD:labels-service'], { cwd: root })
  .toString()
  .trim();
if (!/^[0-9a-f]{40}$/.test(version)) {
  console.error(`no labels-service tree at HEAD (got ${JSON.stringify(version)})`);
  process.exit(1);
}
let page = readFileSync(join(root, 'labels-service', 'index.html'), 'utf8');
for (const [name, value] of [
  ['drum-reviewer-api', api],
  ['drum-reviewer-version', version],
]) {
  const tag = `<meta name="${name}" content="">`;
  if (!page.includes(tag)) {
    console.error(`labels-service/index.html has no ${tag}`);
    process.exit(1);
  }
  page = page.replace(tag, `<meta name="${name}" content="${value}">`);
}
const dest = join(here, '..', 'public', 'labels', 'index.html');
mkdirSync(dirname(dest), { recursive: true });
writeFileSync(dest, page);
console.log(`labels page: labels-service ${version.slice(0, 12)} -> ${dest} (API ${api})`);
