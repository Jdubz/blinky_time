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
#include "inputs/GeneratorButton.h"  // Debounced GPIO button → cycle generator
#include "types/Version.h"           // Version information from repository
#include "hal/DefaultHal.h"          // HAL singleton instances
#include "audio/LoopMetrics.h"       // Main-loop fps + frame-time observability (#137)
#include "hal/hardware/NeoPixelLedStrip.h"  // LED strip wrapper (Adafruit)
#include "hal/hardware/Nrf52PwmLedStrip.h"  // Async PWM driver (nRF52840)
#include "hal/hardware/CompositeLedStrip.h"  // Two-strand composite LED strip
#include "config/DeviceConfigLoader.h"       // Runtime device config loading
#include "devices/TestChipConfig.h"           // Fallback config for unconfigured chips
#include "hal/Uf2BootloaderOverride.h"       // Fix 1200-baud touch → UF2 mode (not serial DFU)
#include "hal/SafeBootWatchdog.h"            // Hardware WDT + auto-recovery to UF2 bootloader
#include "hal/RebootFrequencyCounter.h"      // Flash-backed crash-loop detector (survives power cycles)
#include "audio/FakeAudio.h"                 // Synthetic audio for visual design/debug
#include "audio/AudioTracker.h"              // Simplified ACF+Comb+PLL tracker (v74)

#ifdef BLINKY_PLATFORM_ESP32S3
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPUpdate.h>
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
ILedStrip* leds = nullptr;
// Set true when a multi-strand config (ledPin2 != 0) was requested but
// the multi-strand alloc failed and we fell back to single-strand on
// ledPin only. Surfaces in `json info` as `ledMode: "degraded"` so the
// fleet console can spot it without log triage (PR #139 review).
bool ledModeDegraded = false;

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
GeneratorButton generatorButton;     // Per-device "next generator" button (no-op if buttonPin==0)

// HAL-enabled components - use pointers to avoid static initialization order fiasco
// These are initialized in setup() AFTER Arduino runtime is ready
AudioTracker* audioController = nullptr;     // ACF+Comb+PLL beat tracker (v74)
BatteryMonitor* battery = nullptr;
IMUHelper imu;                     // IMU sensor interface; auto-initializes, uses stub mode if LSM6DS3 not installed
ConfigStorage configStorage;       // Persistent settings storage
SerialConsole* console = nullptr;  // Serial command interface
#ifdef BLINKY_PLATFORM_NRF52840
#include <services/BLEDfu.h>
BLEDfu bleDfu;                     // BLE DFU service (wireless firmware updates)
BleScanner bleScanner;             // BLE passive scanner (receives fleet broadcasts)
BleNus bleNus;                     // BLE NUS peripheral (serial-over-BLE for fleet server)
#elif defined(BLINKY_PLATFORM_ESP32S3)
#include "comms/Esp32BleNus.h"
BleAdvertiser bleAdvertiser;       // BLE advertising broadcaster (sends fleet commands)
Esp32BleNus esp32BleNus;           // BLE NUS peripheral (serial-over-BLE for fleet server)
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

  // Initialize serial with default baud rate (config not loaded yet)
  // Increase RX buffer to handle large device config JSON commands (default 256 is too small)
#ifdef BLINKY_PLATFORM_ESP32S3
  Serial.setRxBufferSize(1024);
#endif
  Serial.begin(115200);
  delay(1000);  // Give serial time to initialize

  // Display version information (always show on boot)
  Serial.println(F("\n=== BLINKY TIME STARTUP ==="));
  Serial.print(F("Blinky Time ")); Serial.println(F(FIRMWARE_VERSION));
  Serial.print(F("[INFO] Built: ")); Serial.println(F(FIRMWARE_BUILD_DATE));

  // Debug: detailed boot info
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
    Serial.print(F("[DEBUG] Boot count: "));
    Serial.print(SafeBootWatchdog::getBootCount());
    Serial.print(F("/"));
    Serial.println(SafeBootWatchdog::BOOT_FAIL_THRESHOLD);
  }

  // Initialize configuration storage and load device config from flash.
  // Falls back to TEST_CHIP_CONFIG if no config stored — avoids safe mode
  // on bare chips so audio analysis and serial commands still work.
  configStorage.begin();

  // Wire the crash-loop quarantine hook BEFORE checkAndIncrement so that if
  // the threshold is tripped on this boot, the stored device config is
  // invalidated before we hand off to BLE DFU recovery. Without this, a
  // config that crashes the app every boot (e.g., misconfigured multi-strand
  // hardware) would just re-crash forever after each recovery firmware
  // flash — there'd be no way to break the loop wirelessly.
  RebootFrequencyCounter::setOnThresholdHook([]() {
    configStorage.quarantineDeviceConfig();
  });

  // Flash-backed crash-loop detector — catches runtime crashes that survive
  // setup() (and therefore SafeBootWatchdog's GPREGRET2 markStable clear) and
  // those across power cycles. MUST be called after configStorage.begin()
  // (which initializes LittleFS) and BEFORE the rest of setup (so we trip
  // recovery before any potentially-crashing component runs).
  // See docs/SCULPTURE_BLE_RECOVERY_PLAN.md (F4).
  RebootFrequencyCounter::checkAndIncrement();

  validDeviceConfig = DeviceConfigLoader::loadFromFlash(configStorage, config);
  if (!validDeviceConfig) {
    config = TEST_CHIP_CONFIG;
    validDeviceConfig = true;
    Serial.println(F("[INFO] No stored config — using Test Chip defaults"));
  }

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
    uint16_t numLeds = config.matrix.width * config.matrix.height;
#ifdef BLINKY_PLATFORM_NRF52840
    // nRF52840: always use async PWM driver. RGB-only (no RGBW), targets 120+
    // LED hardware. Non-blocking show() returns once DMA is queued instead of
    // blocking the main loop for ~30 µs/LED (~3.6 ms at 120 LEDs, ~30 ms at
    // 1024 LEDs) — frees that time for audio drain and FFT processing.
    // Pre-allocated PWM pattern buffer also avoids the per-frame malloc/free
    // that fragments the heap with Adafruit_NeoPixel.
    //
    // Verified at 1 LED on b157 fleet test chips (boot, audio loop healthy,
    // post-boot overrun rate ~0/sec). PWM timing math is count-independent
    // (each LED is 24 PWM values @ 800 KHz), so 8-16 LED bench setups use
    // the same code path with no count-dependent thresholds. The hard upper
    // bound is 1365 LEDs (PWM SEQ[n].CNT 15-bit limit, enforced in begin()).
    //
    // Multi-strand: when ledPin2 is non-zero, split the pixel buffer evenly
    // across two physical strands, each driven by its own PWM peripheral.
    // CompositeLedStrip dispatches pixel writes (0..N/2-1 → strand 1 on
    // ledPin; N/2..N-1 → strand 2 on ledPin2) and the renderer sees one
    // logical strip of N pixels.
    //
    // Each Nrf52PwmLedStrip grabs a free PWM via findFreePwm() — nRF52840
    // has 4 PWM peripherals so a 2-strand config has plenty of headroom.
    // The two DMAs run concurrently on hardware; CPU latency for show()
    // is single-strand's, not 2×.
    //
    // Recovery if this path crashes mid-init (the failure mode cart-inner-
    // old hit on 2026-05-14): RebootFrequencyCounter trips after 5 rapid
    // reboots → quarantineDeviceConfig() invalidates the offending config →
    // next boot enters safeMode (no app crash) → operator re-uploads a
    // corrected config. See feedback_validate_on_recoverable_first.md and
    // ConfigStorage::quarantineDeviceConfig().
    bool useComposite = false;
    if (config.matrix.ledPin2 != 0 && (numLeds % 2 == 0)) {
      const uint16_t halfLeds = numLeds / 2;
      Serial.print(F("[INFO] Multi-strand init: "));
      Serial.print(halfLeds);
      Serial.print(F(" LEDs on pin D"));
      Serial.print(config.matrix.ledPin);
      Serial.print(F(" + "));
      Serial.print(halfLeds);
      Serial.print(F(" LEDs on pin D"));
      Serial.println(config.matrix.ledPin2);

      Serial.println(F("[DEBUG]   alloc strand 1..."));
      auto* s1 = new(std::nothrow) Nrf52PwmLedStrip(halfLeds, config.matrix.ledPin);
      Serial.print(F("[DEBUG]   strand 1 alloc: ptr="));
      Serial.print((uintptr_t)s1, HEX);
      Serial.print(F(" valid="));
      Serial.println(s1 && s1->isValid() ? F("yes") : F("no"));

      Serial.println(F("[DEBUG]   alloc strand 2..."));
      auto* s2 = new(std::nothrow) Nrf52PwmLedStrip(halfLeds, config.matrix.ledPin2);
      Serial.print(F("[DEBUG]   strand 2 alloc: ptr="));
      Serial.print((uintptr_t)s2, HEX);
      Serial.print(F(" valid="));
      Serial.println(s2 && s2->isValid() ? F("yes") : F("no"));

      if (s1 && s1->isValid() && s2 && s2->isValid()) {
        Serial.println(F("[DEBUG]   wrap in CompositeLedStrip..."));
        auto* composite = new(std::nothrow) CompositeLedStrip(s1, s2);
        if (composite) {
          leds = composite;
          useComposite = true;
          Serial.println(F("[INFO]   composite wrap OK"));
        } else {
          Serial.println(F("[ERROR] CompositeLedStrip alloc failed — falling back to single-strand"));
          delete s1;
          delete s2;
          ledModeDegraded = true;
        }
      } else {
        Serial.println(F("[ERROR] strand alloc/validity failed — falling back to single-strand"));
        delete s1;
        delete s2;
        ledModeDegraded = true;
      }
    }
    if (!useComposite) {
      auto* asyncStrip = new(std::nothrow) Nrf52PwmLedStrip(numLeds, config.matrix.ledPin);
      leds = asyncStrip;  // Assign before validity check so cleanup() can free it
      if (!asyncStrip || !asyncStrip->isValid()) {
        haltWithError(F("ERROR: Async LED strip allocation failed"));
      }
      Serial.print(F("[INFO] LED driver: Nrf52PwmLedStrip (async, "));
      Serial.print(numLeds);
      Serial.println(F(" LEDs"));
      if (ledModeDegraded) {
        Serial.println(F(", DEGRADED — multi-strand requested but fell back to single)"));
      } else {
        Serial.println(F(")"));
      }
    }
#else
    // Non-nRF52840 platforms: Adafruit NeoPixel (blocking bit-bang)
    neoPixelStrip = new(std::nothrow) Adafruit_NeoPixel(
        numLeds, config.matrix.ledPin, config.matrix.ledType);
    if (!neoPixelStrip) {
      haltWithError(F("ERROR: NeoPixel allocation failed"));
    }
    leds = new(std::nothrow) NeoPixelLedStrip(*neoPixelStrip);
    if (!leds) {
      haltWithError(F("ERROR: LED strip wrapper allocation failed"));
    }
#endif

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

    // Per-device "cycle generator" button. No-op when buttonPin == 0.
    // Logs the pin assignment at INFO so it's visible during deployment
    // verification (matches the "BLE name:" / "[INFO] Device:" pattern).
    generatorButton.begin(config.input.buttonPin);
    if (config.input.buttonPin != 0) {
      Serial.print(F("[INFO] Generator-cycle button on pin D"));
      Serial.println(config.input.buttonPin);
    }

    // Load effect parameters from flash
    if (configStorage.isValid()) {
      // Load parameters directly into generators' internal storage
      Fire* fireGen = pipeline->getFireGenerator();
      Water* waterGen = pipeline->getWaterGenerator();
      PlasmaGlobe* plasmaGen = pipeline->getPlasmaGlobeGenerator();

      if (fireGen && waterGen && plasmaGen) {
        configStorage.loadConfiguration(
          fireGen->getParamsMutable(),
          waterGen->getParamsMutable(),
          plasmaGen->getParamsMutable(),
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
  // Initialize BLE stack with 1 peripheral connection (NUS) + observer (scanner)
  Bluefruit.begin(1, 0);

  // Per-device BLE name — "Blinky-<deviceId>-<snSuffix2>" when the chip has a
  // stored device config, or "Blinky-<snSuffix4>" for unconfigured chips.
  // snSuffix comes from FICR DEVICEID[0] lower bits (hardware-unique, same
  // source the fleet server uses for the canonical device ID). This gives
  // every device a distinct name in phone scan lists and fleet-console
  // discovery output, and the deviceId segment conveys which hardware
  // variant (hat_v1 / necklace_v2 / etc.) is running.
  {
    char bleName[32];
    const auto& storedDev = configStorage.getDeviceConfig();
    uint32_t sn = NRF_FICR->DEVICEID[0];
    if (configStorage.isDeviceConfigValid()
        && storedDev.deviceId[0] != '\0'
        && strcmp(storedDev.deviceId, "none") != 0) {
      snprintf(bleName, sizeof(bleName), "Blinky-%.16s-%02lX",
               storedDev.deviceId, (unsigned long)(sn & 0xFF));
    } else {
      snprintf(bleName, sizeof(bleName), "Blinky-%04lX",
               (unsigned long)(sn & 0xFFFF));
    }
    Bluefruit.setName(bleName);
    Serial.print(F("BLE name: "));
    Serial.println(bleName);
  }
  Bluefruit.setTxPower(4);  // 4 dBm for peripheral advertising reach

  // BLE DFU service — allows wireless firmware updates from fleet server.
  // Failure is not fatal (NUS-based `bootloader ble` still works), but it
  // removes a recovery path on sealed sculpture devices, so log loudly per
  // no-silent-fallbacks rule. See docs/SCULPTURE_BLE_RECOVERY_PLAN.md (F6).
  {
    err_t bleDfuErr = bleDfu.begin();
    if (bleDfuErr != ERROR_NONE) {
      Serial.print(F("[FALLBACK] bleDfu.begin() failed err=0x"));
      Serial.println((unsigned)bleDfuErr, HEX);
      Serial.println(F("[FALLBACK] Bootloader-mode BLE DFU advertisement may be unavailable."));
      Serial.println(F("[FALLBACK] App-mode `bootloader ble` command still works via NUS."));
    }
  }

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
  // BLE on ESP32-S3 requires external NimBLE-Arduino v2.3.8+ (installed 2.4.0)
  // to fix NimBLE porting layer crash (arduino-esp32 #12357, #12362).
  bleAdvertiser.begin();
  console->setBleAdvertiser(&bleAdvertiser);
  // NUS peripheral — bidirectional serial-over-BLE (same as nRF52840 BleNus)
  esp32BleNus.begin();
  esp32BleNus.setLineCallback([](const char* line) {
      if (console) {
          console->handleCommand(line);
      }
  });
  // Wire BLE NUS TX as TeeStream secondary — serial output goes to both
  // USB Serial and BLE NUS, enabling bidirectional serial-over-BLE.
  console->setEsp32BleNus(&esp32BleNus);
  wifiManager.begin();  // Loads stored credentials from NVS
  console->setWifiManager(&wifiManager);
  // Connect WiFi on Core 1 (where the ESP32 WiFi event loop runs),
  // then hand off the TCP server to Core 0 for non-blocking accept/read/write.
  tcpServer.setConsole(console);
  console->setTcpServer(&tcpServer);
  // Configure OTA callbacks once (begin() called when WiFi connects)
  ArduinoOTA.setHostname("blinky");
  ArduinoOTA.setPort(3232);
  // OTA password is intentional plaintext — ArduinoOTA runs on trusted LAN only
  // and the ESP32 MD5-hashes it before comparison (never sent in cleartext).
  ArduinoOTA.setPassword("blinkyota");
  ArduinoOTA.onStart([]() { Serial.println(F("[OTA] Start")); });
  ArduinoOTA.onEnd([]() { Serial.println(F("[OTA] Done, rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) {
      Serial.print(F("[OTA] Error: ")); Serial.println(e);
  });
  // Try WiFi connect in setup (up to 10s). If it fails, auto-reconnect in loop().
  if (wifiManager.hasCredentials()) {
      wifiManager.connect();
  } else {
      // No credentials — disable the radio entirely.
      // Without this, the WiFi event task keeps running at high FreeRTOS priority
      // and preempts the render loop with irregular ~5-20ms bursts, causing frame jitter.
      WiFi.mode(WIFI_OFF);
      SerialConsole::logDebug(F("WiFi disabled (no credentials)"));
  }
  SerialConsole::logDebug(F("ESP32-S3 BLE + WiFi initialized"));
#endif

  // FIX: Reset frame timing to prevent stale state from previous boot
  lastMs = 0;

  Serial.println(F("Ready."));

  // NOTE: SafeBootWatchdog::markStable() is intentionally NOT called here.
  // Deferred to loop() once millis() >= 60000 — a runtime crash that happens
  // after setup() completes but before that point should still count toward
  // the boot-fail threshold and eventually trigger BLE DFU recovery on sealed
  // devices. See docs/SCULPTURE_BLE_RECOVERY_PLAN.md (F3).

  Serial.print(F("[BOOT] Watchdog active, boot attempt #"));
  Serial.println(SafeBootWatchdog::getBootCount());
}

void loop() {
  SafeBootWatchdog::feed();  // Keep hardware WDT alive

  // Deferred markStable: keep the boot-fail counter armed for the first 60s
  // of runtime, so that a crash shortly after setup() still counts toward the
  // BLE DFU recovery threshold (F3 in SCULPTURE_BLE_RECOVERY_PLAN.md). After
  // 60s of stable uptime we assume the boot succeeded and clear the counter.
  static bool stableMarked = false;
  if (!stableMarked && millis() >= 60000) {
    SafeBootWatchdog::markStable();
    stableMarked = true;
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::INFO) {
      Serial.println(F("[BOOT] Marked stable (60s uptime) — boot-fail counter cleared"));
    }
  }

  // Flash-backed crash counter — separately clears at 5min uptime (F4).
  // Two thresholds because: the in-RAM GPREGRET2 counter wipes on power-on so
  // 60s is enough; the flash counter persists, so we want stronger evidence
  // (5 min) that the device is genuinely stable before clearing it.
  RebootFrequencyCounter::tickStable(millis());

  // Per-device "next generator" button. No-op if buttonPin == 0 in config.
  // Poll every loop iter — debounce lives inside GeneratorButton, so this
  // is just a cheap digitalRead + timestamp check on most iterations.
  if (generatorButton.poll()) {
    GeneratorButton::cycleGenerator(pipeline);
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::INFO) {
      Serial.print(F("[BUTTON] Generator -> "));
      Serial.println(pipeline->getGeneratorName());
    }
  }

  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;

  // FPS counter — always-on (cheap), queryable via `json info`. The serial
  // print remains log-level-gated so production output isn't noisy, but the
  // underlying counters are always tracked so any operator can read them.
  // Required for v36-fmax (#136) merge gate: ≥30 fps under typical load.
  LoopMetrics::tick(now);
  static uint32_t lastFpsLogMs = 0;
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::INFO &&
      LoopMetrics::getLastWindowMs() != lastFpsLogMs &&
      LoopMetrics::getLastWindowMs() != 0) {
    lastFpsLogMs = LoopMetrics::getLastWindowMs();
    Serial.print(F("[FPS] "));
    Serial.print(LoopMetrics::getFps(), 1);
    Serial.print(F(" fps  frame: "));
    Serial.print(LoopMetrics::getMinFrameMs());
    Serial.print(F("-"));
    Serial.print(LoopMetrics::getMaxFrameMs());
    Serial.print(F("ms  acf="));
    Serial.print(audioController ? audioController->getLastAcfMs() : 0);
    Serial.print(F("+"));
    Serial.print(audioController ? audioController->getLastPlpMs() : 0);
    Serial.println(F("ms"));
  }

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

  // Yield to BLE task after audio processing.
  vTaskDelay(1);

  // Advance fake audio clock when enabled
  fakeAudio.update(dt);

  // Send discrete onset events when debug channel is enabled.
  // Uses lastPulseStrength (floor-tracking baseline + rising-edge + cooldown)
  // NOT audio.pulse (continuous envelope for generators — nonzero ~60% of
  // time during music, which floods the serial port with ~35 events/sec).
  // Use: "debug transient on" to enable, "debug transient off" to disable
  if (audioController &&
      SerialConsole::isDebugChannelEnabled(DebugChannel::TRANSIENT)) {
    float onsetStrength = audioController->getLastOnsetStrength();
    if (onsetStrength > 0.0f) {
      // T1.4/T1.5: include gate_mask + spectral feature snapshot at firing
      // time so offline analysis can classify TPs vs FPs by gate state and
      // spectral signature without needing persist_raw signal_frame replay.
      const auto& pf = audioController->getLastPulseFeatures();
      Serial.print(F("{\"type\":\"TRANSIENT\",\"timestampMs\":"));
      Serial.print(now);
      Serial.print(F(",\"strength\":"));
      Serial.print(onsetStrength, 2);
      Serial.print(F(",\"gateMask\":"));
      Serial.print((unsigned int)audioController->getLastPulseGateMask());
      Serial.print(F(",\"features\":{\"flat\":"));
      Serial.print(pf.flatness, 3);
      Serial.print(F(",\"rflux\":"));
      Serial.print(pf.rawFlux, 3);
      Serial.print(F(",\"cent\":"));
      Serial.print(pf.centroid, 3);
      Serial.print(F(",\"crest\":"));
      Serial.print(pf.crest, 3);
      Serial.print(F(",\"roll\":"));
      Serial.print(pf.rolloff, 3);
      Serial.print(F(",\"hfc\":"));
      Serial.print(pf.hfc, 3);
      Serial.print(F(",\"bassR\":"));
      Serial.print(pf.bassRatio, 2);
      Serial.print(F(",\"plpP\":"));
      Serial.print(pf.plpPulse, 2);
      Serial.print(F(",\"plpC\":"));
      Serial.print(pf.plpConfidence, 2);
      Serial.println(F("}}"));
    }
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
      PlasmaGlobe* plasmaGen = pipeline->getPlasmaGlobeGenerator();

      if (fireGen && waterGen && plasmaGen) {
        configStorage.saveIfDirty(
          fireGen->getParams(),
          waterGen->getParams(),
          plasmaGen->getParams(),
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
  esp32BleNus.update();  // Drain BLE NUS TX buffer
  tcpServer.poll();  // Non-blocking TCP accept/read (all on Core 1)
  ArduinoOTA.handle();  // Non-blocking OTA check (~0.5ms)
  // Monitor WiFi and auto-reconnect
  {
      static bool wasConnected = false;
      static bool servicesStarted = false;
      bool isConnected = (WiFi.status() == WL_CONNECTED);
      if (isConnected && !wasConnected) {
          // Always disable power management and set max TX on (re)connect
          WiFi.setSleep(false);
          WiFi.setTxPower(WIFI_POWER_19_5dBm);
          if (!servicesStarted) {
              // First connection: start TCP server
              tcpServer.begin();
              servicesStarted = true;
          }
          // (Re)start mDNS and OTA on every reconnect — IP may have changed
          MDNS.end();
          MDNS.begin("blinky");
          MDNS.addService("blinky", "tcp", 3333);  // Fleet discovery
          ArduinoOTA.begin();
          Serial.print(F("[WiFi] Services started. OTA at "));
          Serial.print(WiFi.localIP());
          Serial.println(F(":3232"));
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
