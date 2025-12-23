import { DeviceInfo, ConnectionState } from '../types';

interface ConnectionBarProps {
  connectionState: ConnectionState;
  deviceInfo: DeviceInfo | null;
  isSupported: boolean;
  onConnect: () => void;
  onDisconnect: () => void;
  onOpenConsole: () => void;
}

export function ConnectionBar({
  connectionState,
  deviceInfo,
  isSupported,
  onConnect,
  onDisconnect,
  onOpenConsole,
}: ConnectionBarProps) {
  const getStatusColor = () => {
    switch (connectionState) {
      case 'connected':
        return '#4ade80';
      case 'connecting':
        return '#facc15';
      case 'error':
        return '#f87171';
      default:
        return '#6b7280';
    }
  };

  const getStatusText = () => {
    switch (connectionState) {
      case 'connected':
        return 'Connected';
      case 'connecting':
        return 'Connecting...';
      case 'error':
        return 'Error';
      default:
        return 'Disconnected';
    }
  };

  if (!isSupported) {
    return (
      <div className="connection-bar error">
        <span className="warning-icon">&#9888;</span>
        <span>WebSerial not supported. Please use Chrome, Edge, or Opera.</span>
      </div>
    );
  }

  return (
    <div className="connection-bar">
      <div className="connection-left">
        <span className="app-title">Blinky Console</span>
        {deviceInfo && (
          <span className="device-info">
            {deviceInfo.device} v{deviceInfo.version} &bull; {deviceInfo.leds} LEDs
          </span>
        )}
      </div>
      <div className="connection-right">
        <button
          className="btn btn-small console-btn"
          onClick={onOpenConsole}
          title="Open Serial Console"
        >
          🖥️
        </button>
        <span className="status-indicator" style={{ backgroundColor: getStatusColor() }} />
        <span className="status-text">{getStatusText()}</span>
        {connectionState === 'connected' ? (
          <button className="btn btn-disconnect" onClick={onDisconnect}>
            Disconnect
          </button>
        ) : (
          <button
            className="btn btn-connect"
            onClick={onConnect}
            disabled={connectionState === 'connecting'}
          >
            {connectionState === 'connecting' ? 'Connecting...' : 'Connect'}
          </button>
        )}
      </div>
    </div>
  );
}
