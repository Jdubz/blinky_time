import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderHook, act, waitFor } from '@testing-library/react';
import { useSerial } from './useSerial';
import { serialService, SerialEvent, SerialEventCallback } from '../services/serial';
import type { AudioSample } from '../types';

// Mock the serial service
vi.mock('../services/serial', () => {
  const listeners: SerialEventCallback[] = [];

  return {
    serialService: {
      isSupported: vi.fn(() => true),
      connect: vi.fn(() => Promise.resolve(true)),
      disconnect: vi.fn(() => Promise.resolve()),
      send: vi.fn(() => Promise.resolve()),
      getDeviceInfo: vi.fn(() =>
        Promise.resolve({
          device: 'Blinky Time',
          version: '1.0.0',
          width: 16,
          height: 16,
          leds: 256,
        })
      ),
      getSettings: vi.fn(() =>
        Promise.resolve({
          settings: [
            { name: 'intensity', value: 0.75, type: 'float', cat: 'fire', min: 0, max: 1 },
            { name: 'enabled', value: true, type: 'bool', cat: 'audio', min: 0, max: 1 },
          ],
        })
      ),
      setSetting: vi.fn(() => Promise.resolve()),
      setStreamEnabled: vi.fn(() => Promise.resolve()),
      saveSettings: vi.fn(() => Promise.resolve()),
      loadSettings: vi.fn(() => Promise.resolve()),
      resetDefaults: vi.fn(() => Promise.resolve()),
      addEventListener: vi.fn((callback: SerialEventCallback) => {
        listeners.push(callback);
      }),
      removeEventListener: vi.fn((callback: SerialEventCallback) => {
        const index = listeners.indexOf(callback);
        if (index > -1) listeners.splice(index, 1);
      }),
      // Helper to emit events in tests
      _emit: (event: SerialEvent) => {
        listeners.forEach(cb => cb(event));
      },
      _listeners: listeners,
    },
  };
});

describe('useSerial', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    (serialService as unknown as { _listeners: SerialEventCallback[] })._listeners.length = 0;
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  describe('initial state', () => {
    it('returns disconnected state initially', () => {
      const { result } = renderHook(() => useSerial());

      expect(result.current.connectionState).toBe('disconnected');
      expect(result.current.deviceInfo).toBeNull();
      expect(result.current.settings).toEqual([]);
      expect(result.current.isStreaming).toBe(false);
      expect(result.current.audioData).toBeNull();
    });

    it('checks WebSerial support', () => {
      const { result } = renderHook(() => useSerial());
      expect(result.current.isSupported).toBe(true);
      expect(serialService.isSupported).toHaveBeenCalled();
    });
  });

  describe('connect', () => {
    it('sets connecting state when connect is called', async () => {
      const { result } = renderHook(() => useSerial());

      act(() => {
        result.current.connect();
      });

      expect(result.current.connectionState).toBe('connecting');
    });

    it('fetches device info and settings on successful connect', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.connect();
      });

      expect(serialService.getDeviceInfo).toHaveBeenCalled();
      expect(serialService.getSettings).toHaveBeenCalled();
      expect(result.current.deviceInfo).toEqual({
        device: 'Blinky Time',
        version: '1.0.0',
        width: 16,
        height: 16,
        leds: 256,
      });
      expect(result.current.settings).toHaveLength(2);
    });
  });

  describe('disconnect', () => {
    it('calls serialService.disconnect', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.disconnect();
      });

      expect(serialService.disconnect).toHaveBeenCalled();
    });

    it('stops streaming before disconnecting if streaming', async () => {
      const { result } = renderHook(() => useSerial());

      // Start streaming first
      await act(async () => {
        await result.current.toggleStreaming();
      });

      await act(async () => {
        await result.current.disconnect();
      });

      expect(serialService.setStreamEnabled).toHaveBeenCalledWith(false);
    });
  });

  describe('setSetting', () => {
    it('calls serialService.setSetting and updates local state', async () => {
      const { result } = renderHook(() => useSerial());

      // First connect to get settings
      await act(async () => {
        await result.current.connect();
      });

      await act(async () => {
        await result.current.setSetting('intensity', 0.5);
      });

      expect(serialService.setSetting).toHaveBeenCalledWith('intensity', 0.5);

      // Check local state was updated
      const intensitySetting = result.current.settings.find(s => s.name === 'intensity');
      expect(intensitySetting?.value).toBe(0.5);
    });
  });

  describe('toggleStreaming', () => {
    it('toggles streaming state', async () => {
      const { result } = renderHook(() => useSerial());

      expect(result.current.isStreaming).toBe(false);

      await act(async () => {
        await result.current.toggleStreaming();
      });

      expect(result.current.isStreaming).toBe(true);
      expect(serialService.setStreamEnabled).toHaveBeenCalledWith(true);

      await act(async () => {
        await result.current.toggleStreaming();
      });

      expect(result.current.isStreaming).toBe(false);
      expect(serialService.setStreamEnabled).toHaveBeenCalledWith(false);
    });

    it('clears audio data when streaming stops', async () => {
      const { result } = renderHook(() => useSerial());

      // Start streaming
      await act(async () => {
        await result.current.toggleStreaming();
      });

      // Simulate receiving audio data
      act(() => {
        (serialService as unknown as { _emit: (e: SerialEvent) => void })._emit({
          type: 'audio',
          audio: {
            a: {
              l: 0.5,
              t: 0.3,
              pk: 0.4,
              vl: 0.05,
              raw: 0.2,
              h: 32,
              alive: 1,
              lo: 0,
              hi: 0,
              los: 0.0,
              his: 0.0,
              z: 0.1,
            },
          },
        });
      });

      expect(result.current.audioData).not.toBeNull();

      // Stop streaming
      await act(async () => {
        await result.current.toggleStreaming();
      });

      expect(result.current.audioData).toBeNull();
    });
  });

  describe('saveSettings', () => {
    it('calls serialService.saveSettings', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.saveSettings();
      });

      expect(serialService.saveSettings).toHaveBeenCalled();
    });
  });

  describe('loadSettings', () => {
    it('calls serialService.loadSettings and refreshes', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.loadSettings();
      });

      expect(serialService.loadSettings).toHaveBeenCalled();
      expect(serialService.getSettings).toHaveBeenCalled();
    });
  });

  describe('resetDefaults', () => {
    it('calls serialService.resetDefaults and refreshes', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.resetDefaults();
      });

      expect(serialService.resetDefaults).toHaveBeenCalled();
      expect(serialService.getSettings).toHaveBeenCalled();
    });
  });

  describe('refreshSettings', () => {
    it('fetches settings again', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.refreshSettings();
      });

      expect(serialService.getSettings).toHaveBeenCalled();
    });
  });

  describe('event handling', () => {
    it('handles connected event', async () => {
      const { result } = renderHook(() => useSerial());

      act(() => {
        (serialService as unknown as { _emit: (e: SerialEvent) => void })._emit({
          type: 'connected',
        });
      });

      await waitFor(() => {
        expect(result.current.connectionState).toBe('connected');
      });
    });

    it('handles disconnected event', async () => {
      const { result } = renderHook(() => useSerial());

      // First set some state
      await act(async () => {
        await result.current.connect();
      });

      act(() => {
        (serialService as unknown as { _emit: (e: SerialEvent) => void })._emit({
          type: 'disconnected',
        });
      });

      await waitFor(() => {
        expect(result.current.connectionState).toBe('disconnected');
        expect(result.current.deviceInfo).toBeNull();
        expect(result.current.settings).toEqual([]);
        expect(result.current.isStreaming).toBe(false);
      });
    });

    it('handles audio event', async () => {
      const { result } = renderHook(() => useSerial());

      const audioSample: AudioSample = {
        l: 0.5,
        t: 0.3,
        pk: 0.4,
        vl: 0.05,
        raw: 0.2,
        h: 32,
        alive: 1,
        lo: 0,
        hi: 0,
        los: 0.0,
        his: 0.0,
        z: 0.1,
      };

      act(() => {
        (serialService as unknown as { _emit: (e: SerialEvent) => void })._emit({
          type: 'audio',
          audio: { a: audioSample },
        });
      });

      await waitFor(() => {
        expect(result.current.audioData).toEqual(audioSample);
      });
    });

    it('handles error event', async () => {
      const { result } = renderHook(() => useSerial());

      act(() => {
        (serialService as unknown as { _emit: (e: SerialEvent) => void })._emit({
          type: 'error',
          error: new Error('Test error'),
        });
      });

      await waitFor(() => {
        expect(result.current.connectionState).toBe('error');
      });
    });
  });

  describe('settingsByCategory', () => {
    it('groups settings by category', async () => {
      const { result } = renderHook(() => useSerial());

      await act(async () => {
        await result.current.connect();
      });

      expect(result.current.settingsByCategory.fire).toBeDefined();
      expect(result.current.settingsByCategory.audio).toBeDefined();
      expect(result.current.settingsByCategory.fire[0].name).toBe('intensity');
      expect(result.current.settingsByCategory.audio[0].name).toBe('enabled');
    });
  });
});
