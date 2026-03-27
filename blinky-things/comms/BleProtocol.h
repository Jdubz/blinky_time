#pragma once

/**
 * BLE Protocol — shared constants and types for BLE communication.
 * Used by BleNus (nRF52840), BleAdvertiser (ESP32-S3), and BleScanner.
 */

// BLE NUS (Nordic UART Service) UUIDs
// Standard NUS service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static constexpr const char* BLE_NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static constexpr const char* BLE_NUS_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
static constexpr const char* BLE_NUS_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

// Protocol version for command/response framing
static constexpr uint8_t BLE_PROTOCOL_VERSION = 1;
