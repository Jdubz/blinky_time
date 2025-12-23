import { useState } from 'react';
import { useSerial } from './hooks/useSerial';
import { useNetworkStatus } from './hooks/useNetworkStatus';
import { ConnectionBar } from './components/ConnectionBar';
import { SettingsPanel } from './components/SettingsPanel';
import { AudioVisualizer } from './components/AudioVisualizer';
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

  return (
    <div className="app">
      <OfflineBanner isOnline={isOnline} />
      <ConnectionBar
        connectionState={connectionState}
        deviceInfo={deviceInfo}
        isSupported={isSupported}
        onConnect={connect}
        onDisconnect={disconnect}
        onOpenConsole={() => setIsConsoleOpen(true)}
      />

      <main className="main-content">
        <div className="left-panel">
          <AudioVisualizer
            audioData={audioData}
            batteryData={batteryData}
            batteryStatusData={batteryStatusData}
            isStreaming={isStreaming}
            onToggleStreaming={toggleStreaming}
            onRequestBatteryStatus={requestBatteryStatus}
            disabled={isDisabled}
          />
        </div>

        <div className="right-panel">
          <SettingsPanel
            settingsByCategory={settingsByCategory}
            onSettingChange={setSetting}
            onSave={saveSettings}
            onLoad={loadSettings}
            onReset={resetDefaults}
            onRefresh={refreshSettings}
            disabled={isDisabled}
          />
        </div>
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
