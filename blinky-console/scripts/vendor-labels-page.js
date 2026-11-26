#!/usr/bin/env node
// Vendor the drum-label reviewer page into the site at /labels/ (blinky-console-deploy.yml runs this
// before the build). The page is blinky-corpus's tools/drum-reviewer/index.html: one self-contained
// file. It's taken from the corpus commit this repo pins as its `corpus` submodule (fetched by that
// SHA, so it's immutable and the submodule needn't be checked out), and the hosted API's public URL is
// written into its <meta name="drum-reviewer-api"> tag.
//
//   CORPUS_SHA=<sha> LABELS_API_URL=https://... node scripts/vendor-labels-page.js
//
// CORPUS_SHA defaults to the gitlink in HEAD (`git ls-tree HEAD corpus`). Output:
// public/labels/index.html (gitignored), which Vite copies into dist/.
import { execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const api = (process.env.LABELS_API_URL || '').replace(/\/+$/, '');
if (!/^https:\/\/[a-z0-9.-]+$/i.test(api)) {
  console.error(`LABELS_API_URL must be https://<host>, got ${JSON.stringify(api)}`);
  process.exit(1);
}
let sha = process.env.CORPUS_SHA;
if (!sha) {
  const line = execFileSync('git', ['ls-tree', 'HEAD', 'corpus'], { cwd: join(here, '..', '..') })
    .toString()
    .trim();
  sha = line.split(/\s+/)[2];
}
if (!/^[0-9a-f]{40}$/.test(sha || '')) {
  console.error(`no corpus commit (got ${JSON.stringify(sha)}): is the corpus submodule pinned?`);
  process.exit(1);
}

const url = `https://raw.githubusercontent.com/Jdubz/blinky-corpus/${sha}/tools/drum-reviewer/index.html`;
const res = await fetch(url);
if (!res.ok) {
  console.error(`${url}: HTTP ${res.status}`);
  process.exit(1);
}
const page = await res.text();
const placeholder = '<meta name="drum-reviewer-api" content="">';
if (!page.includes(placeholder)) {
  console.error(
    `the reviewer page at corpus ${sha.slice(0, 12)} has no API meta tag: bump the corpus pin to a commit with hosted mode`
  );
  process.exit(1);
}
const out = page.replace(placeholder, `<meta name="drum-reviewer-api" content="${api}">`);
const dest = join(here, '..', 'public', 'labels', 'index.html');
mkdirSync(dirname(dest), { recursive: true });
writeFileSync(dest, out);
console.log(`labels page: corpus ${sha.slice(0, 12)} -> ${dest} (API ${api})`);
