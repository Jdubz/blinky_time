/**
 * blinky-simulator - LED effect visualization simulator
 *
 * Renders blinky-things generator effects to animated GIF files
 * for preview and AI-assisted iteration.
 *
 * Usage:
 *   blinky-simulator --generator fire --output preview.gif --duration 3000
 *   blinky-simulator --generator water --pattern steady-120bpm --fps 30
 *   blinky-simulator --help
 */

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

// Arduino compatibility (must be first)
#include "Arduino.h"
#include "ArduinoCompat.h"
#include "SimulatorSystemTime.h"

// Simulation components
#include "LEDImageRenderer.h"
#include "GifEncoder.h"
#include "AudioPatternLoader.h"

// Blinky-things pipeline (using relative includes)
#include "../../blinky-things/types/PixelMatrix.h"
#include "../../blinky-things/audio/AudioControl.h"
#include "../../blinky-things/devices/DeviceConfig.h"
#include "../../blinky-things/hal/mock/MockLedStrip.h"
#include "../../blinky-things/render/LEDMapper.h"
#include "../../blinky-things/render/RenderPipeline.h"

// Generator and effect types
#include "../../blinky-things/generators/Generator.h"
#include "../../blinky-things/generators/Fire.h"
#include "../../blinky-things/generators/Water.h"
#include "../../blinky-things/generators/Lightning.h"
#include "../../blinky-things/effects/Effect.h"
#include "../../blinky-things/effects/HueRotationEffect.h"

struct SimulatorConfig {
    std::string generator = "fire";
    std::string effect = "none";
    std::string pattern = "steady-120bpm";
    std::string output = "preview.gif";
    std::string device = "tube";  // tube, hat, bucket
    int durationMs = 3000;
    int fps = 30;
    int ledSize = 16;
    float hueShift = 0.0f;
    bool verbose = false;
    bool showHelp = false;
};

void printHelp() {
    std::cout << R"(
blinky-simulator - LED effect visualization tool

USAGE:
    blinky-simulator [OPTIONS]

OPTIONS:
    --generator, -g <name>   Generator to use: fire, water, lightning (default: fire)
    --effect, -e <name>      Effect to apply: none, hue (default: none)
    --pattern, -p <name>     Audio pattern: steady-120bpm, steady-90bpm, steady-140bpm,
                             silence, burst, complex, or path to pattern file
    --output, -o <file>      Output GIF filename (default: preview.gif)
    --device, -d <name>      Device config: tube (4x15), hat (89 string), bucket (16x8)
    --duration, -t <ms>      Duration in milliseconds (default: 3000)
    --fps, -f <num>          Frames per second (default: 30)
    --led-size <pixels>      LED circle size in pixels (default: 16)
    --hue <0.0-1.0>          Hue shift for hue effect (default: 0.0)
    --verbose, -v            Verbose output
    --help, -h               Show this help message

EXAMPLES:
    # Generate 3-second fire preview at 30 FPS
    blinky-simulator -g fire -o fire.gif

    # Generate water effect with complex audio pattern
    blinky-simulator -g water -p complex -t 5000 -o water.gif

    # Generate lightning with hue shift (blue lightning)
    blinky-simulator -g lightning -e hue --hue 0.6 -o blue-lightning.gif

    # Use bucket totem device (16x8 matrix)
    blinky-simulator -g fire -d bucket -o bucket-fire.gif

OUTPUT:
    Creates an animated GIF file showing the LED visualization.
    The GIF can be used for preview or AI-assisted parameter tuning.

)" << std::endl;
}

bool parseArgs(int argc, char* argv[], SimulatorConfig& config) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            config.showHelp = true;
            return true;
        } else if (arg == "--verbose" || arg == "-v") {
            config.verbose = true;
        } else if ((arg == "--generator" || arg == "-g") && i + 1 < argc) {
            config.generator = argv[++i];
        } else if ((arg == "--effect" || arg == "-e") && i + 1 < argc) {
            config.effect = argv[++i];
        } else if ((arg == "--pattern" || arg == "-p") && i + 1 < argc) {
            config.pattern = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            config.output = argv[++i];
        } else if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
            config.device = argv[++i];
        } else if ((arg == "--duration" || arg == "-t") && i + 1 < argc) {
            config.durationMs = std::atoi(argv[++i]);
        } else if ((arg == "--fps" || arg == "-f") && i + 1 < argc) {
            config.fps = std::atoi(argv[++i]);
        } else if (arg == "--led-size" && i + 1 < argc) {
            config.ledSize = std::atoi(argv[++i]);
        } else if (arg == "--hue" && i + 1 < argc) {
            config.hueShift = std::atof(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return false;
        }
    }
    return true;
}

DeviceConfig createDeviceConfig(const std::string& device) {
    DeviceConfig config;

    if (device == "tube" || device == "tubelight") {
        config.deviceName = "TubeLight";
        config.matrix.width = 4;
        config.matrix.height = 15;
        config.matrix.ledPin = 0;
        config.matrix.brightness = 255;
        config.matrix.ledType = 0;
        config.matrix.orientation = VERTICAL;
        config.matrix.layoutType = MATRIX_LAYOUT;
    } else if (device == "hat") {
        config.deviceName = "Hat";
        config.matrix.width = 89;
        config.matrix.height = 1;
        config.matrix.ledPin = 0;
        config.matrix.brightness = 255;
        config.matrix.ledType = 0;
        config.matrix.orientation = HORIZONTAL;
        config.matrix.layoutType = LINEAR_LAYOUT;
    } else if (device == "bucket" || device == "totem") {
        config.deviceName = "BucketTotem";
        config.matrix.width = 16;
        config.matrix.height = 8;
        config.matrix.ledPin = 0;
        config.matrix.brightness = 255;
        config.matrix.ledType = 0;
        config.matrix.orientation = HORIZONTAL;
        config.matrix.layoutType = MATRIX_LAYOUT;
    } else {
        // Default to tube
        config.deviceName = "Default";
        config.matrix.width = 4;
        config.matrix.height = 15;
        config.matrix.orientation = VERTICAL;
        config.matrix.layoutType = MATRIX_LAYOUT;
    }

    // Common defaults
    config.charging.fastChargeEnabled = false;
    config.charging.lowBatteryThreshold = 3.5f;
    config.charging.criticalBatteryThreshold = 3.3f;
    config.charging.minVoltage = 3.0f;
    config.charging.maxVoltage = 4.2f;

    config.fireDefaults.baseCooling = 55;
    config.fireDefaults.sparkHeatMin = 150;
    config.fireDefaults.sparkHeatMax = 255;
    config.fireDefaults.sparkChance = 0.4f;
    config.fireDefaults.audioSparkBoost = 0.3f;
    config.fireDefaults.coolingAudioBias = 0;
    config.fireDefaults.bottomRowsForSparks = 2;

    return config;
}

int main(int argc, char* argv[]) {
    SimulatorConfig config;

    if (!parseArgs(argc, argv, config)) {
        std::cerr << "Use --help for usage information." << std::endl;
        return 1;
    }

    if (config.showHelp) {
        printHelp();
        return 0;
    }

    if (config.verbose) {
        std::cout << "blinky-simulator v1.0" << std::endl;
        std::cout << "  Generator: " << config.generator << std::endl;
        std::cout << "  Effect: " << config.effect << std::endl;
        std::cout << "  Pattern: " << config.pattern << std::endl;
        std::cout << "  Device: " << config.device << std::endl;
        std::cout << "  Duration: " << config.durationMs << " ms" << std::endl;
        std::cout << "  FPS: " << config.fps << std::endl;
        std::cout << "  Output: " << config.output << std::endl;
    }

    // Initialize random seed for reproducibility
    randomSeed(42);

    // Set up simulated time
    SimulatorTime::setSimulatedTime(0);

    // Create device configuration
    DeviceConfig deviceConfig = createDeviceConfig(config.device);
    int numLeds = deviceConfig.matrix.width * deviceConfig.matrix.height;

    if (config.verbose) {
        std::cout << "  LED count: " << numLeds << " (" << (int)deviceConfig.matrix.width
                  << "x" << (int)deviceConfig.matrix.height << ")" << std::endl;
    }

    // Create mock LED strip
    MockLedStrip leds(numLeds);
    leds.begin();

    // Create LED mapper
    LEDMapper mapper;
    if (!mapper.begin(deviceConfig)) {
        std::cerr << "Failed to initialize LED mapper" << std::endl;
        return 1;
    }

    // Create render pipeline
    RenderPipeline pipeline;
    if (!pipeline.begin(deviceConfig, leds, mapper)) {
        std::cerr << "Failed to initialize render pipeline" << std::endl;
        return 1;
    }

    // Set generator
    GeneratorType genType = GeneratorType::FIRE;
    if (config.generator == "water") {
        genType = GeneratorType::WATER;
    } else if (config.generator == "lightning") {
        genType = GeneratorType::LIGHTNING;
    }
    pipeline.setGenerator(genType);

    // Set effect
    if (config.effect == "hue" || config.effect == "huerotation") {
        pipeline.setEffect(EffectType::HUE_ROTATION);
        if (pipeline.getHueRotationEffect()) {
            pipeline.getHueRotationEffect()->setHueShift(config.hueShift);
        }
    } else {
        pipeline.setEffect(EffectType::NONE);
    }

    if (config.verbose) {
        std::cout << "  Active generator: " << pipeline.getGeneratorName() << std::endl;
        std::cout << "  Active effect: " << pipeline.getEffectName() << std::endl;
    }

    // Load audio pattern
    AudioPattern audioPattern = AudioPatternLoader::getPattern(config.pattern, config.durationMs);

    if (config.verbose) {
        std::cout << "  Audio pattern: " << audioPattern.getName()
                  << " (" << audioPattern.getDuration() << " ms)" << std::endl;
    }

    // Configure LED image renderer
    LEDRenderConfig renderConfig;
    renderConfig.ledWidth = deviceConfig.matrix.width;
    renderConfig.ledHeight = deviceConfig.matrix.height;
    renderConfig.ledSize = config.ledSize;
    renderConfig.ledSpacing = 4;
    renderConfig.padding = 10;
    renderConfig.drawGlow = true;

    // Choose layout style based on device
    if (config.device == "hat") {
        renderConfig.style = LEDLayoutStyle::STRIP;
    } else {
        renderConfig.style = LEDLayoutStyle::GRID;
    }

    LEDImageRenderer imageRenderer;
    imageRenderer.configure(renderConfig);

    if (config.verbose) {
        std::cout << "  Image size: " << imageRenderer.getWidth() << "x"
                  << imageRenderer.getHeight() << std::endl;
    }

    // Create GIF encoder
    GifEncoder gif;
    if (!gif.begin(config.output, imageRenderer.getWidth(), imageRenderer.getHeight(), config.fps)) {
        std::cerr << "Failed to create output file: " << config.output << std::endl;
        return 1;
    }

    // Calculate frame timing
    int frameIntervalMs = 1000 / config.fps;
    int totalFrames = config.durationMs / frameIntervalMs;

    if (config.verbose) {
        std::cout << "  Rendering " << totalFrames << " frames..." << std::endl;
    }

    // Render frames
    for (int frame = 0; frame < totalFrames; frame++) {
        uint32_t timeMs = frame * frameIntervalMs;

        // Update simulated time
        SimulatorTime::setSimulatedTime(timeMs);

        // Get audio state for this time
        AudioControl audio = audioPattern.getAudioAt(timeMs);

        // Render frame through pipeline
        pipeline.render(audio);
        leds.show();

        // Render LEDs to image
        imageRenderer.render(leds);

        // Add frame to GIF
        gif.addFrame(imageRenderer.getBuffer());

        // Progress indicator
        if (config.verbose && frame % 30 == 0) {
            std::cout << "  Frame " << frame << "/" << totalFrames
                      << " (" << (100 * frame / totalFrames) << "%)" << std::endl;
        }
    }

    // Finish GIF
    gif.close();

    size_t fileSize = GifEncoder::getFileSize(config.output);
    std::cout << "Created " << config.output << " (" << fileSize << " bytes, "
              << totalFrames << " frames)" << std::endl;

    return 0;
}
