import { useState, useEffect, useCallback, useRef } from 'react';
import { serialService, SerialEvent } from '../services/serial';
import {
  DeviceInfo,
  DeviceSetting,
  AudioSample,
  ConnectionState,
  ConsoleEntry,
  SettingsByCategory
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

  // Console
  consoleLog: ConsoleEntry[];

  // Actions
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  sendCommand: (command: string) => Promise<void>;
  setSetting: (name: string, value: number | boolean) => Promise<void>;
  toggleStreaming: () => Promise<void>;
  saveSettings: () => Promise<void>;
  loadSettings: () => Promise<void>;
  resetDefaults: () => Promise<void>;
  refreshSettings: () => Promise<void>;
  clearConsole: () => void;
}

export function useSerial(): UseSerialReturn {
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null);
  const [settings, setSettings] = useState<DeviceSetting[]>([]);
  const [isStreaming, setIsStreaming] = useState(false);
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [consoleLog, setConsoleLog] = useState<ConsoleEntry[]>([]);

  const consoleIdRef = useRef(0);
  const isSupported = serialService.isSupported();

  // Add console entry
  const addConsoleEntry = useCallback((type: ConsoleEntry['type'], message: string) => {
    const entry: ConsoleEntry = {
      id: ++consoleIdRef.current,
      timestamp: new Date(),
      type,
      message
    };
    setConsoleLog(prev => [...prev.slice(-200), entry]); // Keep last 200 entries
  }, []);

  // Group settings by category
  const settingsByCategory: SettingsByCategory = settings.reduce((acc, setting) => {
    const cat = setting.cat || 'other';
    if (!acc[cat]) acc[cat] = [];
    acc[cat].push(setting);
    return acc;
  }, {} as SettingsByCategory);

  // Handle serial events
  useEffect(() => {
    const handleEvent = (event: SerialEvent) => {
      switch (event.type) {
        case 'connected':
          setConnectionState('connected');
          addConsoleEntry('info', 'Connected to device');
          break;
        case 'disconnected':
          setConnectionState('disconnected');
          setDeviceInfo(null);
          setSettings([]);
          setIsStreaming(false);
          setAudioData(null);
          addConsoleEntry('info', 'Disconnected from device');
          break;
        case 'data':
          if (event.data) {
            addConsoleEntry('received', event.data);
          }
          break;
        case 'audio':
          if (event.audio) {
            setAudioData(event.audio.a);
          }
          break;
        case 'error':
          setConnectionState('error');
          if (event.error) {
            addConsoleEntry('error', event.error.message);
          }
          break;
      }
    };

    serialService.addEventListener(handleEvent);
    return () => serialService.removeEventListener(handleEvent);
  }, [addConsoleEntry]);

  // Connect to device
  const connect = useCallback(async () => {
    setConnectionState('connecting');
    const success = await serialService.connect();

    if (success) {
      // Fetch device info
      addConsoleEntry('sent', 'json info');
      const info = await serialService.getDeviceInfo();
      if (info) {
        setDeviceInfo(info);
      }

      // Fetch settings
      addConsoleEntry('sent', 'json settings');
      const settingsResponse = await serialService.getSettings();
      if (settingsResponse) {
        setSettings(settingsResponse.settings);
      }
    }
  }, [addConsoleEntry]);

  // Disconnect from device
  const disconnect = useCallback(async () => {
    if (isStreaming) {
      await serialService.setStreamEnabled(false);
    }
    await serialService.disconnect();
  }, [isStreaming]);

  // Send raw command
  const sendCommand = useCallback(async (command: string) => {
    addConsoleEntry('sent', command);
    await serialService.send(command);
  }, [addConsoleEntry]);

  // Set a setting value
  const setSetting = useCallback(async (name: string, value: number | boolean) => {
    await serialService.setSetting(name, value);
    // Update local state
    setSettings(prev => prev.map(s =>
      s.name === name ? { ...s, value } : s
    ));
  }, []);

  // Toggle audio streaming
  const toggleStreaming = useCallback(async () => {
    const newState = !isStreaming;
    addConsoleEntry('sent', newState ? 'stream on' : 'stream off');
    await serialService.setStreamEnabled(newState);
    setIsStreaming(newState);
    if (!newState) {
      setAudioData(null);
    }
  }, [isStreaming, addConsoleEntry]);

  // Save settings to flash
  const saveSettings = useCallback(async () => {
    addConsoleEntry('sent', 'save');
    await serialService.saveSettings();
  }, [addConsoleEntry]);

  // Load settings from flash
  const loadSettings = useCallback(async () => {
    addConsoleEntry('sent', 'load');
    await serialService.loadSettings();
    // Refresh settings after load
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, [addConsoleEntry]);

  // Reset to defaults
  const resetDefaults = useCallback(async () => {
    addConsoleEntry('sent', 'defaults');
    await serialService.resetDefaults();
    // Refresh settings after reset
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, [addConsoleEntry]);

  // Refresh settings
  const refreshSettings = useCallback(async () => {
    addConsoleEntry('sent', 'json settings');
    const settingsResponse = await serialService.getSettings();
    if (settingsResponse) {
      setSettings(settingsResponse.settings);
    }
  }, [addConsoleEntry]);

  // Clear console
  const clearConsole = useCallback(() => {
    setConsoleLog([]);
  }, []);

  return {
    connectionState,
    isSupported,
    deviceInfo,
    settings,
    settingsByCategory,
    isStreaming,
    audioData,
    consoleLog,
    connect,
    disconnect,
    sendCommand,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
    clearConsole
  };
}
