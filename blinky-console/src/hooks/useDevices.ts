/**
 * useDevices — React hook for the device registry.
 *
 * Subscribes to the DeviceRegistry singleton and returns the current device
 * list. Re-renders when devices are added, removed, or merged.
 *
 * Also initializes same-origin server detection on first mount.
 */

import { useEffect, useState } from 'react';
import { deviceRegistry } from '../services/sources';
import { detectSameOriginServer } from '../services/sources/BlinkyServerSource';
import type { Device } from '../services/sources';
import type { BlinkyServerSource } from '../services/sources/BlinkyServerSource';

export function useDevices() {
  const [devices, setDevices] = useState<Device[]>(deviceRegistry.list());
  // useState (not useRef) so the Fleet route re-renders when detection succeeds.
  // With useRef the assignment never triggered a render and serverUrl stayed null
  // even after the server had been detected and was streaming devices.
  const [serverSource, setServerSource] = useState<BlinkyServerSource | null>(null);

  useEffect(() => {
    const unsubscribe = deviceRegistry.subscribe(setDevices);

    let detected: BlinkyServerSource | null = null;
    detectSameOriginServer(deviceRegistry).then(source => {
      detected = source;
      setServerSource(source);
    });

    return () => {
      unsubscribe();
      detected?.stop();
    };
  }, []);

  return {
    devices,
    registry: deviceRegistry,
    /** URL of the active blinky-server, or null if no server source is active. */
    serverUrl: serverSource ? window.location.origin : null,
  };
}
