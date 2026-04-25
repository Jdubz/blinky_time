/**
 * useDeviceAudioStream — subscribe to one Device's audio stream.
 *
 * Bypasses the singleton serialService used by useSerial(); attaches directly
 * to the per-device DeviceProtocol from the registry. This keeps the audio
 * debug page self-contained — switching devices doesn't disturb whatever the
 * rest of the app is bound to.
 *
 * Connect lifecycle: the hook calls protocol.connect() on mount when the
 * underlying transport isn't already open (server WebSocket transports stay
 * open across the app lifetime, so this is usually a no-op). On unmount, the
 * stream is stopped but the connection is left alone.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import type { Device } from '../services/sources';
import type { SerialEvent } from '../services/protocol';
import type { AudioSample, MusicModeData, TransientMessage } from '../types';

export interface UseDeviceAudioStreamReturn {
  audioData: AudioSample | null;
  musicModeData: MusicModeData | null;
  isStreaming: boolean;
  isConnected: boolean;
  toggleStreaming: () => Promise<void>;
  onTransientEvent: (callback: (msg: TransientMessage) => void) => () => void;
}

export function useDeviceAudioStream(device: Device | null): UseDeviceAudioStreamReturn {
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [musicModeData, setMusicModeData] = useState<MusicModeData | null>(null);
  const [isStreaming, setIsStreaming] = useState(false);
  const [isConnected, setIsConnected] = useState(false);

  const transientCallbacksRef = useRef<Set<(msg: TransientMessage) => void>>(new Set());

  useEffect(() => {
    setAudioData(null);
    setMusicModeData(null);
    setIsStreaming(false);

    if (!device || !device.protocol) {
      setIsConnected(false);
      return;
    }

    const protocol = device.protocol;
    setIsConnected(protocol.isConnected());

    const handler = (event: SerialEvent) => {
      switch (event.type) {
        case 'connected':
          setIsConnected(true);
          break;
        case 'disconnected':
          setIsConnected(false);
          setIsStreaming(false);
          setAudioData(null);
          setMusicModeData(null);
          break;
        case 'audio':
          if (event.audio) {
            setAudioData(event.audio.a);
            if (event.audio.m) setMusicModeData(event.audio.m);
          }
          break;
        case 'transient':
          if (event.transient) {
            transientCallbacksRef.current.forEach(cb => cb(event.transient!));
          }
          break;
      }
    };

    protocol.addEventListener(handler);

    if (!protocol.isConnected()) {
      protocol.connect().catch(err => {
        console.error('useDeviceAudioStream: connect failed', err);
      });
    }

    return () => {
      protocol.removeEventListener(handler);
      if (protocol.isConnected()) {
        protocol.setStreamEnabled(false).catch(() => {});
      }
    };
  }, [device]);

  const toggleStreaming = useCallback(async () => {
    if (!device?.protocol?.isConnected()) return;
    const next = !isStreaming;
    await device.protocol.setStreamEnabled(next);
    setIsStreaming(next);
    if (!next) {
      setAudioData(null);
      setMusicModeData(null);
    }
  }, [device, isStreaming]);

  const onTransientEvent = useCallback((callback: (msg: TransientMessage) => void) => {
    transientCallbacksRef.current.add(callback);
    return () => {
      transientCallbacksRef.current.delete(callback);
    };
  }, []);

  return {
    audioData,
    musicModeData,
    isStreaming,
    isConnected,
    toggleStreaming,
    onTransientEvent,
  };
}
