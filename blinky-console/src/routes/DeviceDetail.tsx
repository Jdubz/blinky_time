/**
 * DeviceDetail — single-device view (pre-multi-device architecture).
 *
 * Route param deviceId is currently ignored — uses the legacy useSerial
 * singleton which manages a single global connection. The URL is decorative;
 * whichever device useSerial is connected to is shown regardless of route.
 *
 * M12 will refactor to use the registry's Device directly and respect
 * the route parameter.
 */

import { useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useSerial } from '../hooks/useSerial';
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
  // TODO(M12): Use deviceId to select the correct device from the registry
  // instead of the legacy serialService singleton.
  useParams<{ deviceId: string }>();
  const navigate = useNavigate();

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
                              [currentGenerator]:
                                settingsByCategory[currentGenerator] || [],
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
