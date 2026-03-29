/**
 * Serial port lock — mutual exclusion for serial port access.
 *
 * Multiple tools (blinky-server, blinky-serial-mcp, uf2_upload.py) compete
 * for serial ports. This module implements per-port file-based locking using
 * atomic O_CREAT|O_EXCL creation, matching the audio-lock pattern.
 *
 * Lock directory: /tmp/blinky-serial/
 * Lock file:      /tmp/blinky-serial/<port_basename>.lock
 * Contents:       JSON {pid, tool, port, purpose, acquired, hold_until}
 *
 * Protocol is identical to tools/serial_lock.py (Python) and
 * blinky-server/blinky_server/transport/serial_lock.py.
 */

import * as fs from 'node:fs';
import * as path from 'node:path';

const LOCK_DIR = '/tmp/blinky-serial';

interface SerialLockInfo {
  pid: number;
  tool: string;
  port: string;
  purpose: string;
  acquired: string;
  hold_until: string;
}

const heldPorts = new Set<string>();

function normalizePort(port: string): string {
  try {
    const resolved = fs.realpathSync(port);
    return path.basename(resolved);
  } catch {
    return path.basename(port);
  }
}

function lockPath(port: string): string {
  return path.join(LOCK_DIR, `${normalizePort(port)}.lock`);
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err: unknown) {
    if ((err as NodeJS.ErrnoException).code === 'ESRCH') {
      return false;
    }
    // EPERM = process exists but we can't signal it
    return true;
  }
}

/**
 * Acquire a lock on a serial port.
 *
 * Uses atomic file creation (O_CREAT|O_EXCL) so two processes racing
 * will never both succeed. If a stale lock is detected (dead PID), it
 * is removed and acquisition retried once.
 */
export function acquireSerialLock(
  port: string,
  purpose: string,
  holdSeconds: number = 60,
): boolean {
  fs.mkdirSync(LOCK_DIR, { recursive: true });
  const lp = lockPath(port);

  const now = new Date();
  const holdUntil = new Date(now.getTime() + holdSeconds * 1000);

  let resolvedPort: string;
  try {
    resolvedPort = fs.realpathSync(port);
  } catch {
    resolvedPort = port;
  }

  const info: SerialLockInfo = {
    pid: process.pid,
    tool: 'blinky-serial-mcp',
    port: resolvedPort,
    purpose,
    acquired: now.toISOString(),
    hold_until: holdUntil.toISOString(),
  };

  // Bounded loop: max 2 attempts (initial + one retry after stale lock cleanup)
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const fd = fs.openSync(
        lp,
        fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_WRONLY,
      );
      fs.writeSync(fd, JSON.stringify(info));
      fs.closeSync(fd);
      heldPorts.add(port);
      return true;
    } catch (err: unknown) {
      const e = err as NodeJS.ErrnoException;
      if (e.code !== 'EEXIST') {
        throw e;
      }
    }

    // Lock exists — check if holder is alive
    try {
      const raw = fs.readFileSync(lp, 'utf-8');
      const holder: SerialLockInfo = JSON.parse(raw);

      if (!pidAlive(holder.pid)) {
        // Stale lock — remove and retry
        try {
          fs.unlinkSync(lp);
        } catch {
          // Race with another cleaner
        }
        continue; // Retry once
      }

      // Process is alive — lock is legitimately held
      return false;
    } catch {
      // Can't read lock file — treat as held (conservative)
      return false;
    }
  }

  return false;
}

/**
 * Release a lock on a serial port. Safe to call if not held.
 */
export function releaseSerialLock(port: string): void {
  const lp = lockPath(port);
  try {
    const raw = fs.readFileSync(lp, 'utf-8');
    const holder: SerialLockInfo = JSON.parse(raw);
    if (holder.pid === process.pid) {
      fs.unlinkSync(lp);
    }
  } catch {
    // Ignore — file may not exist or not be ours
  }
  heldPorts.delete(port);
}

/**
 * Check if a port is locked. Auto-cleans stale locks (dead PID).
 */
export function isSerialLocked(port: string): {
  locked: boolean;
  holder?: SerialLockInfo;
} {
  const lp = lockPath(port);
  try {
    const raw = fs.readFileSync(lp, 'utf-8');
    const holder: SerialLockInfo = JSON.parse(raw);

    if (!pidAlive(holder.pid)) {
      try {
        fs.unlinkSync(lp);
      } catch {
        // Race
      }
      return { locked: false };
    }

    return { locked: true, holder };
  } catch {
    return { locked: false };
  }
}

// --- Process exit cleanup ---
function cleanup() {
  for (const port of heldPorts) {
    releaseSerialLock(port);
  }
}

process.on('exit', cleanup);
process.on('SIGINT', () => {
  cleanup();
  process.exit(128 + 2);
});
process.on('SIGTERM', () => {
  cleanup();
  process.exit(128 + 15);
});
