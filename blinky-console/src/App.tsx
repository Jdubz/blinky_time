import { useState } from 'react';
import { useSerial } from './hooks/useSerial';
import { useNetworkStatus } from './hooks/useNetworkStatus';
import { ConnectionBar } from './components/ConnectionBar';
import { SettingsPanel } from './components/SettingsPanel';
import { AudioVisualizer } from './components/AudioVisualizer';
import { TabView } from './components/TabView';
import { OfflineBanner } from './components/OfflineBanner';
import { SerialConsoleModal } from './components/SerialConsoleModal';
import './styles.css';

function App() {
  const {
    connectionState,
    isSupported,
    deviceInfo,
    settingsByCategory,
    isStreaming,
    audioData,
    batteryData,
    batteryStatusData,
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
                    <AudioVisualizer
                      audioData={audioData}
                      isStreaming={isStreaming}
                      onToggleStreaming={toggleStreaming}
                      disabled={isDisabled}
                    />
                  </div>
                  <div className="tab-panel-settings">
                    <SettingsPanel
                      {...settingsPanelProps}
                      settingsByCategory={{
                        audio: settingsByCategory.audio || [],
                        agc: settingsByCategory.agc || [],
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
                    <SettingsPanel
                      {...settingsPanelProps}
                      settingsByCategory={{
                        fire: settingsByCategory.fire || [],
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
                  <div className="tab-panel-placeholder">
                    <div className="placeholder-content">
                      <span className="placeholder-icon">🎨</span>
                      <h3>No Effect Settings</h3>
                      <p>Effects are currently hardcoded (HueRotation)</p>
                      <p className="placeholder-hint">
                        Future: Configurable effect chains and parameters
                      </p>
                    </div>
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
