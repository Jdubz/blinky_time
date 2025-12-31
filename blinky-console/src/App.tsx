import { useState } from 'react';
import { useSerial } from './hooks/useSerial';
import { useNetworkStatus } from './hooks/useNetworkStatus';
import { ConnectionBar } from './components/ConnectionBar';
import { SettingsPanel } from './components/SettingsPanel';
import { AudioVisualizer } from './components/AudioVisualizer';
import { TabView } from './components/TabView';
import { OfflineBanner } from './components/OfflineBanner';
import { SerialConsoleModal } from './components/SerialConsoleModal';
import { ErrorBoundary } from './components/ErrorBoundary';
import { GeneratorSelector } from './components/GeneratorSelector';
import { EffectSelector } from './components/EffectSelector';
import './styles.css';

function App() {
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
    presets,
    currentPreset,
    isStreaming,
    audioData,
    batteryData,
    batteryStatusData,
    rhythmData,
    musicModeData,
    statusData,
    onPercussionEvent,
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
    applyPreset,
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
    presets,
    currentPreset,
    onApplyPreset: applyPreset,
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
                            <div className="error-boundary-icon">⚠️</div>
                            <h3>Audio Visualizer Error</h3>
                            <p>
                              The audio visualizer encountered an error. Try restarting the stream.
                            </p>
                            <button
                              className="btn btn-primary"
                              onClick={() => window.location.reload()}
                            >
                              Reload Page
                            </button>
                          </div>
                        </div>
                      }
                    >
                      <AudioVisualizer
                        audioData={audioData}
                        rhythmData={rhythmData}
                        musicModeData={musicModeData}
                        statusData={statusData}
                        isStreaming={isStreaming}
                        onToggleStreaming={toggleStreaming}
                        disabled={isDisabled}
                        onPercussionEvent={onPercussionEvent}
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
                        [currentGenerator]: settingsByCategory[currentGenerator] || [],
                        // Include related categories for fire generator
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

export default App;
