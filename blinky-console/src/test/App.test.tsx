import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import App from '../App';

// Mock the useSerial hook
const mockUseSerial = {
  connectionState: 'disconnected' as const,
  isSupported: true,
  deviceInfo: null,
  settings: [],
  settingsByCategory: {},
  isStreaming: false,
  audioData: null,
  consoleLog: [],
  connect: vi.fn(),
  disconnect: vi.fn(),
  sendCommand: vi.fn(),
  setSetting: vi.fn(),
  toggleStreaming: vi.fn(),
  saveSettings: vi.fn(),
  loadSettings: vi.fn(),
  resetDefaults: vi.fn(),
  refreshSettings: vi.fn(),
  clearConsole: vi.fn(),
};

vi.mock('../hooks/useSerial', () => ({
  useSerial: () => mockUseSerial,
}));

describe('App', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    // Reset mock state
    mockUseSerial.connectionState = 'disconnected';
    mockUseSerial.isSupported = true;
    mockUseSerial.deviceInfo = null;
    mockUseSerial.settings = [];
    mockUseSerial.settingsByCategory = {};
    mockUseSerial.isStreaming = false;
    mockUseSerial.audioData = null;
    mockUseSerial.consoleLog = [];
  });

  it('renders the main application', () => {
    render(<App />);
    expect(screen.getByText('Blinky Console')).toBeInTheDocument();
  });

  it('renders all main sections', () => {
    render(<App />);

    // Connection bar
    expect(screen.getByText('Blinky Console')).toBeInTheDocument();

    // Audio visualizer
    expect(screen.getByText('Audio Monitor')).toBeInTheDocument();

    // Console
    expect(screen.getByText('Console')).toBeInTheDocument();

    // Settings panel
    expect(screen.getByText('Settings')).toBeInTheDocument();
  });

  describe('connection flow', () => {
    it('shows connect button when disconnected', () => {
      render(<App />);
      expect(screen.getByRole('button', { name: 'Connect' })).toBeInTheDocument();
    });

    it('calls connect when connect button is clicked', () => {
      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Connect' }));

      expect(mockUseSerial.connect).toHaveBeenCalled();
    });

    it('shows disconnect button when connected', () => {
      mockUseSerial.connectionState = 'connected';
      render(<App />);

      expect(screen.getByRole('button', { name: 'Disconnect' })).toBeInTheDocument();
    });

    it('calls disconnect when disconnect button is clicked', () => {
      mockUseSerial.connectionState = 'connected';
      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Disconnect' }));

      expect(mockUseSerial.disconnect).toHaveBeenCalled();
    });
  });

  describe('disabled state', () => {
    it('disables controls when disconnected', () => {
      render(<App />);

      // Console input should be disabled
      const consoleInput = screen.getByPlaceholderText('Connect to send commands...');
      expect(consoleInput).toBeDisabled();

      // Stream button should be disabled
      const streamButton = screen.getByRole('button', { name: 'Start Stream' });
      expect(streamButton).toBeDisabled();

      // Settings buttons should be disabled
      const saveButton = screen.getByRole('button', { name: 'Save' });
      expect(saveButton).toBeDisabled();
    });

    it('enables controls when connected', () => {
      mockUseSerial.connectionState = 'connected';
      render(<App />);

      // Console input should be enabled
      const consoleInput = screen.getByPlaceholderText('Type command and press Enter...');
      expect(consoleInput).not.toBeDisabled();

      // Stream button should be enabled
      const streamButton = screen.getByRole('button', { name: 'Start Stream' });
      expect(streamButton).not.toBeDisabled();
    });
  });

  describe('WebSerial not supported', () => {
    it('shows warning when WebSerial is not supported', () => {
      mockUseSerial.isSupported = false;
      render(<App />);

      expect(screen.getByText(/WebSerial not supported/)).toBeInTheDocument();
    });
  });

  describe('device info display', () => {
    it('shows device info when connected', () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.deviceInfo = {
        device: 'Blinky Time',
        version: '2.0.0',
        width: 16,
        height: 16,
        leds: 256,
      };

      render(<App />);

      expect(screen.getByText(/Blinky Time v2.0.0/)).toBeInTheDocument();
      expect(screen.getByText(/256 LEDs/)).toBeInTheDocument();
    });
  });

  describe('settings interaction', () => {
    it('calls setSetting when setting is changed', async () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.settingsByCategory = {
        fire: [
          { name: 'intensity', value: true, type: 'bool' as const, cat: 'fire', min: 0, max: 1 },
        ],
      };

      render(<App />);

      const checkbox = screen.getByRole('checkbox');
      fireEvent.click(checkbox);

      // Wait for debounce
      await waitFor(
        () => {
          expect(mockUseSerial.setSetting).toHaveBeenCalled();
        },
        { timeout: 200 }
      );
    });

    it('calls saveSettings when Save is clicked', () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.settingsByCategory = {
        test: [{ name: 'test', value: 1, type: 'uint8' as const, cat: 'test', min: 0, max: 255 }],
      };

      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Save' }));

      expect(mockUseSerial.saveSettings).toHaveBeenCalled();
    });

    it('calls loadSettings when Load is clicked', () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.settingsByCategory = {
        test: [{ name: 'test', value: 1, type: 'uint8' as const, cat: 'test', min: 0, max: 255 }],
      };

      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Load' }));

      expect(mockUseSerial.loadSettings).toHaveBeenCalled();
    });

    it('calls resetDefaults when Reset is clicked', () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.settingsByCategory = {
        test: [{ name: 'test', value: 1, type: 'uint8' as const, cat: 'test', min: 0, max: 255 }],
      };

      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Reset' }));

      expect(mockUseSerial.resetDefaults).toHaveBeenCalled();
    });
  });

  describe('console interaction', () => {
    it('calls sendCommand when command is sent', async () => {
      mockUseSerial.connectionState = 'connected';
      render(<App />);

      const input = screen.getByPlaceholderText('Type command and press Enter...');
      fireEvent.change(input, { target: { value: 'test command' } });
      fireEvent.keyDown(input, { key: 'Enter' });

      expect(mockUseSerial.sendCommand).toHaveBeenCalledWith('test command');
    });

    it('calls clearConsole when Clear is clicked', () => {
      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Clear' }));

      expect(mockUseSerial.clearConsole).toHaveBeenCalled();
    });

    it('displays console entries', () => {
      mockUseSerial.consoleLog = [
        { id: 1, timestamp: new Date(), type: 'sent' as const, message: 'test message' },
      ];

      render(<App />);

      expect(screen.getByText('test message')).toBeInTheDocument();
    });
  });

  describe('audio streaming', () => {
    it('calls toggleStreaming when stream button is clicked', () => {
      mockUseSerial.connectionState = 'connected';
      render(<App />);

      fireEvent.click(screen.getByRole('button', { name: 'Start Stream' }));

      expect(mockUseSerial.toggleStreaming).toHaveBeenCalled();
    });

    it('displays audio values when streaming', () => {
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.isStreaming = true;
      mockUseSerial.audioData = { l: 0.75, t: 0.5, e: 0.6, g: 2.0 };

      render(<App />);

      expect(screen.getByText('L: 0.75')).toBeInTheDocument();
      expect(screen.getByText('T: 0.50')).toBeInTheDocument();
      expect(screen.getByText('G: 2.0x')).toBeInTheDocument();
    });
  });
});
