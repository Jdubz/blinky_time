/**
 * Schema validation tests for firmware ↔ console JSON contract
 *
 * These tests verify that Zod schemas correctly validate actual firmware responses.
 * If firmware changes its JSON format, these tests will fail, alerting us to update
 * both the schemas and any dependent code.
 */

import { describe, it, expect } from 'vitest';
import {
  DeviceInfoSchema,
  SettingsResponseSchema,
  AudioMessageSchema,
  BatteryMessageSchema,
  BatteryStatusResponseSchema,
  TransientMessageSchema,
  RhythmMessageSchema,
  StatusMessageSchema,
} from './index';

/**
 * Sample firmware responses captured from real device communication.
 * Update these when firmware JSON format changes.
 */
const FIRMWARE_SAMPLES = {
  // Response from `json info` command (v28+ format with nested device object)
  deviceInfo: {
    version: 'v1.2.0-dev',
    device: {
      id: 'bucket_v1',
      name: 'Bucket Totem',
      width: 16,
      height: 8,
      leds: 128,
      configured: true as const,
    },
  },

  // Response from `json info` when device is not configured (safe mode)
  deviceInfoUnconfigured: {
    version: 'v1.2.0-dev',
    device: {
      configured: false as const,
      safeMode: true as const,
    },
  },

  // Response from `json settings` command (subset for testing)
  settings: {
    settings: [
      {
        name: 'cooling',
        value: 55,
        type: 'uint8',
        cat: 'fire',
        min: 0,
        max: 255,
        desc: 'Base cooling rate',
      },
      {
        name: 'sparkchance',
        value: 0.15,
        type: 'float',
        cat: 'fire',
        min: 0,
        max: 1,
        desc: 'Probability of sparks',
      },
      {
        name: 'debugtransients',
        value: false,
        type: 'bool',
        cat: 'audio',
        min: 0,
        max: 1,
      },
    ],
  },

  // Streaming audio message `{"a":{...}}`
  audioMessage: {
    a: {
      l: 0.42,
      t: 0.0,
      pk: 0.65,
      vl: 0.12,
      raw: 0.35,
      h: 40,
      alive: 1,
      z: 0.23,
    },
  },

  // Streaming audio message with rhythm data
  audioMessageWithRhythm: {
    a: {
      l: 0.55,
      t: 0.8,
      pk: 0.72,
      vl: 0.15,
      raw: 0.48,
      h: 45,
      alive: 1,
      z: 0.31,
    },
    r: {
      bpm: 120,
      str: 0.85,
      per: 500,
      lik: 0.72,
      ph: 0.35,
      buf: 128,
    },
  },

  // Streaming audio message with music mode data
  audioMessageWithMusic: {
    a: {
      l: 0.6,
      t: 0.0,
      pk: 0.8,
      vl: 0.1,
      raw: 0.5,
      h: 50,
      alive: 1,
      z: 0.25,
    },
    m: {
      a: 1,
      bpm: 128,
      ph: 0.5,
      conf: 0.9,
      q: 1,
      h: 0,
      w: 0,
    },
  },

  // Streaming battery message `{"b":{...}}`
  batteryMessage: {
    b: {
      n: true,
      c: false,
      v: 3.85,
      p: 72,
    },
  },

  // Battery status response from `battery` command
  batteryStatus: {
    battery: {
      voltage: 3.92,
      percent: 78,
      charging: true,
      connected: true,
    },
  },

  // Transient detection event (minimal format from firmware)
  transientMessage: {
    type: 'TRANSIENT',
    timestampMs: 12345678,
    strength: 0.85,
  },

  // Transient with extended fields (optional, for future use)
  transientMessageExtended: {
    type: 'TRANSIENT',
    timestampMs: 12345678,
    strength: 0.75,
    ts: 12345678,
    mode: 3,
    level: 0.68,
    energy: 0.55,
  },

  // Rhythm analyzer telemetry
  rhythmMessage: {
    type: 'RHYTHM',
    bpm: 124,
    strength: 0.78,
    periodMs: 484,
    likelihood: 0.65,
    phase: 0.42,
    bufferFill: 200,
  },

  // Status telemetry
  statusMessage: {
    type: 'STATUS',
    ts: 12345900,
    mode: 4,
    hwGain: 45,
    level: 0.55,
    avgLevel: 0.48,
    peakLevel: 0.82,
  },
};

describe('DeviceInfoSchema', () => {
  it('validates configured device info', () => {
    const result = DeviceInfoSchema.safeParse(FIRMWARE_SAMPLES.deviceInfo);
    expect(result.success).toBe(true);
    if (result.success && result.data.device.configured) {
      expect(result.data.device.name).toBe('Bucket Totem');
      expect(result.data.device.leds).toBe(128);
    }
  });

  it('validates unconfigured device info (safe mode)', () => {
    const result = DeviceInfoSchema.safeParse(FIRMWARE_SAMPLES.deviceInfoUnconfigured);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.device.configured).toBe(false);
    }
  });

  it('rejects missing required fields', () => {
    const result = DeviceInfoSchema.safeParse({
      version: '1.0',
      device: {
        // missing required fields for configured device
        configured: true,
      },
    });
    expect(result.success).toBe(false);
  });

  it('rejects invalid LED count', () => {
    const result = DeviceInfoSchema.safeParse({
      version: 'v1.0',
      device: {
        ...FIRMWARE_SAMPLES.deviceInfo.device,
        leds: -1,
      },
    });
    expect(result.success).toBe(false);
  });
});

describe('SettingsResponseSchema', () => {
  it('validates correct settings response', () => {
    const result = SettingsResponseSchema.safeParse(FIRMWARE_SAMPLES.settings);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.settings).toHaveLength(3);
      expect(result.data.settings[0].name).toBe('cooling');
      expect(result.data.settings[1].value).toBe(0.15);
      expect(result.data.settings[2].value).toBe(false);
    }
  });

  it('validates all setting types', () => {
    const allTypes = {
      settings: [
        { name: 'u8', value: 255, type: 'uint8', cat: 'test', min: 0, max: 255 },
        { name: 'i8', value: -50, type: 'int8', cat: 'test', min: -128, max: 127 },
        { name: 'u16', value: 1000, type: 'uint16', cat: 'test', min: 0, max: 65535 },
        { name: 'u32', value: 100000, type: 'uint32', cat: 'test', min: 0, max: 4294967295 },
        { name: 'f', value: 0.5, type: 'float', cat: 'test', min: 0, max: 1 },
        { name: 'b', value: true, type: 'bool', cat: 'test', min: 0, max: 1 },
      ],
    };
    const result = SettingsResponseSchema.safeParse(allTypes);
    expect(result.success).toBe(true);
  });

  it('rejects invalid setting type', () => {
    const result = SettingsResponseSchema.safeParse({
      settings: [{ name: 'bad', value: 0, type: 'invalid_type', cat: 'test', min: 0, max: 1 }],
    });
    expect(result.success).toBe(false);
  });
});

describe('AudioMessageSchema', () => {
  it('validates basic audio message', () => {
    const result = AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessage);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.a.l).toBe(0.42);
      expect(result.data.a.alive).toBe(1);
    }
  });

  it('validates audio message with rhythm data', () => {
    const result = AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessageWithRhythm);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.r?.bpm).toBe(120);
      expect(result.data.r?.str).toBe(0.85);
    }
  });

  it('validates audio message with music mode data', () => {
    const result = AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessageWithMusic);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.m?.a).toBe(1);
      expect(result.data.m?.bpm).toBe(128);
    }
  });

  it('rejects audio level out of range', () => {
    const result = AudioMessageSchema.safeParse({
      a: {
        ...FIRMWARE_SAMPLES.audioMessage.a,
        l: 1.5, // Over 1.0
      },
    });
    expect(result.success).toBe(false);
  });

  it('rejects invalid alive value', () => {
    const result = AudioMessageSchema.safeParse({
      a: {
        ...FIRMWARE_SAMPLES.audioMessage.a,
        alive: 2, // Not 0 or 1
      },
    });
    expect(result.success).toBe(false);
  });
});

describe('BatteryMessageSchema', () => {
  it('validates streaming battery message', () => {
    const result = BatteryMessageSchema.safeParse(FIRMWARE_SAMPLES.batteryMessage);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.b.v).toBe(3.85);
      expect(result.data.b.p).toBe(72);
    }
  });

  it('rejects percent over 100', () => {
    const result = BatteryMessageSchema.safeParse({
      b: {
        ...FIRMWARE_SAMPLES.batteryMessage.b,
        p: 150,
      },
    });
    expect(result.success).toBe(false);
  });
});

describe('BatteryStatusResponseSchema', () => {
  it('validates battery status response', () => {
    const result = BatteryStatusResponseSchema.safeParse(FIRMWARE_SAMPLES.batteryStatus);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.battery.voltage).toBe(3.92);
      expect(result.data.battery.charging).toBe(true);
    }
  });
});

describe('TransientMessageSchema', () => {
  it('validates transient detection message', () => {
    const result = TransientMessageSchema.safeParse(FIRMWARE_SAMPLES.transientMessage);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.type).toBe('TRANSIENT');
      expect(result.data.timestampMs).toBe(12345678);
      expect(result.data.strength).toBe(0.85);
      expect(result.data.mode).toBeUndefined(); // mode is optional
    }
  });

  it('validates transient with extended fields', () => {
    const result = TransientMessageSchema.safeParse(FIRMWARE_SAMPLES.transientMessageExtended);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.timestampMs).toBe(12345678);
      expect(result.data.mode).toBe(3);
      expect(result.data.level).toBe(0.68);
    }
  });

  it('validates all detection modes', () => {
    for (const mode of [0, 1, 2, 3, 4]) {
      const result = TransientMessageSchema.safeParse({
        ...FIRMWARE_SAMPLES.transientMessage,
        mode,
      });
      expect(result.success).toBe(true);
    }
  });

  it('rejects invalid detection mode', () => {
    const result = TransientMessageSchema.safeParse({
      ...FIRMWARE_SAMPLES.transientMessage,
      mode: 5,
    });
    expect(result.success).toBe(false);
  });
});

describe('RhythmMessageSchema', () => {
  it('validates rhythm message', () => {
    const result = RhythmMessageSchema.safeParse(FIRMWARE_SAMPLES.rhythmMessage);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.type).toBe('RHYTHM');
      expect(result.data.bpm).toBe(124);
      expect(result.data.periodMs).toBe(484);
    }
  });

  it('rejects wrong type discriminator', () => {
    const result = RhythmMessageSchema.safeParse({
      ...FIRMWARE_SAMPLES.rhythmMessage,
      type: 'STATUS',
    });
    expect(result.success).toBe(false);
  });
});

describe('StatusMessageSchema', () => {
  it('validates status message', () => {
    const result = StatusMessageSchema.safeParse(FIRMWARE_SAMPLES.statusMessage);
    expect(result.success).toBe(true);
    if (result.success) {
      expect(result.data.type).toBe('STATUS');
      expect(result.data.hwGain).toBe(45);
      expect(result.data.avgLevel).toBe(0.48);
    }
  });

  it('rejects hwGain over 80', () => {
    const result = StatusMessageSchema.safeParse({
      ...FIRMWARE_SAMPLES.statusMessage,
      hwGain: 100,
    });
    expect(result.success).toBe(false);
  });
});

describe('Contract validation', () => {
  it('all firmware samples are valid', () => {
    // This test ensures we have comprehensive coverage
    expect(DeviceInfoSchema.safeParse(FIRMWARE_SAMPLES.deviceInfo).success).toBe(true);
    expect(DeviceInfoSchema.safeParse(FIRMWARE_SAMPLES.deviceInfoUnconfigured).success).toBe(true);
    expect(SettingsResponseSchema.safeParse(FIRMWARE_SAMPLES.settings).success).toBe(true);
    expect(AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessage).success).toBe(true);
    expect(AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessageWithRhythm).success).toBe(
      true
    );
    expect(AudioMessageSchema.safeParse(FIRMWARE_SAMPLES.audioMessageWithMusic).success).toBe(true);
    expect(BatteryMessageSchema.safeParse(FIRMWARE_SAMPLES.batteryMessage).success).toBe(true);
    expect(BatteryStatusResponseSchema.safeParse(FIRMWARE_SAMPLES.batteryStatus).success).toBe(
      true
    );
    expect(TransientMessageSchema.safeParse(FIRMWARE_SAMPLES.transientMessage).success).toBe(true);
    expect(
      TransientMessageSchema.safeParse(FIRMWARE_SAMPLES.transientMessageExtended).success
    ).toBe(true);
    expect(RhythmMessageSchema.safeParse(FIRMWARE_SAMPLES.rhythmMessage).success).toBe(true);
    expect(StatusMessageSchema.safeParse(FIRMWARE_SAMPLES.statusMessage).success).toBe(true);
  });
});
