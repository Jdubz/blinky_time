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
  /** Last error from connect() or stream toggle, or null if none. Cleared on
   *  successful reconnect. Surfaced so callers can render an error state
   *  instead of leaving the UI stuck in "connecting". */
  error: Error | null;
  toggleStreaming: () => Promise<void>;
  onTransientEvent: (callback: (msg: TransientMessage) => void) => () => void;
}

/** Audio sample fields where NaN/Infinity from a malformed firmware frame
 *  would render the chart with broken paths or stuck axes. The protocol
 *  layer's zod schema logs a warning on schema-failed messages but still
 *  emits the parsed object (see DeviceProtocol.handleLine), so we re-check
 *  here before pushing into render state. */
function isAudioSampleFinite(s: AudioSample): boolean {
  return (
    Number.isFinite(s.l) &&
    Number.isFinite(s.t) &&
    Number.isFinite(s.pk) &&
    Number.isFinite(s.vl) &&
    Number.isFinite(s.raw) &&
    Number.isFinite(s.h)
  );
}

function isMusicModeFinite(m: MusicModeData): boolean {
  // All required numeric fields. The optional PLP fields (pp/od/nn/per/ts)
  // are also numeric and may feed chart axes, but the schema's `.optional()`
  // means they may be missing entirely; only validate when present.
  if (
    !Number.isFinite(m.bpm) ||
    !Number.isFinite(m.ph) ||
    !Number.isFinite(m.str) ||
    !Number.isFinite(m.e) ||
    !Number.isFinite(m.p)
  ) {
    return false;
  }
  for (const v of [m.pp, m.od, m.nn, m.per, m.ts, m.conf, m.cb, m.oss]) {
    if (v !== undefined && !Number.isFinite(v)) return false;
  }
  return true;
}

export function useDeviceAudioStream(device: Device | null): UseDeviceAudioStreamReturn {
  const [audioData, setAudioData] = useState<AudioSample | null>(null);
  const [musicModeData, setMusicModeData] = useState<MusicModeData | null>(null);
  const [isStreaming, setIsStreaming] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  const transientCallbacksRef = useRef<Set<(msg: TransientMessage) => void>>(new Set());
  // Track the latest streaming state via ref so cleanup can read it without
  // capturing a stale closure. Only the cleanup path reads this; the render
  // path reads `isStreaming` state directly.
  const isStreamingRef = useRef(false);

  // Effect keys on `device.protocol` as well as `device` so that lazy protocol
  // attachment (`device.protocol = new DeviceProtocol(...)` from a parent's
  // useEffect) re-runs this effect once the protocol exists. Without
  // `device?.protocol` in the dep list the effect only sees the original
  // `protocol = null` snapshot and never connects.
  const protocol = device?.protocol ?? null;

  useEffect(() => {
    setAudioData(null);
    setMusicModeData(null);
    setIsStreaming(false);
    setError(null);

    if (!device || !protocol) {
      setIsConnected(false);
      return;
    }

    setIsConnected(protocol.isConnected());

    const handler = (event: SerialEvent) => {
      switch (event.type) {
        case 'connected':
          setIsConnected(true);
          setError(null);
          break;
        case 'disconnected':
          setIsConnected(false);
          setIsStreaming(false);
          setAudioData(null);
          setMusicModeData(null);
          break;
        case 'audio':
          if (event.audio) {
            if (isAudioSampleFinite(event.audio.a)) {
              setAudioData(event.audio.a);
            } else {
              console.warn('useDeviceAudioStream: dropping non-finite audio sample', event.audio.a);
            }
            if (event.audio.m) {
              if (isMusicModeFinite(event.audio.m)) {
                setMusicModeData(event.audio.m);
              } else {
                console.warn('useDeviceAudioStream: dropping non-finite music data', event.audio.m);
              }
            }
          }
          break;
        case 'transient': {
          const transient = event.transient;
          if (transient) {
            transientCallbacksRef.current.forEach(cb => cb(transient));
          }
          break;
        }
      }
    };

    protocol.addEventListener(handler);

    if (!protocol.isConnected()) {
      protocol.connect().catch(err => {
        console.error('useDeviceAudioStream: connect failed', err);
        setError(err instanceof Error ? err : new Error(String(err)));
      });
    }

    return () => {
      protocol.removeEventListener(handler);
      // Only send the stop command if we actually started a stream — sending
      // it unconditionally produces a spurious wire command for protocols
      // whose firmware tracks stream state explicitly (and is wasted work
      // even when it's a no-op).
      if (protocol.isConnected() && isStreamingRef.current) {
        protocol.setStreamEnabled(false).catch(err => {
          console.warn('useDeviceAudioStream: stream teardown failed', err);
        });
      }
      isStreamingRef.current = false;
    };
  }, [device, protocol]);

  const toggleStreaming = useCallback(async () => {
    if (!device?.protocol?.isConnected()) return;
    const next = !isStreaming;
    try {
      await device.protocol.setStreamEnabled(next);
      isStreamingRef.current = next;
      setIsStreaming(next);
      setError(null);
      if (!next) {
        setAudioData(null);
        setMusicModeData(null);
      }
    } catch (err) {
      console.error('useDeviceAudioStream: setStreamEnabled failed', err);
      setError(err instanceof Error ? err : new Error(String(err)));
      throw err;
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
    error,
    toggleStreaming,
    onTransientEvent,
  };
}
