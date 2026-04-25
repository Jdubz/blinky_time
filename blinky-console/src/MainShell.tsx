/**
 * MainShell — root UI. Single page, no routes.
 *
 * Layout: header + device strip + generator + effect. Selection state in the
 * device strip drives a Target that every control below dispatches through.
 * "All" (default) = fleet via /api/fleet/*. A specific device = that device's
 * DeviceProtocol over whichever transport it's bound to.
 */
import { useEffect, useState } from 'react';
import { useDevices } from './hooks/useDevices';
import { DeviceStrip } from './components/DeviceStrip';
import { GeneratorSelector } from './components/GeneratorSelector';
import { EffectSelector, type EffectMode } from './components/EffectSelector';
import { ScenesPanel, type Scene } from './components/ScenesPanel';
import { AudioDebugPage } from './pages/AudioDebugPage';
import { DeviceProtocol } from './services/protocol';
import { type Target, targetSetGenerator, targetSetEffect, targetSetSetting } from './lib/target';
import type { GeneratorType } from './types';

const GENERATORS: GeneratorType[] = ['fire', 'water', 'lightning', 'audio'];

export function MainShell() {
  const { devices, serverUrl } = useDevices();
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [generator, setGenerator] = useState<GeneratorType>('fire');
  const [effectMode, setEffectMode] = useState<EffectMode>('off');
  const [hueSpeed, setHueSpeed] = useState(0.5);
  const [hueShift, setHueShift] = useState(0);
  const [showAudioDebug, setShowAudioDebug] = useState(false);

  const selectedDevice = selectedId ? (devices.find(d => d.id === selectedId) ?? null) : null;

  // Protocol lazy-init lives in an effect (not render body) to keep render
  // pure — directly mutating the device object during render violates React's
  // purity contract and can misbehave under concurrent rendering.
  useEffect(() => {
    if (!selectedDevice || selectedDevice.protocol || selectedDevice.transports.length === 0) {
      return;
    }
    selectedDevice.protocol = new DeviceProtocol(selectedDevice.transports[0].transport);
    // `selectedDevice` is derived from `selectedId`; keying on id is correct.
  }, [selectedId]); // eslint-disable-line react-hooks/exhaustive-deps

  if (showAudioDebug) {
    return <AudioDebugPage devices={devices} onClose={() => setShowAudioDebug(false)} />;
  }

  // Target for the current selection. Three shapes:
  //   - fleet: "All" selection or no device selected → dispatch via HTTP
  //   - device: a specific device with an initialized, connected protocol
  //   - device-unavailable: a specific device is selected but its protocol
  //     isn't connectable yet — UI shows the selection but disables dispatch
  //     so we never silently fall back to fleet-wide commands.
  let target: Target;
  let deviceUnavailable = false;
  if (selectedDevice) {
    if (selectedDevice.protocol?.isConnected()) {
      target = { kind: 'device', id: selectedDevice.id, protocol: selectedDevice.protocol };
    } else {
      // Keep fleet target for typing but disable dispatch below.
      target = { kind: 'fleet' };
      deviceUnavailable = true;
    }
  } else {
    target = { kind: 'fleet' };
  }

  const dispatchEnabled = !deviceUnavailable && (target.kind === 'fleet' ? !!serverUrl : true);

  // Every handler wraps dispatch in try/catch. Previously, a
  // SerialError(NOT_CONNECTED) or network error would bubble as an unhandled
  // promise rejection AND leave local state updated as if the command
  // succeeded. Now: apply local state ONLY after the dispatch resolves.
  const handleGenerator = async (gen: GeneratorType) => {
    if (!dispatchEnabled) return;
    try {
      await targetSetGenerator(target, gen);
      setGenerator(gen);
    } catch (err) {
      console.error('Failed to set generator', err);
    }
  };

  const handleEffectMode = async (mode: EffectMode) => {
    if (!dispatchEnabled) return;
    // NOTE: this is a multi-command sequence (effect → speed/hue) which can
    // half-apply if a later step fails — e.g. effect='hue' lands but the
    // huespeed setting errors. Local UI state is only updated on the success
    // path so the user sees they're back in the prior mode, but the device
    // is left with effect='hue' and the old speed/hue. A clean rollback
    // would require a "last known good" snapshot; for now the user can
    // re-pick the desired mode to retry the full sequence.
    try {
      if (mode === 'off') {
        await targetSetEffect(target, 'none');
        setEffectMode(mode);
        return;
      }
      await targetSetEffect(target, 'hue');
      if (mode === 'static') {
        await targetSetSetting(target, 'huespeed', 0);
        await targetSetSetting(target, 'hueshift', hueShift);
      } else {
        await targetSetSetting(target, 'huespeed', hueSpeed);
      }
      setEffectMode(mode);
    } catch (err) {
      console.error('Failed to set effect mode', err);
    }
  };

  const handleSpeedChange = async (s: number) => {
    if (!dispatchEnabled) return;
    setHueSpeed(s); // slider UX: update UI immediately, fire-and-forget dispatch
    if (effectMode === 'rotate') {
      try {
        await targetSetSetting(target, 'huespeed', s);
      } catch (err) {
        console.error('Failed to set huespeed', err);
      }
    }
  };

  const handleHueChange = async (h: number) => {
    if (!dispatchEnabled) return;
    setHueShift(h);
    if (effectMode === 'static') {
      try {
        await targetSetSetting(target, 'hueshift', h);
      } catch (err) {
        console.error('Failed to set hueshift', err);
      }
    }
  };

  return (
    <div className="shell">
      <header className="shell-header">
        <h1 className="shell-title">Blinky Fleet</h1>
        <span className="shell-subtitle">
          {target.kind === 'fleet'
            ? `All · ${devices.filter(d => d.isConnected()).length} connected`
            : (selectedDevice?.displayName ?? 'Device')}
        </span>
        <button
          className="btn btn-small shell-debug-btn"
          onClick={() => setShowAudioDebug(true)}
          title="Open audio stream debug page"
        >
          Audio Debug
        </button>
      </header>

      <DeviceStrip
        devices={devices}
        selectedId={selectedId}
        onSelect={setSelectedId}
        serverReachable={!!serverUrl}
      />

      <main className="shell-body">
        {!dispatchEnabled && (
          <div className="shell-warning">
            {deviceUnavailable
              ? `${selectedDevice?.displayName ?? 'Device'} is not connected. Select "All" or wait for the device to come online.`
              : 'blinky-server not detected. Connect a device via USB or serve the console from blinky-server to use fleet mode.'}
          </div>
        )}

        <GeneratorSelector
          currentGenerator={generator}
          availableGenerators={GENERATORS}
          onGeneratorChange={handleGenerator}
          disabled={!dispatchEnabled}
        />

        <EffectSelector
          mode={effectMode}
          speed={hueSpeed}
          hue={hueShift}
          onModeChange={handleEffectMode}
          onSpeedChange={handleSpeedChange}
          onHueChange={handleHueChange}
          disabled={!dispatchEnabled}
        />

        {serverUrl && (
          <ScenesPanel
            currentGenerator={generator}
            currentEffectMode={effectMode}
            currentHueSpeed={hueSpeed}
            currentHueShift={hueShift}
            onApplied={(scene: Scene) => {
              // Resync local state so UI reflects what the server just pushed.
              setGenerator(scene.generator);
              setEffectMode(scene.effect_mode);
              setHueSpeed(scene.effect_speed);
              setHueShift(scene.effect_hue);
            }}
            disabled={!dispatchEnabled}
          />
        )}
      </main>
    </div>
  );
}
