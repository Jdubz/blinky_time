/**
 * Protocol-layer types shared by {@link DeviceProtocol} and its consumers.
 *
 * These types describe the protocol the firmware speaks (newline-delimited
 * JSON commands and stream messages) — not the wire transport beneath it.
 * The legacy "Serial" prefix is kept for compatibility with existing
 * imports; despite the name they apply to any transport (WebSerial,
 * Web Bluetooth, server-proxied WebSocket, …).
 */

import type {
  AudioMessage,
  BatteryMessage,
  TransientMessage,
  RhythmMessage,
  StatusMessage,
} from '../../types';

export class SerialError extends Error {
  constructor(
    message: string,
    public readonly code: SerialErrorCode
  ) {
    super(message);
    this.name = 'SerialError';
  }
}

export enum SerialErrorCode {
  NOT_SUPPORTED = 'NOT_SUPPORTED',
  NOT_CONNECTED = 'NOT_CONNECTED',
  CONNECTION_FAILED = 'CONNECTION_FAILED',
  DISCONNECTED = 'DISCONNECTED',
  COMMAND_INVALID = 'COMMAND_INVALID',
  COMMAND_FAILED = 'COMMAND_FAILED',
  TIMEOUT = 'TIMEOUT',
  PARSE_ERROR = 'PARSE_ERROR',
  PORT_IN_USE = 'PORT_IN_USE',
  PERMISSION_DENIED = 'PERMISSION_DENIED',
  DEVICE_LOST = 'DEVICE_LOST',
}

export type SerialEventType =
  | 'connected'
  | 'disconnected'
  | 'data'
  | 'error'
  | 'audio'
  | 'battery'
  | 'batteryStatus'
  | 'transient'
  | 'rhythm'
  | 'status';

export interface BatteryStatusData {
  voltage: number; // Battery voltage in volts
  percent: number; // Battery percentage (0-100)
  charging: boolean; // True if currently charging
  connected: boolean; // True if battery is connected
}

export interface SerialEvent {
  type: SerialEventType;
  data?: string;
  audio?: AudioMessage;
  battery?: BatteryMessage;
  batteryStatus?: BatteryStatusData;
  transient?: TransientMessage;
  rhythm?: RhythmMessage;
  status?: StatusMessage;
  error?: Error;
}

export type SerialEventCallback = (event: SerialEvent) => void;
