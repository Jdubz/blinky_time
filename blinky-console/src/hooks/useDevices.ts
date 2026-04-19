/**
 * useDevices — React hook for the device registry.
 *
 * Subscribes to the DeviceRegistry singleton and returns the current device
 * list. Re-renders when devices are added, removed, or merged.
 *
 * Also initializes same-origin server detection on first mount.
 */

import { useEffect, useRef, useState } from 'react';
import { deviceRegistry } from '../services/sources';
import { detectSameOriginServer } from '../services/sources/BlinkyServerSource';
import type { Device } from '../services/sources';
import type { BlinkyServerSource } from '../services/sources/BlinkyServerSource';

export function useDevices() {
  const [devices, setDevices] = useState<Device[]>(deviceRegistry.list());
  const serverSourceRef = useRef<BlinkyServerSource | null>(null);

  useEffect(() => {
    // Subscribe to registry changes
    const unsubscribe = deviceRegistry.subscribe(setDevices);

    // Auto-detect same-origin blinky-server on first mount
    detectSameOriginServer(deviceRegistry).then(source => {
      serverSourceRef.current = source;
    });

    return () => {
      unsubscribe();
      serverSourceRef.current?.stop();
    };
  }, []);

  return {
    devices,
    registry: deviceRegistry,
    /** URL of the active blinky-server, or null if no server source is active. */
    serverUrl: serverSourceRef.current ? window.location.origin : null,
  };
}
