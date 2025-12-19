import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

// Create a fresh instance for each test by resetting module
const createSerialService = async () => {
  vi.resetModules();
  const module = await import('./serial');
  return module.serialService;
};

describe('SerialService', () => {
  let mockPort: {
    open: ReturnType<typeof vi.fn>;
    close: ReturnType<typeof vi.fn>;
    readable: ReadableStream<Uint8Array> | null;
    writable: WritableStream<Uint8Array> | null;
    getInfo: ReturnType<typeof vi.fn>;
  };

  let mockReader: {
    read: ReturnType<typeof vi.fn>;
    cancel: ReturnType<typeof vi.fn>;
    releaseLock: ReturnType<typeof vi.fn>;
  };

  let mockWriter: {
    write: ReturnType<typeof vi.fn>;
    releaseLock: ReturnType<typeof vi.fn>;
  };

  beforeEach(() => {
    mockReader = {
      read: vi.fn().mockResolvedValue({ value: new Uint8Array(), done: true }),
      cancel: vi.fn().mockResolvedValue(undefined),
      releaseLock: vi.fn(),
    };

    mockWriter = {
      write: vi.fn().mockResolvedValue(undefined),
      releaseLock: vi.fn(),
    };

    const mockReadable = {
      getReader: vi.fn().mockReturnValue(mockReader),
    } as unknown as ReadableStream<Uint8Array>;

    const mockWritable = {
      getWriter: vi.fn().mockReturnValue(mockWriter),
    } as unknown as WritableStream<Uint8Array>;

    mockPort = {
      open: vi.fn().mockResolvedValue(undefined),
      close: vi.fn().mockResolvedValue(undefined),
      readable: mockReadable,
      writable: mockWritable,
      getInfo: vi.fn().mockReturnValue({ usbVendorId: 0x1234, usbProductId: 0x5678 }),
    };

    // Mock navigator.serial
    Object.defineProperty(navigator, 'serial', {
      value: {
        requestPort: vi.fn().mockResolvedValue(mockPort),
        getPorts: vi.fn().mockResolvedValue([]),
      },
      writable: true,
      configurable: true,
    });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  describe('isSupported', () => {
    it('returns true when WebSerial is available', async () => {
      const serialService = await createSerialService();
      expect(serialService.isSupported()).toBe(true);
    });

    it('returns false when WebSerial is not available', async () => {
      Object.defineProperty(navigator, 'serial', {
        value: undefined,
        writable: true,
        configurable: true,
      });

      const serialService = await createSerialService();
      expect(serialService.isSupported()).toBe(false);
    });
  });

  describe('event listeners', () => {
    it('adds and removes event listeners', async () => {
      const serialService = await createSerialService();
      const callback = vi.fn();

      serialService.addEventListener(callback);
      serialService.removeEventListener(callback);

      // Listener should be removed
      expect(callback).not.toHaveBeenCalled();
    });
  });

  describe('connect', () => {
    it('requests a serial port from the user', async () => {
      const serialService = await createSerialService();

      await serialService.connect();

      expect(navigator.serial.requestPort).toHaveBeenCalled();
    });

    it('opens the port with correct baud rate', async () => {
      const serialService = await createSerialService();

      await serialService.connect(9600);

      expect(mockPort.open).toHaveBeenCalledWith({ baudRate: 9600 });
    });

    it('uses default baud rate of 115200', async () => {
      const serialService = await createSerialService();

      await serialService.connect();

      expect(mockPort.open).toHaveBeenCalledWith({ baudRate: 115200 });
    });

    it('emits connected event on success', async () => {
      const serialService = await createSerialService();
      const callback = vi.fn();
      serialService.addEventListener(callback);

      await serialService.connect();

      expect(callback).toHaveBeenCalledWith({ type: 'connected' });
    });

    it('returns true on successful connection', async () => {
      const serialService = await createSerialService();

      const result = await serialService.connect();

      expect(result).toBe(true);
    });

    it('emits error event when WebSerial not supported', async () => {
      Object.defineProperty(navigator, 'serial', {
        value: undefined,
        writable: true,
        configurable: true,
      });

      const serialService = await createSerialService();
      const callback = vi.fn();
      serialService.addEventListener(callback);

      const result = await serialService.connect();

      expect(result).toBe(false);
      expect(callback).toHaveBeenCalledWith(
        expect.objectContaining({
          type: 'error',
          error: expect.any(Error),
        })
      );
    });

    it('emits error event when port request fails', async () => {
      (navigator.serial.requestPort as ReturnType<typeof vi.fn>).mockRejectedValue(
        new Error('User cancelled')
      );

      const serialService = await createSerialService();
      const callback = vi.fn();
      serialService.addEventListener(callback);

      const result = await serialService.connect();

      expect(result).toBe(false);
      expect(callback).toHaveBeenCalledWith(
        expect.objectContaining({
          type: 'error',
        })
      );
    });
  });

  describe('disconnect', () => {
    it('closes reader, writer, and port', async () => {
      const serialService = await createSerialService();

      await serialService.connect();
      await serialService.disconnect();

      expect(mockReader.cancel).toHaveBeenCalled();
      expect(mockReader.releaseLock).toHaveBeenCalled();
      expect(mockWriter.releaseLock).toHaveBeenCalled();
      expect(mockPort.close).toHaveBeenCalled();
    });

    it('emits disconnected event', async () => {
      const serialService = await createSerialService();
      const callback = vi.fn();
      serialService.addEventListener(callback);

      await serialService.connect();
      callback.mockClear();

      await serialService.disconnect();

      expect(callback).toHaveBeenCalledWith({ type: 'disconnected' });
    });
  });

  describe('isConnected', () => {
    it('returns false when not connected', async () => {
      const serialService = await createSerialService();
      expect(serialService.isConnected()).toBe(false);
    });

    it('returns true when connected', async () => {
      const serialService = await createSerialService();
      await serialService.connect();
      expect(serialService.isConnected()).toBe(true);
    });

    it('returns false after disconnect', async () => {
      const serialService = await createSerialService();
      await serialService.connect();
      await serialService.disconnect();
      expect(serialService.isConnected()).toBe(false);
    });
  });

  describe('send', () => {
    it('throws error when not connected', async () => {
      const serialService = await createSerialService();

      await expect(serialService.send('test')).rejects.toThrow('Not connected');
    });

    it('sends command with newline', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.send('test command');

      expect(mockWriter.write).toHaveBeenCalled();
      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('test command\n');
    });

    it('validates and sanitizes commands', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      // Command with special characters should be sanitized
      await serialService.send('test<script>alert("xss")</script>');

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      const sent = decoder.decode(writtenData);

      // Should not contain < or >
      expect(sent).not.toContain('<');
      expect(sent).not.toContain('>');
    });

    it('trims and limits command length', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      const longCommand = 'a'.repeat(200);
      await serialService.send(longCommand);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      const sent = decoder.decode(writtenData);

      // Should be limited to MAX_COMMAND_LENGTH (128) + newline
      expect(sent.length).toBeLessThanOrEqual(129);
    });

    it('throws error for invalid commands after sanitization', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      // Command that becomes empty after sanitization
      await expect(serialService.send('   ')).rejects.toThrow('Invalid command');
    });
  });

  describe('setSetting', () => {
    it('sends set command with name and value', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.setSetting('intensity', 0.5);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('set intensity 0.5\n');
    });

    it('handles boolean values', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.setSetting('enabled', true);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('set enabled true\n');
    });
  });

  describe('setStreamEnabled', () => {
    it('sends stream on command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.setStreamEnabled(true);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('stream on\n');
    });

    it('sends stream off command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.setStreamEnabled(false);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('stream off\n');
    });
  });

  describe('saveSettings', () => {
    it('sends save command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.saveSettings();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('save\n');
    });
  });

  describe('loadSettings', () => {
    it('sends load command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.loadSettings();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('load\n');
    });
  });

  describe('resetDefaults', () => {
    it('sends defaults command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      await serialService.resetDefaults();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('defaults\n');
    });
  });

  describe('getDeviceInfo', () => {
    it('sends json info command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      // Start the request (don't await, it will timeout)
      serialService.getDeviceInfo();

      // Check the command was sent
      await vi.waitFor(() => {
        expect(mockWriter.write).toHaveBeenCalled();
      });

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('json info\n');
    });
  });

  describe('getSettings', () => {
    it('sends json settings command', async () => {
      const serialService = await createSerialService();
      await serialService.connect();

      // Start the request (don't await, it will timeout)
      serialService.getSettings();

      // Check the command was sent
      await vi.waitFor(() => {
        expect(mockWriter.write).toHaveBeenCalled();
      });

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('json settings\n');
    });
  });
});
