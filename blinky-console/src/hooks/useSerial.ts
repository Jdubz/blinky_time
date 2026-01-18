import { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import {
  serialService,
  SerialEvent,
  BatteryStatusData,
  SerialError,
  SerialErrorCode,
} from '../services/serial';
import {
  DeviceInfo,
  DeviceSetting,
  AudioSample,
  BatterySample,
  ConnectionState,
  SettingsByCategory,
  TransientMessage,
  RhythmData,
  MusicModeData,
  RhythmMessage,
  StatusMessage,
  GeneratorType,
  EffectType,
} from '../types';
import { logger } from '../lib/logger';
import { notify } from '../lib/toast';

/**
 * Loading states for various async operations
 */
export interface LoadingState {
  connecting: boolean;
  settings: boolean;
  streaming: boolean;
  generator: boolean;
  effect: boolean;
  saving: boolean;
}

export interface UseSerialReturn {
  // Connection state
  connectionState: ConnectionState;
  isSupported: boolean;
  errorMessage: string | null;
  errorCode: SerialErrorCode | null;
  loading: LoadingState;

  // Device data
  deviceInfo: DeviceInfo | null;
  settings: DeviceSetting[];
  settingsByCategory: SettingsByCategory;

  // Generator/effect state
  currentGenerator: GeneratorType;
  currentEffect: EffectType;
  availableGenerators: GeneratorType[];
  availableEffects: EffectType[];

  // Streaming data
  isStreaming: boolean;
  audioData: AudioSample | null;
  batteryData: BatterySample | null;
  batteryStatusData: BatteryStatusData | null;
  rhythmData: RhythmData | null;
  musicModeData: MusicModeData | null;
  statusData: StatusMessage | null;

  // Transient detection events
  onTransientEvent: (callback: (msg: TransientMessage) => void) => () => void;

  // Rhythm analyzer events
  onRhythmEvent: (callback: (msg: RhythmMessage) => void) => () => void;

  // Status events
  onStatusEvent: (callback: (msg: StatusMessage) => void) => () => void;

  // Serial console
  consoleLines: string[];
  clearConsole: () => void;
  sendCommand: (command: string) => Promise<void>;

  // Actions
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  setSetting: (name: string, value: number | boolean) => Promise<void>;
  toggleStreaming: () => Promise<void>;
  saveSettings: () => Promise<void>;
  loadSettings: () => Promise<void>;
  resetDefaults: () => Promise<void>;
  refreshSettings: () => Promise<void>;
  loadSettingsByCategory: (category: string) => Promise<void>;
  requestBatteryStatus: () => Promise<void>;
  setGenerator: (name: GeneratorType) => Promise<void>;
  setEffect: (name: EffectType) => Promise<void>;
}

const MAX_CONSOLE_LINES = 500;

/**
 * Validates incoming AudioSample data to prevent crashes from malformed serial data
 * Checks for NaN, Infinity, and out-of-range values
 */
function validateAudioSample(sample: AudioSample): boolean {
  // Check all required numeric fields are finite numbers
  // Note: sample.z is optional (not always sent by firmware)
  const numericFields = [sample.l, sample.t, sample.pk, sample.vl, sample.raw, sample.h];
  if (numericFields.some(v => !Number.isFinite(v))) {
    console.warn('Invalid audio sample: non-finite value detected', sample);
    return false;
  }

  // Check ranges for critical fields
  if (
    sample.l < 0 ||
    sample.l > 1 ||
    sample.pk < 0 ||
    sample.pk > 1 ||
    sample.vl < 0 ||
    sample.vl > 1 ||
    sample.raw < 0 ||
    sample.raw > 1
  ) {
    console.warn('Invalid audio sample: value out of 0-1 range', sample);
    return false;
  }

  // Check boolean flags are exactly 0 or 1
  if (![0, 1].includes(sample.alive)) {
    console.warn('Invalid audio sample: boolean flag not 0 or 1', sample);
    return false;
  }

  return true;
}

// Available generators and effects (matching firmware RenderPipeline)
const AVAILABLE_GENERATORS: GeneratorType[] = ['fire', 'water', 'lightning'];
const AVAILABLE_EFFECTS: EffectType[] = ['none', 'hue'];

const initialLoadingState: LoadingState = {
  connecting: false,
  settings: false,
  streaming: false,
  generator: false,
  effect: false,
  saving: false,
};

export function useSerial(): UseSerialReturn {
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [errorCode, setErrorCode] = useState<SerialErrorCode | null>(null);
  const [loading, setLoading] = useState<LoadingState>(initialLoadingState);
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [settings, setSettings] = useState<DeviceSetting[]>([]);
  const [currentGenerator, setCurrentGenerator] = useState<GeneratorType>('fire');
  const [currentEffect, setCurrentEffect] = useState<EffectType>('none');
  const [isStreaming, setIsStreaming] = useState(false);
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [batteryData, setBatteryData] = useState<BatterySample | null>(null);
  const [batteryStatusData, setBatteryStatusData] = useState<BatteryStatusData | null>(null);
  const [rhythmData, setRhythmData] = useState<RhythmData | null>(null);
  const [musicModeData, setMusicModeData] = useState<MusicModeData | null>(null);
  const [statusData, setStatusData] = useState<StatusMessage | null>(null);
  const [consoleLines, setConsoleLines] = useState<string[]>([]);

  // Helper to update loading state
  const setLoadingState = useCallback((key: keyof LoadingState, value: boolean) => {
    setLoading(prev => ({ ...prev, [key]: value }));
  }, []);

  // Transient event callbacks
  const transientCallbacksRef = useRef<Set<(msg: TransientMessage) => void>>(new Set());
  // Rhythm analyzer event callbacks
  const rhythmCallbacksRef = useRef<Set<(msg: RhythmMessage) => void>>(new Set());
  // Status event callbacks
  const statusCallbacksRef = useRef<Set<(msg: StatusMessage) => void>>(new Set());

  const isSupported = serialService.isSupported();

  // Cleanup on page unload - disconnect serial port to prevent it from being locked
  useEffect(() => {
    const handleBeforeUnload = () => {
      if (connectionState === 'connected') {
        serialService.disconnect();
      }
    };

    const handleVisibilityChange = () => {
      // If page becomes hidden and we're connected, optionally disconnect
      if (document.hidden && connectionState === 'connected') {
        // Optionally disconnect when tab is hidden
        // serialService.disconnect();
      }
    };

    // Keyboard shortcut: Ctrl+D or Escape to disconnect
    const handleKeyDown = (e: KeyboardEvent) => {
      if (connectionState === 'connected') {
        if ((e.ctrlKey && e.key === 'd') || e.key === 'Escape') {
          e.preventDefault();
          serialService.disconnect();
        }
      }
    };

    window.addEventListener('beforeunload', handleBeforeUnload);
    window.addEventListener('pagehide', handleBeforeUnload);
    document.addEventListener('visibilitychange', handleVisibilityChange);
    window.addEventListener('keydown', handleKeyDown);

    return () => {
      window.removeEventListener('beforeunload', handleBeforeUnload);
      window.removeEventListener('pagehide', handleBeforeUnload);
      document.removeEventListener('visibilitychange', handleVisibilityChange);
      window.removeEventListener('keydown', handleKeyDown);
      // Disconnect on component unmount
      if (connectionState === 'connected') {
        serialService.disconnect();
      }
    };
  }, [connectionState]);

  // Group settings by category - memoized to prevent recalculation on every render
  const settingsByCategory = useMemo(
    () =>
      (settings || []).reduce((acc, setting) => {
        const cat = setting.cat || 'other';
        if (!acc[cat]) acc[cat] = [];
        acc[cat].push(setting);
        return acc;
      }, {} as SettingsByCategory),
    [settings]
  );

  // Handle serial events
  useEffect(() => {
    const handleEvent = (event: SerialEvent) => {
      switch (event.type) {
        case 'connected':
          setConnectionState('connected');
          setErrorMessage(null);
          setErrorCode(null);
          setConsoleLines([]);
          break;
        case 'disconnected':
          setConnectionState('disconnected');
          setDeviceInfo(null);
          setSettings([]);
          setIsStreaming(false);
          setAudioData(null);
          setBatteryData(null);
          break;
        case 'audio':
          if (event.audio && validateAudioSample(event.audio.a)) {
            setAudioData(event.audio.a);
            // Update rhythm and music mode data if present in audio stream
            if (event.audio.r) {
              setRhythmData(event.audio.r);
            }
            if (event.audio.m) {
              setMusicModeData(event.audio.m);
            }
          }
          break;
        case 'battery':
          if (event.battery) {
            setBatteryData(event.battery.b);
          }
          break;
        case 'batteryStatus':
          if (event.batteryStatus) {
            setBatteryStatusData(event.batteryStatus);
          }
          break;
        case 'transient':
          if (event.transient) {
            transientCallbacksRef.current.forEach(callback => {
              callback(event.transient!);
            });
          }
          break;
        case 'rhythm':
          if (event.rhythm) {
            // Notify all registered rhythm callbacks
            rhythmCallbacksRef.current.forEach(callback => {
              callback(event.rhythm!);
            });
          }
          break;
        case 'status':
          if (event.status) {
            setStatusData(event.status);
            // Notify all registered status callbacks
            statusCallbacksRef.current.forEach(callback => {
              callback(event.status!);
            });
          }
          break;
        case 'data':
          if (event.data) {
            setConsoleLines(prev => {
              const newLines = [...prev, event.data!];
              // Keep only the last MAX_CONSOLE_LINES
              return newLines.length > MAX_CONSOLE_LINES
                ? newLines.slice(newLines.length - MAX_CONSOLE_LINES)
                : newLines;
            });
          }
          break;
        case 'error':
          setConnectionState('error');
          setLoading(initialLoadingState); // Reset all loading states on error
          if (event.error) {
            const message = event.error.message || 'Unknown error';
            const code = event.error instanceof SerialError ? event.error.code : null;
            setErrorMessage(message);
            setErrorCode(code);
            logger.error('Serial error', { code, message });
            notify.error(`Connection error: ${message}`);
          }
          break;
      }
    };

    serialService.addEventListener(handleEvent);
    return () => serialService.removeEventListener(handleEvent);
  }, []);

  // Connect to device
  const connect = useCallback(async () => {
    logger.info('Initiating device connection');
    setConnectionState('connecting');
    setLoadingState('connecting', true);

    try {
      const success = await serialService.connect();

      if (success) {
        setLoadingState('connecting', false);
        setLoadingState('settings', true);

        // Fetch device info
        const info = await serialService.getDeviceInfo();
        if (info) {
          setDeviceInfo(info);
          logger.debug('Device info loaded', { device: info.device });
        } else {
          logger.warn('Failed to fetch device info');
        }

        // Fetch settings
        const settingsResponse = await serialService.getSettings();
        if (settingsResponse?.settings) {
          setSettings(settingsResponse.settings);
          logger.info('Settings loaded', { count: settingsResponse.settings.length });
        } else {
          logger.warn('Failed to fetch settings');
        }

        setLoadingState('settings', false);
        notify.success('Connected to device');
      } else {
        setLoadingState('connecting', false);
        notify.error('Failed to connect');
      }
    } catch (error) {
      setLoadingState('connecting', false);
      setLoadingState('settings', false);
      const message = error instanceof Error ? error.message : 'Connection failed';
      logger.error('Connection error', { error: message });
      notify.error(message);
    }
  }, [setLoadingState]);

  // Disconnect from device
  const disconnect = useCallback(async () => {
    logger.info('Disconnecting from device');
    try {
      if (isStreaming) {
        await serialService.setStreamEnabled(false);
      }
      await serialService.disconnect();
      notify.info('Disconnected from device');
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Disconnect failed';
      logger.error('Disconnect error', { error: message });
    }
  }, [isStreaming]);

  // Set a setting value
  const setSetting = useCallback(async (name: string, value: number | boolean) => {
    await serialService.setSetting(name, value);
    // Update local state
    setSettings(prev => prev.map(s => (s.name === name ? { ...s, value } : s)));
  }, []);

  // Toggle audio streaming
  const toggleStreaming = useCallback(async () => {
    const newState = !isStreaming;
    await serialService.setStreamEnabled(newState);
    setIsStreaming(newState);
    if (!newState) {
      setAudioData(null);
      setBatteryData(null);
    }
  }, [isStreaming]);

  // Save settings to flash
  const saveSettings = useCallback(async () => {
    logger.debug('Saving settings to flash');
    setLoadingState('saving', true);
    try {
      await serialService.saveSettings();
      notify.success('Settings saved');
      logger.info('Settings saved to flash');
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Save failed';
      logger.error('Save settings error', { error: message });
      notify.error(`Failed to save: ${message}`);
      throw error;
    } finally {
      setLoadingState('saving', false);
    }
  }, [setLoadingState]);

  // Load settings from flash
  const loadSettings = useCallback(async () => {
    logger.debug('Loading settings from flash');
    setLoadingState('settings', true);
    try {
      await serialService.loadSettings();
      // Refresh settings after load
      const settingsResponse = await serialService.getSettings();
      if (settingsResponse) {
        setSettings(settingsResponse.settings);
        notify.success('Settings loaded');
        logger.info('Settings loaded from flash', { count: settingsResponse.settings.length });
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Load failed';
      logger.error('Load settings error', { error: message });
      notify.error(`Failed to load: ${message}`);
      throw error;
    } finally {
      setLoadingState('settings', false);
    }
  }, [setLoadingState]);

  // Reset to defaults
  const resetDefaults = useCallback(async () => {
    logger.debug('Resetting to defaults');
    setLoadingState('settings', true);
    try {
      await serialService.resetDefaults();
      // Refresh settings after reset
      const settingsResponse = await serialService.getSettings();
      if (settingsResponse) {
        setSettings(settingsResponse.settings);
        notify.success('Reset to defaults');
        logger.info('Settings reset to defaults');
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : 'Reset failed';
      logger.error('Reset defaults error', { error: message });
      notify.error(`Failed to reset: ${message}`);
      throw error;
    } finally {
      setLoadingState('settings', false);
    }
  }, [setLoadingState]);

  // Refresh settings
  const refreshSettings = useCallback(async () => {
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, []);

  // Load settings for a specific category (lazy loading)
  const loadSettingsByCategory = useCallback(
    async (category: string) => {
      logger.debug('Loading settings for category', { category });
      setLoadingState('settings', true);
      try {
        const response = await serialService.getSettingsByCategory(category);
        if (response?.settings) {
          // Capture settings from response before the state update
          const newSettings = response.settings;
          // Merge category settings with existing settings using functional update
          // This ensures we always merge with the latest state, preventing race conditions
          setSettings(prev => {
            // Remove old settings from this category, add new ones
            const filtered = prev.filter(s => s.cat !== category);
            return [...filtered, ...newSettings];
          });
          logger.debug('Category settings loaded', { category, count: newSettings.length });
        } else {
          logger.warn('No settings returned for category', { category });
        }
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Failed to load category settings';
        logger.error('Load category settings error', { category, error: message });
        notify.error(`Failed to load ${category} settings: ${message}`);
      } finally {
        setLoadingState('settings', false);
      }
    },
    [setLoadingState]
  );

  // Request battery status data
  const requestBatteryStatus = useCallback(async () => {
    await serialService.requestBatteryStatus();
  }, []);

  // Set active generator
  const setGenerator = useCallback(
    async (name: GeneratorType) => {
      logger.debug('Setting generator', { name });
      setLoadingState('generator', true);
      try {
        await serialService.setGenerator(name);
        setCurrentGenerator(name);
        // Refresh settings after switching generator
        const settingsResponse = await serialService.getSettings();
        if (settingsResponse) {
          setSettings(settingsResponse.settings);
        }
        notify.success(`Generator: ${name}`);
        logger.info('Generator set', { name });
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Generator switch failed';
        logger.error('Set generator error', { name, error: message });
        notify.error(`Failed to set generator: ${message}`);
        throw error;
      } finally {
        setLoadingState('generator', false);
      }
    },
    [setLoadingState]
  );

  // Set active effect
  const setEffect = useCallback(
    async (name: EffectType) => {
      logger.debug('Setting effect', { name });
      setLoadingState('effect', true);
      try {
        await serialService.setEffect(name);
        setCurrentEffect(name);
        notify.success(`Effect: ${name}`);
        logger.info('Effect set', { name });
      } catch (error) {
        const message = error instanceof Error ? error.message : 'Effect switch failed';
        logger.error('Set effect error', { name, error: message });
        notify.error(`Failed to set effect: ${message}`);
        throw error;
      } finally {
        setLoadingState('effect', false);
      }
    },
    [setLoadingState]
  );

  // Clear console
  const clearConsole = useCallback(() => {
    setConsoleLines([]);
  }, []);

  // Send command to serial
  const sendCommand = useCallback(async (command: string) => {
    await serialService.send(command);
    // Add the command to console as well
    setConsoleLines(prev => {
      const newLines = [...prev, `> ${command}`];
      return newLines.length > MAX_CONSOLE_LINES
        ? newLines.slice(newLines.length - MAX_CONSOLE_LINES)
        : newLines;
    });
  }, []);

  // Register callback for transient events
  const onTransientEvent = useCallback((callback: (msg: TransientMessage) => void) => {
    transientCallbacksRef.current.add(callback);
    return () => {
      transientCallbacksRef.current.delete(callback);
    };
  }, []);

  // Register callback for rhythm analyzer events
  const onRhythmEvent = useCallback((callback: (msg: RhythmMessage) => void) => {
    rhythmCallbacksRef.current.add(callback);
    // Return cleanup function
    return () => {
      rhythmCallbacksRef.current.delete(callback);
    };
  }, []);

  // Register callback for status events
  const onStatusEvent = useCallback((callback: (msg: StatusMessage) => void) => {
    statusCallbacksRef.current.add(callback);
    // Return cleanup function
    return () => {
      statusCallbacksRef.current.delete(callback);
    };
  }, []);

  return {
    connectionState,
    isSupported,
    errorMessage,
    errorCode,
    loading,
    deviceInfo,
    settings,
    settingsByCategory,
    currentGenerator,
    currentEffect,
    availableGenerators: AVAILABLE_GENERATORS,
    availableEffects: AVAILABLE_EFFECTS,
    isStreaming,
    audioData,
    batteryData,
    batteryStatusData,
    rhythmData,
    musicModeData,
    statusData,
    onTransientEvent,
    onRhythmEvent,
    onStatusEvent,
    consoleLines,
    clearConsole,
    sendCommand,
    connect,
    disconnect,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
    loadSettingsByCategory,
    requestBatteryStatus,
    setGenerator,
    setEffect,
  };
}
