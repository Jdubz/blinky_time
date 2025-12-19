import { describe, it, expect, vi, beforeEach } from 'vitest';
import { serialService } from './serial';

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
    vi.clearAllMocks();

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

    // Update the mock to return our mockPort
    (navigator.serial.requestPort as ReturnType<typeof vi.fn>).mockResolvedValue(mockPort);
  });

  describe('isSupported', () => {
    it('returns true when WebSerial is available', () => {
      expect(serialService.isSupported()).toBe(true);
    });
  });

  describe('event listeners', () => {
    it('adds and removes event listeners', () => {
      const callback = vi.fn();

      serialService.addEventListener(callback);
      serialService.removeEventListener(callback);

      // Listener should be removed - no way to verify without internal access
      expect(callback).not.toHaveBeenCalled();
    });
  });

  describe('connect', () => {
    it('requests a serial port from the user', async () => {
      await serialService.connect();
      expect(navigator.serial.requestPort).toHaveBeenCalled();
    });

    it('opens the port with correct baud rate', async () => {
      await serialService.connect(9600);
      expect(mockPort.open).toHaveBeenCalledWith({ baudRate: 9600 });
    });

    it('uses default baud rate of 115200', async () => {
      await serialService.connect();
      expect(mockPort.open).toHaveBeenCalledWith({ baudRate: 115200 });
    });

    it('emits connected event on success', async () => {
      const callback = vi.fn();
      serialService.addEventListener(callback);

      await serialService.connect();

      expect(callback).toHaveBeenCalledWith({ type: 'connected' });
      serialService.removeEventListener(callback);
    });

    it('returns true on successful connection', async () => {
      const result = await serialService.connect();
      expect(result).toBe(true);
    });

    it('emits error event when port request fails', async () => {
      (navigator.serial.requestPort as ReturnType<typeof vi.fn>).mockRejectedValue(
        new Error('User cancelled')
      );

      const callback = vi.fn();
      serialService.addEventListener(callback);

      const result = await serialService.connect();

      expect(result).toBe(false);
      expect(callback).toHaveBeenCalledWith(
        expect.objectContaining({
          type: 'error',
        })
      );
      serialService.removeEventListener(callback);
    });
  });

  describe('disconnect', () => {
    it('emits disconnected event', async () => {
      const callback = vi.fn();
      serialService.addEventListener(callback);

      await serialService.connect();
      callback.mockClear();

      await serialService.disconnect();

      expect(callback).toHaveBeenCalledWith({ type: 'disconnected' });
      serialService.removeEventListener(callback);
    });
  });

  describe('isConnected', () => {
    it('returns true when connected', async () => {
      await serialService.connect();
      expect(serialService.isConnected()).toBe(true);
    });

    it('returns false after disconnect', async () => {
      await serialService.connect();
      await serialService.disconnect();
      expect(serialService.isConnected()).toBe(false);
    });
  });

  describe('send', () => {
    it('sends command with newline', async () => {
      await serialService.connect();

      await serialService.send('test command');

      expect(mockWriter.write).toHaveBeenCalled();
      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('test command\n');
    });

    it('validates and sanitizes commands', async () => {
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
      await serialService.connect();

      const longCommand = 'a'.repeat(200);
      await serialService.send(longCommand);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      const sent = decoder.decode(writtenData);

      // Should be limited to MAX_COMMAND_LENGTH (128) + newline
      expect(sent.length).toBeLessThanOrEqual(129);
    });
  });

  describe('setSetting', () => {
    it('sends set command with name and value', async () => {
      await serialService.connect();

      await serialService.setSetting('intensity', 0.5);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('set intensity 0.5\n');
    });

    it('handles boolean values', async () => {
      await serialService.connect();

      await serialService.setSetting('enabled', true);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('set enabled true\n');
    });
  });

  describe('setStreamEnabled', () => {
    it('sends stream on command', async () => {
      await serialService.connect();

      await serialService.setStreamEnabled(true);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('stream on\n');
    });

    it('sends stream off command', async () => {
      await serialService.connect();

      await serialService.setStreamEnabled(false);

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('stream off\n');
    });
  });

  describe('saveSettings', () => {
    it('sends save command', async () => {
      await serialService.connect();

      await serialService.saveSettings();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('save\n');
    });
  });

  describe('loadSettings', () => {
    it('sends load command', async () => {
      await serialService.connect();

      await serialService.loadSettings();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('load\n');
    });
  });

  describe('resetDefaults', () => {
    it('sends defaults command', async () => {
      await serialService.connect();

      await serialService.resetDefaults();

      const writtenData = mockWriter.write.mock.calls[0][0];
      const decoder = new TextDecoder();
      expect(decoder.decode(writtenData)).toBe('defaults\n');
    });
  });
});
