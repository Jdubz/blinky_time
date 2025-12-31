import '@testing-library/jest-dom';
import { vi } from 'vitest';

// Mock react-hot-toast
vi.mock('react-hot-toast', () => ({
  default: {
    success: vi.fn(),
    error: vi.fn(),
    loading: vi.fn(),
    dismiss: vi.fn(),
    promise: vi.fn((promise: Promise<unknown>) => promise),
  },
  Toaster: () => null,
  toast: {
    success: vi.fn(),
    error: vi.fn(),
    loading: vi.fn(),
    dismiss: vi.fn(),
    promise: vi.fn((promise: Promise<unknown>) => promise),
  },
}));

// Mock scrollIntoView (not supported in jsdom)
Element.prototype.scrollIntoView = vi.fn();

// Mock AudioContext (not supported in jsdom)
class MockAudioContext {
  currentTime = 0;
  destination = {};
  state = 'running';
  resume = vi.fn().mockResolvedValue(undefined);
  close = vi.fn().mockResolvedValue(undefined);
  createOscillator = vi.fn(() => ({
    type: 'sine',
    frequency: { value: 440, setValueAtTime: vi.fn() },
    connect: vi.fn(),
    start: vi.fn(),
    stop: vi.fn(),
  }));
  createGain = vi.fn(() => ({
    gain: { value: 1, setValueAtTime: vi.fn(), exponentialRampToValueAtTime: vi.fn() },
    connect: vi.fn(),
  }));
  createBiquadFilter = vi.fn(() => ({
    type: 'lowpass',
    frequency: { value: 1000, setValueAtTime: vi.fn() },
    Q: { value: 1 },
    connect: vi.fn(),
  }));
}
(globalThis as unknown as { AudioContext: typeof MockAudioContext }).AudioContext =
  MockAudioContext;

// Mock WebSerial API for testing
Object.defineProperty(navigator, 'serial', {
  value: {
    getPorts: vi.fn().mockResolvedValue([]),
    requestPort: vi.fn(),
  },
  writable: true,
  configurable: true,
});

// Mock matchMedia for components that use it
Object.defineProperty(window, 'matchMedia', {
  writable: true,
  value: vi.fn().mockImplementation(query => ({
    matches: false,
    media: query,
    onchange: null,
    addListener: vi.fn(),
    removeListener: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  })),
});

// Mock ResizeObserver
window.ResizeObserver = vi.fn().mockImplementation(() => ({
  observe: vi.fn(),
  unobserve: vi.fn(),
  disconnect: vi.fn(),
}));

// Mock Chart.js to avoid canvas rendering issues in tests
vi.mock('chart.js', () => {
  // Create a mock Chart constructor with static methods
  const MockChart = Object.assign(
    vi.fn().mockImplementation(() => ({
      destroy: vi.fn(),
      update: vi.fn(),
      render: vi.fn(),
      resize: vi.fn(),
      clear: vi.fn(),
      stop: vi.fn(),
      canvas: null,
      ctx: null,
      data: {},
      options: {},
    })),
    {
      register: vi.fn(),
      unregister: vi.fn(),
      defaults: { font: {}, color: '' },
      instances: {},
    }
  );

  // Mock scale and element classes
  const createMockClass = () => vi.fn().mockImplementation(() => ({}));

  return {
    Chart: MockChart,
    CategoryScale: createMockClass(),
    LinearScale: createMockClass(),
    PointElement: createMockClass(),
    LineElement: createMockClass(),
    Title: createMockClass(),
    Tooltip: createMockClass(),
    Legend: createMockClass(),
    Filler: createMockClass(),
    registerables: [],
  };
});

vi.mock('react-chartjs-2', () => ({
  Line: vi.fn(() => null),
}));
