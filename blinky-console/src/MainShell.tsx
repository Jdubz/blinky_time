/**
 * MainShell — root UI. Single page, no routes.
 *
 * Layout: header + device strip + generator + effect. Selection state in the
 * device strip drives a Target that every control below dispatches through.
 * "All" (default) = fleet via /api/fleet/*. A specific device = that device's
 * DeviceProtocol over whichever transport it's bound to.
 */
import { useState } from 'react';
import { useDevices } from './hooks/useDevices';
import { DeviceStrip } from './components/DeviceStrip';
import { GeneratorSelector } from './components/GeneratorSelector';
import { EffectSelector, type EffectMode } from './components/EffectSelector';
import { ScenesPanel, type Scene } from './components/ScenesPanel';
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

  // Build the Target for the current selection.
  const selectedDevice = selectedId ? (devices.find(d => d.id === selectedId) ?? null) : null;
  if (selectedDevice && !selectedDevice.protocol && selectedDevice.transports.length > 0) {
    // Lazy-init the protocol on first selection so Target can dispatch.
    selectedDevice.protocol = new DeviceProtocol(selectedDevice.transports[0].transport);
  }

  const target: Target =
    selectedDevice && selectedDevice.protocol
      ? { kind: 'device', id: selectedDevice.id, protocol: selectedDevice.protocol }
      : { kind: 'fleet' };

  const dispatchEnabled = target.kind === 'fleet' ? !!serverUrl : true;

  const handleGenerator = async (gen: GeneratorType) => {
    setGenerator(gen);
    await targetSetGenerator(target, gen);
  };

  const handleEffectMode = async (mode: EffectMode) => {
    setEffectMode(mode);
    if (mode === 'off') {
      await targetSetEffect(target, 'none');
      return;
    }
    await targetSetEffect(target, 'hue');
    if (mode === 'static') {
      await targetSetSetting(target, 'huespeed', 0);
      await targetSetSetting(target, 'hueshift', hueShift);
    } else {
      await targetSetSetting(target, 'huespeed', hueSpeed);
    }
  };

  const handleSpeedChange = async (s: number) => {
    setHueSpeed(s);
    if (effectMode === 'rotate') {
      await targetSetSetting(target, 'huespeed', s);
    }
  };

  const handleHueChange = async (h: number) => {
    setHueShift(h);
    if (effectMode === 'static') {
      await targetSetSetting(target, 'hueshift', h);
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
            blinky-server not detected. Connect a device via USB or serve the console from
            blinky-server to use fleet mode.
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
