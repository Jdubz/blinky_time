/**
 * Audio lock — mutual exclusion for audio playback.
 *
 * All devices share the same room, so only one process may play audio at a
 * time. This module implements a file-based lock using atomic O_CREAT|O_EXCL
 * creation, matching the protocol in ml-training/tools/ab_test_multidev.cjs.
 *
 * Lock file: /tmp/blinky-audio.lock
 * Contents:  { pid, ports, started } (JSON)
 */

import * as fs from 'node:fs';

const LOCK_PATH = '/tmp/blinky-audio.lock';

let lockHeld = false;

interface LockInfo {
  pid: number;
  ports: string[];
  started: string;
}

/**
 * Attempt to acquire the audio lock.
 *
 * Uses atomic file creation (O_CREAT|O_EXCL) so two processes racing will
 * never both succeed. If a stale lock is detected (owning PID no longer
 * exists), it is removed and acquisition is retried once.
 *
 * Returns true if the lock was acquired, false if another live process holds it.
 */
export function acquireAudioLock(ports: string[]): boolean {
  try {
    const fd = fs.openSync(
      LOCK_PATH,
      fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_WRONLY,
    );
    const info: LockInfo = {
      pid: process.pid,
      ports,
      started: new Date().toISOString(),
    };
    fs.writeSync(fd, JSON.stringify(info));
    fs.closeSync(fd);
    lockHeld = true;
    return true;
  } catch (err: unknown) {
    const e = err as NodeJS.ErrnoException;
    if (e.code === 'EEXIST') {
      // Lock file already exists — check if the holder is still alive.
      try {
        const raw = fs.readFileSync(LOCK_PATH, 'utf-8');
        const info: LockInfo = JSON.parse(raw);

        try {
          // Signal 0 checks existence without actually signaling.
          process.kill(info.pid, 0);
        } catch (killErr: unknown) {
          if ((killErr as NodeJS.ErrnoException).code === 'ESRCH') {
            // Process is dead — stale lock. Remove and retry once.
            try {
              fs.unlinkSync(LOCK_PATH);
            } catch {
              // Another process may have grabbed it between our read and unlink.
            }
            return acquireAudioLock(ports);
          }
        }

        // Process is alive — lock is legitimately held.
        console.error(
          `Audio lock held by PID ${info.pid} on ${info.ports.join(', ')} (started ${info.started})`,
        );
        console.error(
          'All devices share the same room — concurrent audio tests are invalid.',
        );
        console.error(
          `Remove ${LOCK_PATH} manually if the process is stuck.`,
        );
      } catch {
        // Could not read/parse the lock file.
        console.error(
          `Audio lock exists at ${LOCK_PATH}. Another test may be running.`,
        );
      }
      return false;
    }
    // Unexpected error (permissions, disk full, etc.) — propagate.
    throw e;
  }
}

/**
 * Release the audio lock. Safe to call even if the lock is not held.
 */
export function releaseAudioLock(): void {
  if (!lockHeld) return;
  try {
    fs.unlinkSync(LOCK_PATH);
  } catch {
    // Ignore — file may already be gone.
  }
  lockHeld = false;
}

/**
 * Check whether the audio lock is currently held by a live process.
 *
 * If a stale lock is found (dead PID), it is cleaned up automatically and
 * the function reports unlocked.
 */
export function isAudioLocked(): {
  locked: boolean;
  holder?: LockInfo;
} {
  try {
    const raw = fs.readFileSync(LOCK_PATH, 'utf-8');
    const info: LockInfo = JSON.parse(raw);

    try {
      process.kill(info.pid, 0);
    } catch (killErr: unknown) {
      if ((killErr as NodeJS.ErrnoException).code === 'ESRCH') {
        // Stale lock — clean it up.
        try {
          fs.unlinkSync(LOCK_PATH);
        } catch {
          // Race with another cleaner — fine.
        }
        return { locked: false };
      }
    }

    return { locked: true, holder: info };
  } catch {
    // No lock file or unreadable — not locked.
    return { locked: false };
  }
}

// --- Process exit cleanup ---
// Release the lock on any exit path so we never leave stale locks behind.

function cleanup() {
  releaseAudioLock();
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
process.on('uncaughtException', (err) => {
  console.error('Uncaught exception — releasing audio lock:', err);
  cleanup();
  process.exit(1);
});
