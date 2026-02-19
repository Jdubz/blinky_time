import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import App from '../App';
import type { UseSerialReturn } from '../hooks/useSerial';

// Mock the useSerial hook with proper typing
const mockUseSerial: UseSerialReturn = {
  connectionState: 'disconnected',
  isSupported: true,
  errorMessage: null,
  errorCode: null,
  loading: {
    connecting: false,
    settings: false,
    streaming: false,
    generator: false,
    effect: false,
    saving: false,
  },
  deviceInfo: null,
  settings: [],
  settingsByCategory: {},
  currentGenerator: 'fire',
  currentEffect: 'none',
  availableGenerators: ['fire', 'water', 'lightning', 'audio'],
  availableEffects: ['none', 'hue'],
  isStreaming: false,
  audioData: null,
  batteryData: null,
  batteryStatusData: null,
  musicModeData: null,
  statusData: null,
  onTransientEvent: vi.fn(() => () => {}),
  onRhythmEvent: vi.fn(() => () => {}),
  onStatusEvent: vi.fn(() => () => {}),
  consoleLines: [],
  clearConsole: vi.fn(),
  sendCommand: vi.fn(),
  connect: vi.fn(),
  disconnect: vi.fn(),
  setSetting: vi.fn(),
  toggleStreaming: vi.fn(),
  saveSettings: vi.fn(),
  loadSettings: vi.fn(),
  resetDefaults: vi.fn(),
  refreshSettings: vi.fn(),
  loadSettingsByCategory: vi.fn(),
  requestBatteryStatus: vi.fn(),
  setGenerator: vi.fn(),
  setEffect: vi.fn(),
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
  });

  it('renders the main application', () => {
    render(<App />);
    expect(screen.getByText('Blinky Console')).toBeInTheDocument();
  });

  it('renders all main sections', () => {
    render(<App />);

    // Connection bar
    expect(screen.getByText('Blinky Console')).toBeInTheDocument();

    // Tab buttons
    expect(screen.getByRole('tab', { name: 'Inputs' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: 'Generators' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: 'Effects' })).toBeInTheDocument();

    // Audio visualizer (in Inputs tab by default)
    expect(screen.getByText('AdaptiveMic Output')).toBeInTheDocument();

    // Settings panel (in Inputs tab by default)
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
      // Add some settings so the settings panel renders buttons
      mockUseSerial.settingsByCategory = {
        test: [{ name: 'test', value: 1, type: 'uint8' as const, cat: 'test', min: 0, max: 255 }],
      };

      render(<App />);

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
        version: '2.0.0',
        device: {
          id: 'blinky_v1',
          name: 'Blinky Time',
          width: 16,
          height: 16,
          leds: 256,
          configured: true as const,
        },
      };

      render(<App />);

      expect(screen.getByText(/Blinky Time v2.0.0/)).toBeInTheDocument();
      expect(screen.getByText(/256 LEDs/)).toBeInTheDocument();
    });
  });

  describe('settings interaction', () => {
    it('calls setSetting when setting is changed', async () => {
      vi.useFakeTimers();
      mockUseSerial.connectionState = 'connected';
      mockUseSerial.settingsByCategory = {
        audio: [
          { name: 'enabled', value: true, type: 'bool' as const, cat: 'audio', min: 0, max: 1 },
        ],
      };

      render(<App />);

      // Audio settings are in the Inputs tab (default)
      const checkbox = screen.getByRole('checkbox');
      fireEvent.click(checkbox);

      // Advance timers for debounce
      await vi.advanceTimersByTimeAsync(150);

      expect(mockUseSerial.setSetting).toHaveBeenCalled();
      vi.useRealTimers();
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
      mockUseSerial.audioData = {
        l: 0.75,
        t: 0.5,
        pk: 0.6,
        vl: 0.05,
        raw: 0.25,
        h: 32,
        alive: 1,
        z: 0.15,
      };

      render(<App />);

      // Audio values displayed in header (percussion removed - shown via indicators instead)
      expect(screen.getByText('Level: 0.75')).toBeInTheDocument();
      expect(screen.getByText('Peak: 0.60')).toBeInTheDocument();
      expect(screen.getByText('Valley: 0.05')).toBeInTheDocument();
      expect(screen.getByText('HW Gain: 32')).toBeInTheDocument();
    });
  });
});
