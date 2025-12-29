import { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { serialService, SerialEvent, BatteryStatusData } from '../services/serial';
import {
  DeviceInfo,
  DeviceSetting,
  AudioSample,
  BatterySample,
  ConnectionState,
  SettingsByCategory,
  TransientMessage,
} from '../types';

export interface UseSerialReturn {
  // Connection state
  connectionState: ConnectionState;
  isSupported: boolean;

  // Device data
  deviceInfo: DeviceInfo | null;
  settings: DeviceSetting[];
  settingsByCategory: SettingsByCategory;

  // Preset data
  presets: string[];
  currentPreset: string | null;

  // Streaming data
  isStreaming: boolean;
  audioData: AudioSample | null;
  batteryData: BatterySample | null;
  batteryStatusData: BatteryStatusData | null;

  // Transient detection (legacy name kept for compatibility)
  onPercussionEvent: (callback: (msg: TransientMessage) => void) => () => void;

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
  requestBatteryStatus: () => Promise<void>;
  applyPreset: (name: string) => Promise<void>;
}

const MAX_CONSOLE_LINES = 500;

/**
 * Validates incoming AudioSample data to prevent crashes from malformed serial data
 * Checks for NaN, Infinity, and out-of-range values
 */
function validateAudioSample(sample: AudioSample): boolean {
  // Check all numeric fields are finite numbers
  const numericFields = [sample.l, sample.t, sample.pk, sample.vl, sample.raw, sample.h, sample.z];
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

export function useSerial(): UseSerialReturn {
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [settings, setSettings] = useState<DeviceSetting[]>([]);
  const [presets, setPresets] = useState<string[]>([]);
  const [currentPreset, setCurrentPreset] = useState<string | null>(null);
  const [isStreaming, setIsStreaming] = useState(false);
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [batteryData, setBatteryData] = useState<BatterySample | null>(null);
  const [batteryStatusData, setBatteryStatusData] = useState<BatteryStatusData | null>(null);
  const [consoleLines, setConsoleLines] = useState<string[]>([]);

  // Transient event callbacks (legacy name kept for compatibility)
  const percussionCallbacksRef = useRef<Set<(msg: TransientMessage) => void>>(new Set());

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
      settings.reduce((acc, setting) => {
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
            // Notify all registered transient callbacks
            percussionCallbacksRef.current.forEach(callback => {
              callback(event.transient!);
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
          break;
      }
    };

    serialService.addEventListener(handleEvent);
    return () => serialService.removeEventListener(handleEvent);
  }, []);

  // Connect to device
  const connect = useCallback(async () => {
    setConnectionState('connecting');
    const success = await serialService.connect();

    if (success) {
      // Fetch device info
      const info = await serialService.getDeviceInfo();
      if (info) {
        setDeviceInfo(info);
      }

      // Fetch settings
      const settingsResponse = await serialService.getSettings();
      if (settingsResponse) {
        setSettings(settingsResponse.settings);
      }

      // Fetch available presets
      const presetList = await serialService.getPresets();
      if (presetList) {
        setPresets(presetList);
      }
    }
  }, []);

  // Disconnect from device
  const disconnect = useCallback(async () => {
    if (isStreaming) {
      await serialService.setStreamEnabled(false);
    }
    await serialService.disconnect();
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
    await serialService.saveSettings();
  }, []);

  // Load settings from flash
  const loadSettings = useCallback(async () => {
    await serialService.loadSettings();
    // Refresh settings after load
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, []);

  // Reset to defaults
  const resetDefaults = useCallback(async () => {
    await serialService.resetDefaults();
    // Refresh settings after reset
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, []);

  // Refresh settings
  const refreshSettings = useCallback(async () => {
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, []);

  // Request battery status data
  const requestBatteryStatus = useCallback(async () => {
    await serialService.requestBatteryStatus();
  }, []);

  // Apply a preset
  const applyPreset = useCallback(async (name: string) => {
    await serialService.applyPreset(name);
    setCurrentPreset(name);
    // Refresh settings after applying preset
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, []);

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

  // Register callback for transient events (legacy name kept for compatibility)
  const onPercussionEvent = useCallback((callback: (msg: TransientMessage) => void) => {
    percussionCallbacksRef.current.add(callback);
    // Return cleanup function
    return () => {
      percussionCallbacksRef.current.delete(callback);
    };
  }, []);

  return {
    connectionState,
    isSupported,
    deviceInfo,
    settings,
    settingsByCategory,
    presets,
    currentPreset,
    isStreaming,
    audioData,
    batteryData,
    batteryStatusData,
    onPercussionEvent,
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
    requestBatteryStatus,
    applyPreset,
  };
}
