/**
 * DeviceDetail — single-device view bound to a specific device from the registry.
 *
 * Reads deviceId from the route param, looks up the Device in the registry,
 * and binds its protocol to the legacy serialService so useSerial works.
 * When the route changes to a different device, the protocol is rebound.
 */

import { useState, useEffect } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useSerial } from '../hooks/useSerial';
import { deviceRegistry } from '../services/sources';
import { serialService } from '../services/serial';
import { DeviceProtocol } from '../services/protocol';
import { logger } from '../lib/logger';
import { useNetworkStatus } from '../hooks/useNetworkStatus';
import { ConnectionBar } from '../components/ConnectionBar';
import { SettingsPanel } from '../components/SettingsPanel';
import { AudioVisualizer } from '../components/AudioVisualizer';
import { TabView } from '../components/TabView';
import { OfflineBanner } from '../components/OfflineBanner';
import { SerialConsoleModal } from '../components/SerialConsoleModal';
import { ErrorBoundary } from '../components/ErrorBoundary';
import { GeneratorSelector } from '../components/GeneratorSelector';
import { EffectSelector } from '../components/EffectSelector';

export function DeviceDetail() {
  const { deviceId } = useParams<{ deviceId: string }>();
  const navigate = useNavigate();

  // Bind the registry device's protocol to serialService so useSerial works.
  // When deviceId changes, rebind. On unmount, unbind to prevent stale refs.
  useEffect(() => {
    if (!deviceId) return;
    const device = deviceRegistry.get(deviceId);
    if (!device) {
      logger.warn('DeviceDetail: device not found in registry, redirecting', { deviceId });
      navigate('/', { replace: true });
      return;
    }

    // Ensure the device has a protocol. If not (e.g., discovered by
    // BlinkyServerSource polling but never connected), create one from
    // the first available transport. This is a one-time initialization;
    // subsequent navigations to the same device reuse the existing protocol.
    if (!device.protocol && device.transports.length > 0) {
      device.protocol = new DeviceProtocol(device.transports[0].transport);
    }

    if (device.protocol) {
      serialService.bindProtocol(device.protocol);
    }

    return () => {
      // Unbind on unmount so serialService doesn't keep a stale reference
      // to this device's protocol after navigation.
      serialService.unbind();
    };
  }, [deviceId, navigate]);

  // For now, useSerial still manages the single active device.
  // Phase 4 M12 will refactor this to use the registry's Device directly.
  const {
    connectionState,
    isSupported,
    errorMessage,
    deviceInfo,
    settingsByCategory,
    currentGenerator,
    currentEffect,
    availableGenerators,
    availableEffects,
    isStreaming,
    audioData,
    batteryData,
    batteryStatusData,
    musicModeData,
    onTransientEvent,
    consoleLines,
    sendCommand,
    connect,
    disconnect,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
    requestBatteryStatus,
    setGenerator,
    setEffect,
  } = useSerial();

  const [isConsoleOpen, setIsConsoleOpen] = useState(false);
  const isOnline = useNetworkStatus();
  const isDisabled = connectionState !== 'connected';

  const settingsPanelProps = {
    onSettingChange: setSetting,
    onSave: saveSettings,
    onLoad: loadSettings,
    onReset: resetDefaults,
    onRefresh: refreshSettings,
    disabled: isDisabled,
  };

  return (
    <div className="app">
      <OfflineBanner isOnline={isOnline} />
      <ConnectionBar
        connectionState={connectionState}
        deviceInfo={deviceInfo}
        batteryData={batteryData}
        batteryStatusData={batteryStatusData}
        isSupported={isSupported}
        errorMessage={errorMessage}
        onConnect={connect}
        onDisconnect={disconnect}
        onOpenConsole={() => setIsConsoleOpen(true)}
        onRequestBatteryStatus={requestBatteryStatus}
        onBackToList={() => navigate('/')}
      />

      <main className="main-content">
        <TabView
          tabs={[
            {
              id: 'inputs',
              label: 'Inputs',
              content: (
                <div className="tab-panel">
                  <div className="tab-panel-visualizer">
                    <ErrorBoundary
                      fallback={
                        <div className="audio-visualizer-error">
                          <div className="error-boundary-content">
                            <div className="error-boundary-icon">!</div>
                            <h3>Audio Visualizer Error</h3>
                            <p>The audio visualizer encountered an error.</p>
                            <button
                              className="btn btn-primary"
                              onClick={() => window.location.reload()}
                            >
                              Reload
                            </button>
                          </div>
                        </div>
                      }
                    >
                      <AudioVisualizer
                        audioData={audioData}
                        musicModeData={musicModeData}
                        isStreaming={isStreaming}
                        onToggleStreaming={toggleStreaming}
                        disabled={isDisabled}
                        onTransientEvent={onTransientEvent}
                        connectionState={connectionState}
                      />
                    </ErrorBoundary>
                  </div>
                  <div className="tab-panel-settings">
                    <SettingsPanel
                      {...settingsPanelProps}
                      settingsByCategory={{
                        audio: settingsByCategory.audio || [],
                        agc: settingsByCategory.agc || [],
                        freq: settingsByCategory.freq || [],
                      }}
                    />
                  </div>
                </div>
              ),
            },
            {
              id: 'generators',
              label: 'Generators',
              content: (
                <div className="tab-panel">
                  <div className="tab-panel-settings-full">
                    <GeneratorSelector
                      currentGenerator={currentGenerator}
                      availableGenerators={availableGenerators}
                      onGeneratorChange={setGenerator}
                      disabled={isDisabled}
                    />
                    <SettingsPanel
                      {...settingsPanelProps}
                      settingsByCategory={{
                        ...(currentGenerator === 'audio'
                          ? { audiovis: settingsByCategory.audiovis || [] }
                          : {
                              [currentGenerator]: settingsByCategory[currentGenerator] || [],
                            }),
                        ...(currentGenerator === 'fire' && {
                          firemusic: settingsByCategory.firemusic || [],
                          fireorganic: settingsByCategory.fireorganic || [],
                        }),
                      }}
                    />
                  </div>
                </div>
              ),
            },
            {
              id: 'effects',
              label: 'Effects',
              content: (
                <div className="tab-panel">
                  <div className="tab-panel-settings-full">
                    <EffectSelector
                      currentEffect={currentEffect}
                      availableEffects={availableEffects}
                      onEffectChange={setEffect}
                      disabled={isDisabled}
                    />
                    {currentEffect === 'none' ? (
                      <div className="effect-info">
                        <p className="effect-description">
                          No effect applied - using original generator colors.
                        </p>
                      </div>
                    ) : (
                      <SettingsPanel
                        {...settingsPanelProps}
                        settingsByCategory={{
                          effect: settingsByCategory.effect || [],
                        }}
                      />
                    )}
                  </div>
                </div>
              ),
            },
          ]}
        />
      </main>

      <SerialConsoleModal
        isOpen={isConsoleOpen}
        onClose={() => setIsConsoleOpen(false)}
        onSendCommand={sendCommand}
        consoleLines={consoleLines}
        disabled={isDisabled}
      />
    </div>
  );
}
