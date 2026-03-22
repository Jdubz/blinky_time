#!/usr/bin/env node
/**
 * CLI wrapper for blinky-serial validation suite.
 * Runs independently of MCP protocol — can be invoked via SSH/nohup/tmux.
 *
 * Usage:
 *   node dist/cli.js validate --ports /dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM4 --duration 35000
 *   node dist/cli.js validate --ports /dev/ttyACM1 --tracks techno-minimal-01,trance-party
 */

import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import { spawn } from 'child_process';
import { join } from 'path';

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
  const duration = parseInt(getArg('--duration') || '35000');
  const tracks = getArg('--tracks')?.split(',').filter(Boolean);
  const runs = parseInt(getArg('--runs') || '1');

  if (ports.length === 0) {
    console.error('Error: --ports required');
    process.exit(1);
  }

  console.error(`Validation suite: ${ports.length} ports, ${duration}ms duration, ${runs} run(s)`);
  if (tracks) console.error(`Tracks: ${tracks.join(', ')}`);
  else console.error('Tracks: all');

  // Spawn the MCP server as a subprocess and connect via stdio
  const serverPath = join(__dirname, 'index.js');
  const transport = new StdioClientTransport({
    command: 'node',
    args: [serverPath],
  });

  const client = new Client({ name: 'blinky-cli', version: '1.0.0' }, {});
  await client.connect(transport);

  try {
    // Call the validation suite tool
    const toolArgs: Record<string, unknown> = {
      ports,
      duration_ms: duration,
      runs,
    };
    if (tracks) toolArgs.tracks = tracks;

    console.error('Starting validation suite...');
    const result = await client.callTool({ name: 'run_validation_suite', arguments: toolArgs });

    // Output result to stdout (JSON)
    if (result.content && Array.isArray(result.content)) {
      for (const item of result.content) {
        if (item.type === 'text') {
          console.log(item.text);
        }
      }
    }
  } finally {
    await client.close();
  }
}

main().catch((err) => {
  console.error('Error:', err.message);
  process.exit(1);
});
