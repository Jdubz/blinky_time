/**
 * Structured logger with debug toggle for blinky-console
 *
 * Usage:
 *   import { logger } from './lib/logger';
 *   logger.debug('Fetching settings', { command: 'json settings' });
 *   logger.error('Failed to connect', { error });
 *
 * Enable debug mode in production:
 *   localStorage.setItem('BLINKY_DEBUG', 'true');
 *   location.reload();
 */

const DEBUG_KEY = 'BLINKY_DEBUG';

// Check if debug mode is enabled (always on in dev, opt-in in prod)
const isDebugEnabled = (): boolean => {
  if (import.meta.env.DEV) return true;
  try {
    return localStorage.getItem(DEBUG_KEY) === 'true';
  } catch {
    return false;
  }
};

// Format timestamp for log messages
const timestamp = (): string => {
  const now = new Date();
  return now.toISOString().slice(11, 23); // HH:MM:SS.mmm
};

export type LogLevel = 'debug' | 'info' | 'warn' | 'error';

export interface Logger {
  debug: (message: string, data?: object) => void;
  info: (message: string, data?: object) => void;
  warn: (message: string, data?: object) => void;
  error: (message: string, data?: object) => void;
  setLevel: (level: LogLevel) => void;
  enableDebug: () => void;
  disableDebug: () => void;
}

// Log level priority (lower = more verbose)
const levelPriority: Record<LogLevel, number> = {
  debug: 0,
  info: 1,
  warn: 2,
  error: 3,
};

let minLevel: LogLevel = 'debug';

const shouldLog = (level: LogLevel): boolean => {
  if (level === 'debug' && !isDebugEnabled()) return false;
  return levelPriority[level] >= levelPriority[minLevel];
};

export const logger: Logger = {
  debug: (message: string, data?: object) => {
    if (!shouldLog('debug')) return;
    const prefix = `%c[${timestamp()}] [DEBUG]`;
    const style = 'color: #888';
    if (data) {
      console.log(prefix, style, message, data);
    } else {
      console.log(prefix, style, message);
    }
  },

  info: (message: string, data?: object) => {
    if (!shouldLog('info')) return;
    const prefix = `[${timestamp()}] [INFO]`;
    if (data) {
      console.log(prefix, message, data);
    } else {
      console.log(prefix, message);
    }
  },

  warn: (message: string, data?: object) => {
    if (!shouldLog('warn')) return;
    const prefix = `[${timestamp()}] [WARN]`;
    if (data) {
      console.warn(prefix, message, data);
    } else {
      console.warn(prefix, message);
    }
  },

  error: (message: string, data?: object) => {
    if (!shouldLog('error')) return;
    const prefix = `[${timestamp()}] [ERROR]`;
    if (data) {
      console.error(prefix, message, data);
    } else {
      console.error(prefix, message);
    }
  },

  setLevel: (level: LogLevel) => {
    minLevel = level;
  },

  enableDebug: () => {
    try {
      localStorage.setItem(DEBUG_KEY, 'true');
      console.log('[Logger] Debug mode enabled. Reload page to see debug logs.');
    } catch {
      console.warn('[Logger] Could not enable debug mode (localStorage unavailable)');
    }
  },

  disableDebug: () => {
    try {
      localStorage.removeItem(DEBUG_KEY);
      console.log('[Logger] Debug mode disabled.');
    } catch {
      console.warn('[Logger] Could not disable debug mode (localStorage unavailable)');
    }
  },
};

// Export helper to check debug state
export const isDebug = isDebugEnabled;
