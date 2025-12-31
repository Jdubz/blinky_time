/**
 * Structured logger with debug toggle for blinky-console
 *
 * Usage:
 *   import { logger } from './lib/logger';
 *   logger.debug('Fetching settings', { command: 'json settings' });
 *   logger.error('Failed to connect', { error });
 *
 * Console commands (in browser developer console):
 *   blinky.log.setLevel('debug')  - Show all logs (debug, info, warn, error)
 *   blinky.log.setLevel('info')   - Show info and above (default)
 *   blinky.log.setLevel('warn')   - Show warn and error only
 *   blinky.log.setLevel('error')  - Show errors only
 *   blinky.log.enableDebug()      - Enable debug mode (persists in localStorage)
 *   blinky.log.disableDebug()     - Disable debug mode
 *   blinky.log.getLevel()         - Show current log level
 *   blinky.log.help()             - Show available commands
 */

const DEBUG_KEY = 'BLINKY_DEBUG';
const LEVEL_KEY = 'BLINKY_LOG_LEVEL';

// Check if debug mode is enabled (opt-in via localStorage in both dev and prod)
const isDebugEnabled = (): boolean => {
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
  getLevel: () => LogLevel;
  enableDebug: () => void;
  disableDebug: () => void;
  help: () => void;
}

// Log level priority (lower = more verbose)
const levelPriority: Record<LogLevel, number> = {
  debug: 0,
  info: 1,
  warn: 2,
  error: 3,
};

// Load persisted level from localStorage, default to 'info'
const loadPersistedLevel = (): LogLevel => {
  try {
    const stored = localStorage.getItem(LEVEL_KEY);
    if (stored && stored in levelPriority) {
      return stored as LogLevel;
    }
  } catch {
    // localStorage unavailable
  }
  return 'info';
};

let minLevel: LogLevel = loadPersistedLevel();

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
    if (!(level in levelPriority)) {
      console.error(`[Logger] Invalid level '${level}'. Use: debug, info, warn, error`);
      return;
    }
    minLevel = level;
    try {
      localStorage.setItem(LEVEL_KEY, level);
    } catch {
      // localStorage unavailable
    }
    console.log(`[Logger] Log level set to '${level}'`);
  },

  getLevel: () => minLevel,

  enableDebug: () => {
    try {
      localStorage.setItem(DEBUG_KEY, 'true');
      console.log('[Logger] Debug mode enabled. Debug logs will now appear.');
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

  help: () => {
    console.log(
      `
%cBlinky Console Logger Commands
%c─────────────────────────────────────────────
  blinky.log.setLevel('debug')  - Show all logs
  blinky.log.setLevel('info')   - Show info and above (default)
  blinky.log.setLevel('warn')   - Show warnings and errors only
  blinky.log.setLevel('error')  - Show errors only
  blinky.log.getLevel()         - Get current log level
  blinky.log.enableDebug()      - Enable debug mode (persists)
  blinky.log.disableDebug()     - Disable debug mode
  blinky.log.help()             - Show this help

%cCurrent level: ${minLevel}
%cDebug mode: ${isDebugEnabled() ? 'enabled' : 'disabled'}
`,
      'font-weight: bold; font-size: 14px',
      'color: #888',
      'color: #4CAF50',
      'color: #2196F3'
    );
  },
};

// Expose logger to window for console access
declare global {
  interface Window {
    blinky: {
      log: Logger;
    };
  }
}

if (typeof window !== 'undefined') {
  window.blinky = window.blinky || {};
  window.blinky.log = logger;
}

// Export helper to check debug state
export const isDebug = isDebugEnabled;
