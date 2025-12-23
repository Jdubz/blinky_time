import { useState, useEffect, useCallback, useMemo } from 'react';
import { serialService, SerialEvent, BatteryDebugData } from '../services/serial';
import {
  DeviceInfo,
  DeviceSetting,
  AudioSample,
  BatterySample,
  ConnectionState,
  SettingsByCategory,
} from '../types';

export interface UseSerialReturn {
  // Connection state
  connectionState: ConnectionState;
  isSupported: boolean;

  // Device data
  deviceInfo: DeviceInfo | null;
  settings: DeviceSetting[];
  settingsByCategory: SettingsByCategory;

  // Streaming data
  isStreaming: boolean;
  audioData: AudioSample | null;
  batteryData: BatterySample | null;
  batteryDebugData: BatteryDebugData | null;

  // Actions
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  setSetting: (name: string, value: number | boolean) => Promise<void>;
  toggleStreaming: () => Promise<void>;
  saveSettings: () => Promise<void>;
  loadSettings: () => Promise<void>;
  resetDefaults: () => Promise<void>;
  refreshSettings: () => Promise<void>;
  requestBatteryDebug: () => Promise<void>;
}

export function useSerial(): UseSerialReturn {
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [settings, setSettings] = useState<DeviceSetting[]>([]);
  const [isStreaming, setIsStreaming] = useState(false);
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [batteryData, setBatteryData] = useState<BatterySample | null>(null);
  const [batteryDebugData, setBatteryDebugData] = useState<BatteryDebugData | null>(null);

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
          if (event.audio) {
            setAudioData(event.audio.a);
          }
          break;
        case 'battery':
          if (event.battery) {
            setBatteryData(event.battery.b);
          }
          break;
        case 'batteryDebug':
          if (event.batteryDebug) {
            setBatteryDebugData(event.batteryDebug);
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

  // Request battery debug data
  const requestBatteryDebug = useCallback(async () => {
    await serialService.requestBatteryDebug();
  }, []);

  return {
    connectionState,
    isSupported,
    deviceInfo,
    settings,
    settingsByCategory,
    isStreaming,
    audioData,
    batteryData,
    batteryDebugData,
    connect,
    disconnect,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
    requestBatteryDebug,
  };
}
