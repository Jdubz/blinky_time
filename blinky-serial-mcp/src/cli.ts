#!/usr/bin/env node
/**
 * CLI wrapper for blinky-serial validation suite.
 * Spawns test-runner.js directly — no MCP server needed.
 *
 * Usage:
 *   node dist/cli.js validate --ports /dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM4 --duration 35000
 *   node dist/cli.js validate --ports /dev/ttyACM1 --tracks techno-minimal-01,trance-party
 */

import { spawn } from 'child_process';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

async function main() {
  const args = process.argv.slice(2);
  const command = args[0];

  if (command !== 'validate') {
    console.error('Usage: node dist/cli.js validate --ports PORT1,PORT2 [--duration MS] [--tracks TRACK1,TRACK2] [--runs N]');
    process.exit(1);
  }

  // Parse args
  const getArg = (name: string): string | undefined => {
    const idx = args.indexOf(name);
    return idx >= 0 && idx + 1 < args.length ? args[idx + 1] : undefined;
  };

  const ports = (getArg('--ports') || '').split(',').filter(Boolean);
  const duration = parseInt(getArg('--duration') || '35000', 10);
  const tracks = getArg('--tracks')?.split(',').filter(Boolean);
  const runs = parseInt(getArg('--runs') || '1', 10);

  if (ports.length === 0) {
    console.error('Error: --ports required');
    process.exit(1);
  }

  console.error(`Validation suite: ${ports.length} ports, ${duration}ms duration, ${runs} run(s)`);
  if (tracks) console.error(`Tracks: ${tracks.join(', ')}`);
  else console.error('Tracks: all');

  // Spawn test-runner.js directly (no MCP server hop)
  const runnerPath = join(__dirname, 'test-runner.js');
  const cliArgs = ['validate', '--ports', ports.join(','), '--duration', String(duration), '--runs', String(runs)];
  if (tracks) cliArgs.push('--tracks', tracks.join(','));

  const child = spawn('node', [runnerPath, ...cliArgs], {
    stdio: ['ignore', 'pipe', 'inherit'],  // stdout piped (JSON), stderr inherited (progress)
  });

  child.stdout.pipe(process.stdout);

  const exitCode = await new Promise<number>((resolve) => {
    child.on('close', (code) => resolve(code ?? 1));
  });

  process.exit(exitCode);
}

main().catch((err) => {
  console.error('Error:', err.message);
  process.exit(1);
});
