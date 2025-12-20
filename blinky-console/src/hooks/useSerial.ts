import { useState, useEffect, useCallback, useMemo } from 'react';
import { serialService, SerialEvent } from '../services/serial';
import {
  DeviceInfo,
  DeviceSetting,
  AudioSample,
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

  // Audio streaming
  isStreaming: boolean;
  audioData: AudioSample | null;

  // Actions
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  setSetting: (name: string, value: number | boolean) => Promise<void>;
  toggleStreaming: () => Promise<void>;
  saveSettings: () => Promise<void>;
  loadSettings: () => Promise<void>;
  resetDefaults: () => Promise<void>;
  refreshSettings: () => Promise<void>;
}

export function useSerial(): UseSerialReturn {
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [settings, setSettings] = useState<DeviceSetting[]>([]);
  const [isStreaming, setIsStreaming] = useState(false);
  const [audioData, setAudioData] = useState<AudioSample | null>(null);

  const isSupported = serialService.isSupported();

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
          break;
        case 'audio':
          if (event.audio) {
            setAudioData(event.audio.a);
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

  return {
    connectionState,
    isSupported,
    deviceInfo,
    settings,
    settingsByCategory,
    isStreaming,
    audioData,
    connect,
    disconnect,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
  };
}
