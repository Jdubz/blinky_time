import { useSerial } from './hooks/useSerial';
import { ConnectionBar } from './components/ConnectionBar';
import { SettingsPanel } from './components/SettingsPanel';
import { AudioVisualizer } from './components/AudioVisualizer';
import { Console } from './components/Console';
import './styles.css';

function App() {
  const {
    connectionState,
    isSupported,
    deviceInfo,
    settingsByCategory,
    isStreaming,
    audioData,
    consoleLog,
    connect,
    disconnect,
    sendCommand,
    setSetting,
    toggleStreaming,
    saveSettings,
    loadSettings,
    resetDefaults,
    refreshSettings,
    clearConsole
  } = useSerial();

  const isDisabled = connectionState !== 'connected';

  return (
    <div className="app">
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
            isStreaming={isStreaming}
            onToggleStreaming={toggleStreaming}
            disabled={isDisabled}
          />
          <Console
            entries={consoleLog}
            onSendCommand={sendCommand}
            onClear={clearConsole}
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
