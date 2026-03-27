/**
 * Blinky Time - LED Fire Effect Controller
 *
 * A sophisticated fire effect system for wearable LED installations.
 * Supports multiple device configurations with realistic fire simulation,
 * audio reactivity, battery management, and motion sensing.
 *
 * Hardware: nRF52840 XIAO Sense with WS2812B LED strips
 * Author: Blinky Time Project Contributors
 * License: Creative Commons Attribution-ShareAlike 4.0 International
 * Repository: https://github.com/Jdubz/blinky_time
 *
 * Device Types:
 * - Hat: 89 LEDs in string configuration
 * - Tube Light: 60 LEDs in 4x15 zigzag matrix
 * - Bucket Totem: 128 LEDs in 16x8 matrix
 */

// v74: AudioTracker replaces AudioController (ACF+Comb+PLL, ~10 params, 60fps).

// NOTE: Adafruit_NeoPixel must be first. PDM.h is included separately in
// Nrf52PdmMic.cpp to avoid pinDefinitions.h redefinition (Seeeduino mbed platform bug)
#include <Adafruit_NeoPixel.h>
#include "BlinkyArchitecture.h"     // Includes all architecture components and config
#include "BlinkyImplementations.h"  // Includes all .cpp implementations for Arduino IDE
#include "render/RenderPipeline.h"  // Generator/Effect/Renderer management
#include "types/Version.h"           // Version information from repository
#include "tests/SafeMode.h"          // Crash recovery system
#include "hal/DefaultHal.h"          // HAL singleton instances
#include "hal/hardware/NeoPixelLedStrip.h"  // LED strip wrapper
#include "config/DeviceConfigLoader.h"       // Runtime device config loading
#include "hal/Uf2BootloaderOverride.h"       // Fix 1200-baud touch → UF2 mode (not serial DFU)
#include "hal/SafeBootWatchdog.h"            // Hardware WDT + auto-recovery to UF2 bootloader
#include "audio/FakeAudio.h"                 // Synthetic audio for visual design/debug
#include "audio/AudioTracker.h"              // Simplified ACF+Comb+PLL tracker (v74)

#ifdef BLINKY_PLATFORM_ESP32S3
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#endif

// Runtime Device Configuration (v28+)
// Device config is now loaded from flash at boot time instead of compile-time selection.
// This allows a single firmware to support multiple device types without recompilation.
//
// To configure a device:
// 1. Flash this universal firmware
// 2. Upload device config via serial: `upload config <JSON>`
// 3. Reboot - device will auto-configure from flash
//
// If no config is present in flash, the device enters SAFE MODE:
// - Audio analysis runs normally
// - Serial console available for configuration
// - LED output DISABLED (prevents driving wrong hardware)
DeviceConfig config;  // Runtime device configuration (loaded from flash)
bool validDeviceConfig = false;  // Is device configured and ready?
LEDMapper ledMapper;

// Hardware abstraction layer for testability
// Use pointers to avoid static initialization order fiasco
// Adafruit_NeoPixel allocates memory in constructor - unsafe at global scope
Adafruit_NeoPixel* neoPixelStrip = nullptr;
NeoPixelLedStrip* leds = nullptr;

// New Generator-Effect-Render Architecture
// === ARCHITECTURE STATUS ===
// ✅ Core System: Inputs→Generator→Effect(optional)→Render pipeline operational
// ✅ Fire: Realistic fire simulation (red/orange/yellow) for all layout types
// ✅ Water: Flowing water effects (blue/cyan) for all layout types
// ✅ Lightning: Electric bolt effects (yellow/white) for all layout types
// ✅ Effects: HueRotation for color cycling, NoOp for pass-through
// ✅ Hardware: AdaptiveMic ready for audio input
// ✅ Compilation: Ready for all device types (Hat, Tube Light, Bucket Totem)
RenderPipeline* pipeline = nullptr;  // Manages generators, effects, and rendering

// HAL-enabled components - use pointers to avoid static initialization order fiasco
// These are initialized in setup() AFTER Arduino runtime is ready
AudioTracker* audioController = nullptr;     // ACF+Comb+PLL beat tracker (v74)
BatteryMonitor* battery = nullptr;
IMUHelper imu;                     // IMU sensor interface; auto-initializes, uses stub mode if LSM6DS3 not installed
ConfigStorage configStorage;       // Persistent settings storage
SerialConsole* console = nullptr;  // Serial command interface
#ifdef BLINKY_PLATFORM_NRF52840
BleScanner bleScanner;             // BLE passive scanner (receives fleet broadcasts)
BleNus bleNus;                     // BLE NUS peripheral (serial-over-BLE for fleet server)
#elif defined(BLINKY_PLATFORM_ESP32S3)
BleAdvertiser bleAdvertiser;       // BLE advertising broadcaster (sends fleet commands)
WifiManager wifiManager;           // WiFi credential storage and connection
WifiCommandServer tcpServer;       // TCP command server for wireless fleet management
#endif

uint32_t lastMs = 0;
bool prevChargingState = false;
FakeAudio fakeAudio;  // Synthetic 120 BPM audio for visual design/debug ("fakeaudio on/off")

// Helper function for Generator-Effect-Renderer pipeline
void renderFrame() {
  // Generate -> Effect -> Render -> Display pipeline
  if (pipeline && audioController) {
    if (fakeAudio.isEnabled()) {
      AudioControl fakeCtrl = fakeAudio.getControl();
      pipeline->render(fakeCtrl);
    } else {
      const AudioControl& audio = audioController->getControl();
      pipeline->render(audio);
    }
    leds->show();
  }
}

/**
 * Cleanup all dynamically allocated resources.
 * Called on allocation failure to prevent memory leaks.
 * Also useful for future sleep/wake or restart scenarios.
 */
void cleanup() {
  delete console;    console = nullptr;
  delete pipeline;   pipeline = nullptr;  // RenderPipeline cleans up generators/effects/renderer
  delete battery;    battery = nullptr;
  delete audioController; audioController = nullptr;
  delete leds;       leds = nullptr;
  delete neoPixelStrip; neoPixelStrip = nullptr;
}

/**
 * Halt execution with error message after cleanup.
 * Use instead of bare while(1) loops to prevent memory leaks.
 */
void haltWithError(const __FlashStringHelper* msg) {
  Serial.println(msg);
  cleanup();
  while(1) { delay(10000); }
}

void setup() {
  // CRITICAL: Hardware watchdog + boot counter — catches HardFaults, heap
  // exhaustion, infinite loops. After 3 consecutive failed boots, automatically
  // enters UF2 bootloader for safe firmware upload. Uses GPREGRET2 which
  // persists across all reset types (WDT, soft reset, HardFault).
  SafeBootWatchdog::begin();

  // Software crash detection (complementary — uses .noinit RAM)
  SafeMode::check();

  // Initialize serial with default baud rate (config not loaded yet)
  Serial.begin(115200);
  delay(1000);  // Give serial time to initialize

  // Display version information (always show on boot)
  Serial.println(F("\n=== BLINKY TIME STARTUP ==="));
  Serial.println(F(BLINKY_FULL_VERSION));
  Serial.print(F("[INFO] Build: ")); Serial.print(F(BLINKY_BUILD_DATE));
  Serial.print(F(" ")); Serial.println(F(BLINKY_BUILD_TIME));

  // Debug: detailed boot info
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
    Serial.print(F("[DEBUG] Boot count: "));
    Serial.print(SafeMode::getCrashCount());
    Serial.print(F("/"));
    Serial.println(SafeMode::CRASH_THRESHOLD);
  }

  // Initialize configuration storage and load device config from flash
  configStorage.begin();
  validDeviceConfig = DeviceConfigLoader::loadFromFlash(configStorage, config);

  // Reinitialize serial if configured baud rate differs from default
  if (validDeviceConfig && config.serial.baudRate != 115200) {
    Serial.print(F("[INFO] Switching serial baud rate to: "));
    Serial.println(config.serial.baudRate);
    Serial.println(F("Reinitializing serial port in 2 seconds..."));
    delay(2000);  // Give user time to see message before baud rate changes
    Serial.end();
    Serial.begin(config.serial.baudRate);
    delay(500);   // Let serial stabilize
    Serial.println(F("\n[INFO] Serial port reinitialized"));
  }

  if (!validDeviceConfig) {
    // SAFE MODE: No valid device configuration
    Serial.println(F("\n!!! SAFE MODE - NO DEVICE CONFIG !!!"));
    Serial.println(F("LED output DISABLED"));
    Serial.println(F("Audio analysis ENABLED"));
    Serial.println(F("Serial console ENABLED"));
    Serial.println(F("\nUpload device config via serial:"));
    Serial.println(F("  device upload <JSON>"));
    Serial.println(F("  Example: device upload {\"deviceId\":\"hat_v1\",\"ledWidth\":89,\"ledHeight\":1,...}"));
    Serial.println(F("\nOr use the web console to select a device type."));
    Serial.println(F(""));
  } else {
    // Valid config loaded
    Serial.print(F("[INFO] Device: "));
    Serial.println(config.deviceName);

    // Validate critical configuration parameters
    if (config.matrix.width <= 0 || config.matrix.height <= 0) {
      haltWithError(F("ERROR: Invalid matrix dimensions"));
    }
    if (config.matrix.brightness > 255) {
      SerialConsole::logWarn(F("Brightness clamped to 255"));
      config.matrix.brightness = 255;
    }
  }

  // Initialize HAL-enabled components - ALWAYS initialize (even in safe mode)
  // Audio and battery monitoring work independently of LED configuration
  audioController = new(std::nothrow) AudioTracker(DefaultHal::pdm(), DefaultHal::time());
  battery = new(std::nothrow) BatteryMonitor(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
  if (!audioController || !battery) {
    haltWithError(F("ERROR: HAL component allocation failed"));
  }

  // Initialize audio controller (uses default or configured sample rate)
  uint16_t audioSampleRate = validDeviceConfig ? config.microphone.sampleRate : 16000;
  bool audioOk = audioController->begin(audioSampleRate);
  if (!audioOk) {
    SerialConsole::logError(F("Audio controller failed to start"));
  } else {
    SerialConsole::logDebug(F("Audio controller initialized"));
  }

  // === LED SYSTEM INITIALIZATION (only if valid device config) ===
  if (validDeviceConfig) {
    Serial.println(F("\n=== Initializing LED System ==="));

    // Initialize LED strip (must be done in setup, not global scope)
    neoPixelStrip = new(std::nothrow) Adafruit_NeoPixel(
        config.matrix.width * config.matrix.height,
        config.matrix.ledPin,
        config.matrix.ledType);
    if (!neoPixelStrip) {
      haltWithError(F("ERROR: NeoPixel allocation failed"));
    }
    leds = new(std::nothrow) NeoPixelLedStrip(*neoPixelStrip);
    if (!leds || !leds->isValid()) {
      haltWithError(F("ERROR: LED strip wrapper allocation failed"));
    }

    leds->begin();
    leds->setBrightness(min((int)config.matrix.brightness, 255));
    leds->show();

    // Basic LED test - light up first few LEDs to verify hardware
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
      Serial.print(F("[DEBUG] LED Test at brightness "));
      Serial.println(config.matrix.brightness);
    }
    leds->setPixelColor(0, leds->Color(255, 0, 0));  // Should show RED
    leds->setPixelColor(1, leds->Color(0, 255, 0));  // Should show GREEN
    leds->setPixelColor(2, leds->Color(0, 0, 255));  // Should show BLUE
    leds->show();
    delay(3000);  // Hold for 3 seconds to verify colors are correct

    // Clear test LEDs
    leds->setPixelColor(0, 0);
    leds->setPixelColor(1, 0);
    leds->setPixelColor(2, 0);
    leds->show();

    if (!ledMapper.begin(config)) {
      haltWithError(F("ERROR: LED mapper initialization failed"));
    }

    // Debug: detailed config info
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
      Serial.print(F("[DEBUG] Layout: "));
      switch (config.matrix.layoutType) {
        case MATRIX_LAYOUT:  Serial.print(F("MATRIX")); break;
        case LINEAR_LAYOUT:  Serial.print(F("LINEAR")); break;
        case RANDOM_LAYOUT:  Serial.print(F("RANDOM")); break;
        default:             Serial.print(F("UNKNOWN")); break;
      }
      Serial.print(F(", Matrix: "));
      Serial.print(config.matrix.width);
      Serial.print(F("x"));
      Serial.print(config.matrix.height);
      Serial.print(F(" = "));
      Serial.print(config.matrix.width * config.matrix.height);
      Serial.println(F(" LEDs"));
    }

    // Initialize RenderPipeline (manages generators, effects, and rendering)
    pipeline = new(std::nothrow) RenderPipeline();
    if (!pipeline || !pipeline->begin(config, *leds, ledMapper)) {
      haltWithError(F("ERROR: RenderPipeline initialization failed"));
    }

    SerialConsole::logDebug(F("RenderPipeline initialized"));

    // Load effect parameters from flash
    if (configStorage.isValid()) {
      // Load parameters directly into generators' internal storage
      Fire* fireGen = pipeline->getFireGenerator();
      Water* waterGen = pipeline->getWaterGenerator();
      Lightning* lightningGen = pipeline->getLightningGenerator();

      if (fireGen && waterGen && lightningGen) {
        configStorage.loadConfiguration(
          fireGen->getParamsMutable(),
          waterGen->getParamsMutable(),
          lightningGen->getParamsMutable(),
          audioController->getMicForTuning(),
          audioController
        );
        SerialConsole::logDebug(F("Loaded effect params from flash"));

        // CRITICAL: Sync physics params to force adapters after loading from flash
        // The force adapters were initialized with default params before we loaded
        // the saved values, so we must explicitly update them now
        fireGen->syncPhysicsParams();
        waterGen->syncPhysicsParams();
      } else {
        SerialConsole::logWarn(F("Generator pointers invalid, using defaults"));
      }
    } else {
      SerialConsole::logDebug(F("Using default effect params"));
    }

    Serial.println(F("=== LED System Ready ===\n"));
  }
  // End of LED system initialization

  // Initialize battery monitor - ALWAYS initialize (even in safe mode)
  if (!battery->begin()) {
    SerialConsole::logWarn(F("Battery monitor failed to start"));
  } else {
    bool fastCharge = validDeviceConfig ? config.charging.fastChargeEnabled : false;
    battery->setFastCharge(fastCharge);
    SerialConsole::logDebug(F("Battery monitor initialized"));
  }

  // Rhythm tracking handled internally by AudioTracker (ACF+Comb+PLL)

  // Initialize serial console for interactive settings management
  // Uses RenderPipeline for generator/effect switching
  console = new(std::nothrow) SerialConsole(pipeline, &audioController->getMicForTuning());
  if (!console) {
    haltWithError(F("ERROR: SerialConsole allocation failed"));
  }
  console->setConfigStorage(&configStorage);
  console->setBatteryMonitor(battery);
  console->setAudioController(audioController);
  console->setFakeAudio(&fakeAudio);
  console->begin();
  SerialConsole::logDebug(F("Serial console initialized"));

  // Initialize BLE (nRF52840 only)
#ifdef BLINKY_PLATFORM_NRF52840
  // Initialize SoftDevice with 1 peripheral connection (NUS) + observer (scanner)
  Bluefruit.begin(1, 0);
  Bluefruit.setName("Blinky");
  Bluefruit.setTxPower(4);  // 4 dBm for peripheral advertising reach

  // NUS peripheral — bidirectional serial-over-BLE for fleet server
  bleNus.begin();
  bleNus.setLineCallback([](const char* line) {
      if (console) {
          console->handleCommand(line);
      }
  });
  console->setBleNus(&bleNus);
  SerialConsole::logDebug(F("BLE NUS peripheral initialized"));

  // Passive scanner — receives fleet broadcasts from ESP32-S3 gateway
  bleScanner.begin();
  bleScanner.setCommandCallback([](const char* payload, size_t len) {
      if (console) {
          console->handleCommand(payload);
      }
  });
  console->setBleScanner(&bleScanner);
  SerialConsole::logDebug(F("BLE scanner initialized"));
#elif defined(BLINKY_PLATFORM_ESP32S3)
  // BLE disabled on ESP32-S3 — NimBLE porting layer crashes on core 3.3.7
  // (npl_freertos_sem_init / npl_freertos_mutex_init assertion failure).
  // Known bug: arduino-esp32 #12357, #12362. Fix requires external
  // NimBLE-Arduino library v2.3.8+ (PRs #1090, #1117).
  // ESP32-S3 uses WiFi TCP for fleet communication instead.
  // bleAdvertiser.begin();
  // console->setBleAdvertiser(&bleAdvertiser);
  wifiManager.begin();  // Loads stored credentials from NVS
  console->setWifiManager(&wifiManager);
  // Connect WiFi on Core 1 (where the ESP32 WiFi event loop runs),
  // then hand off the TCP server to Core 0 for non-blocking accept/read/write.
  tcpServer.setConsole(console);
  console->setTcpServer(&tcpServer);
  // Configure OTA callbacks once (begin() called when WiFi connects)
  ArduinoOTA.setHostname("blinky");
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setPassword("blinkyota");
  ArduinoOTA.onStart([]() { Serial.println(F("[OTA] Start")); });
  ArduinoOTA.onEnd([]() { Serial.println(F("[OTA] Done, rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) {
      Serial.print(F("[OTA] Error: ")); Serial.println(e);
  });
  // Try WiFi connect in setup (up to 10s). If it fails, auto-reconnect in loop().
  if (wifiManager.hasCredentials()) {
      wifiManager.connect();
  }
  SerialConsole::logDebug(F("WiFi (BLE disabled — NimBLE core 3.3.7 bug)"));
#endif

  // FIX: Reset frame timing to prevent stale state from previous boot
  lastMs = 0;

  Serial.println(F("Ready."));

  // Mark boot as stable - we made it through setup without crashing
  // This resets the crash counter so future boots start fresh
  SafeMode::markStable();
  SafeBootWatchdog::markStable();

  Serial.print(F("[BOOT] Watchdog active, boot attempt #"));
  Serial.println(SafeBootWatchdog::getBootCount());
}

void loop() {
  SafeBootWatchdog::feed();  // Keep hardware WDT alive
  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;

  // Frame time diagnostics: only warn for unexpectedly long frames.
  // When NN inference is active (~98ms per frame), long frames are expected
  // and warnings would flood serial output, corrupting JSON streams.
  // Only warn when frame time exceeds 200ms (indicates actual performance issue).
  if (dt > 0.2f) {
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
      Serial.print(F("[WARN] Frame time: "));
      Serial.print((now - lastMs));
      Serial.println(F("ms"));
    }
  }

  dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME); // Clamp dt to reasonable range
  lastMs = now;

  // Update unified audio controller (handles mic + rhythm tracking internally)
  if (audioController) {
    audioController->update(dt);
  }

  // Advance fake audio clock when enabled
  fakeAudio.update(dt);

  // Get pulse from audio control signal for transient logging
  const AudioControl& audio = audioController ? audioController->getControl() : AudioControl{};
  float pulse = audio.pulse;

  // Send transient detection events when debug channel is enabled
  // Use: "debug transient on" to enable, "debug transient off" to disable
  if (audioController && pulse > 0.0f &&
      SerialConsole::isDebugChannelEnabled(DebugChannel::TRANSIENT)) {
    // TRANSIENT message: simplified single-band detection
    Serial.print(F("{\"type\":\"TRANSIENT\",\"timestampMs\":"));
    Serial.print(now);
    Serial.print(F(",\"strength\":"));
    Serial.print(pulse, 2);
    Serial.println(F("}"));
  }

  // Track charging state changes
  bool currentChargingState = battery ? battery->isCharging() : false;
  if (currentChargingState != prevChargingState) {
    if (currentChargingState) {
      SerialConsole::logInfo(F("Charging started"));
    } else {
      SerialConsole::logInfo(F("Charging stopped"));
    }
    prevChargingState = currentChargingState;
  }

  // Render current generator through the effect pipeline (only if valid config)
  if (validDeviceConfig) {
    renderFrame();

    // Auto-save dirty settings to flash (debounced)
    if (audioController && pipeline) {
      Fire* fireGen = pipeline->getFireGenerator();
      Water* waterGen = pipeline->getWaterGenerator();
      Lightning* lightningGen = pipeline->getLightningGenerator();

      if (fireGen && waterGen && lightningGen) {
        configStorage.saveIfDirty(
          fireGen->getParams(),
          waterGen->getParams(),
          lightningGen->getParams(),
          audioController->getMicForTuning(),
          audioController
        );
      }
    }
  } else {
    // Safe mode: blink built-in LED as heartbeat
    static uint32_t lastHeartbeat = 0;
    if (now - lastHeartbeat > 1000) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      lastHeartbeat = now;
    }
  }

  // Handle serial commands via SerialConsole (always active)
  if (console) {
    console->update();
  }

  // Process wireless data
#ifdef BLINKY_PLATFORM_NRF52840
  bleNus.update();       // NUS peripheral (serial-over-BLE)
  bleScanner.update();   // Fleet broadcast receiver
#elif defined(BLINKY_PLATFORM_ESP32S3)
  tcpServer.poll();  // Non-blocking TCP accept/read (all on Core 1)
  ArduinoOTA.handle();  // Non-blocking OTA check (~0.5ms)
  // Monitor WiFi and auto-reconnect
  {
      static bool wasConnected = false;
      static bool servicesStarted = false;
      bool isConnected = (WiFi.status() == WL_CONNECTED);
      if (isConnected && !wasConnected) {
          if (!servicesStarted) {
              // First connection: start TCP server, mDNS, and OTA
              MDNS.begin("blinky");
              MDNS.addService("blinky", "tcp", 3333);  // Fleet discovery
              tcpServer.begin();
              ArduinoOTA.begin();
              servicesStarted = true;
              Serial.print(F("[WiFi] Services started. OTA at "));
              Serial.print(WiFi.localIP());
              Serial.println(F(":3232"));
          }
          Serial.print(F("[WiFi] Connected: "));
          Serial.print(WiFi.localIP());
          Serial.print(F(" RSSI="));
          Serial.println(WiFi.RSSI());
      } else if (!isConnected && wasConnected) {
          Serial.println(F("[WiFi] Disconnected, reconnecting..."));
          WiFi.reconnect();
      }
      wasConnected = isConnected;
  }
#endif

  // Battery monitoring - periodic voltage check
  static uint32_t lastBatteryCheck = 0;
  if (battery && validDeviceConfig && millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) {
    lastBatteryCheck = millis();
    float voltage = battery->getVoltage();
    if (voltage > 0 && voltage < config.charging.criticalBatteryThreshold) {
      if (SerialConsole::getGlobalLogLevel() >= LogLevel::ERROR) {
        Serial.print(F("[ERROR] CRITICAL BATTERY: "));
        Serial.print(voltage);
        Serial.println(F("V"));
      }
    } else if (voltage > 0 && voltage < config.charging.lowBatteryThreshold) {
      if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
        Serial.print(F("[WARN] Low battery: "));
        Serial.print(voltage);
        Serial.println(F("V"));
      }
    }
  }
}
