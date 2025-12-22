import { useSerial } from './hooks/useSerial';
import { useNetworkStatus } from './hooks/useNetworkStatus';
import { ConnectionBar } from './components/ConnectionBar';
import { SettingsPanel } from './components/SettingsPanel';
import { AudioVisualizer } from './components/AudioVisualizer';
import { OfflineBanner } from './components/OfflineBanner';
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
    connect,
    disconnect,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
  } = useSerial();

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
      />

      <main className="main-content">
        <div className="left-panel">
          <AudioVisualizer
            audioData={audioData}
            batteryData={batteryData}
            isStreaming={isStreaming}
            onToggleStreaming={toggleStreaming}
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
    </div>
  );
}

export default App;
