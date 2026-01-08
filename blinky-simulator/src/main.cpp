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
#include <sys/stat.h>
#include <ctime>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <direct.h>
#endif

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

// Parameter injection for agent-assisted optimization
#include "ParamParser.h"
#include "MetricsCalculator.h"

struct SimulatorConfig {
    std::string generator = "fire";
    std::string effect = "none";
    std::string pattern = "steady-120bpm";
    std::string device = "bucket";  // bucket, tube, hat
    std::string params = "";        // Runtime param overrides: key=val,key=val
    int durationMs = 3000;
    int fps = 30;
    float hueShift = 0.0f;
    bool verbose = false;
    bool showHelp = false;
};

// Fixed output directory (gitignored)
const std::string OUTPUT_DIR = "previews";

// Create directory (cross-platform)
void createDir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

// Get timestamp string for unique filenames (YYYYMMDD-HHMMSS)
std::string getTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y%m%d-%H%M%S");
    return oss.str();
}

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
    --device, -d <name>      Device config: bucket (16x8), tube (4x15), hat (89 string) [default: bucket]
    --duration, -t <ms>      Duration in milliseconds (default: 3000)
    --fps, -f <num>          Frames per second (default: 30)
    --hue <0.0-1.0>          Hue shift for hue effect (default: 0.0)
    --params <key=val,...>   Override generator params (e.g., "baseSpawnChance=0.15,gravity=-12")
    --verbose, -v            Verbose output
    --help, -h               Show this help message

EXAMPLES:
    # Generate fire preview (16x8 bucket, outputs both resolutions)
    blinky-simulator -g fire

    # Generate water effect with complex audio pattern
    blinky-simulator -g water -p complex -t 5000

    # Generate lightning with hue shift
    blinky-simulator -g lightning -e hue --hue 0.6

    # Use tube device (4x15 matrix)
    blinky-simulator -g fire -d tube

OUTPUT:
    Creates TWO animated GIFs with timestamp in previews/ (gitignored):
      previews/low-res/<generator>-<timestamp>.gif   - Exact LED pixels (for AI)
      previews/high-res/<generator>-<timestamp>.gif  - Human-readable preview

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
        } else if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
            config.device = argv[++i];
        } else if ((arg == "--duration" || arg == "-t") && i + 1 < argc) {
            config.durationMs = std::atoi(argv[++i]);
        } else if ((arg == "--fps" || arg == "-f") && i + 1 < argc) {
            config.fps = std::atoi(argv[++i]);
        } else if (arg == "--hue" && i + 1 < argc) {
            config.hueShift = std::atof(argv[++i]);
        } else if (arg == "--params" && i + 1 < argc) {
            config.params = argv[++i];
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
    }

    // Initialize random seed with time for varied output each run
    // This prevents particle clustering from deterministic sequences
    randomSeed((unsigned long)time(nullptr));

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

    // Parse and apply parameter overrides
    ParamMap paramOverrides = ParamParser::parse(config.params);
    ParamMap allParams;

    if (genType == GeneratorType::FIRE && pipeline.getFireGenerator()) {
        applyParams(pipeline.getFireGenerator()->getParamsMutable(), paramOverrides);
        allParams = getParamMap(pipeline.getFireGenerator()->getParams());
    } else if (genType == GeneratorType::WATER && pipeline.getWaterGenerator()) {
        applyParams(pipeline.getWaterGenerator()->getParamsMutable(), paramOverrides);
        allParams = getParamMap(pipeline.getWaterGenerator()->getParams());
    } else if (genType == GeneratorType::LIGHTNING && pipeline.getLightningGenerator()) {
        applyParams(pipeline.getLightningGenerator()->getParamsMutable(), paramOverrides);
        allParams = getParamMap(pipeline.getLightningGenerator()->getParams());
    }

    if (config.verbose && !paramOverrides.empty()) {
        std::cout << "  Param overrides: " << paramOverrides.size() << " values" << std::endl;
    }

    // Load audio pattern
    AudioPattern audioPattern = AudioPatternLoader::getPattern(config.pattern, config.durationMs);

    if (config.verbose) {
        std::cout << "  Audio pattern: " << audioPattern.getName()
                  << " (" << audioPattern.getDuration() << " ms)" << std::endl;
    }

    // Create output directories (fixed to previews/, which is gitignored)
    std::string lowResDir = OUTPUT_DIR + "/low-res";
    std::string highResDir = OUTPUT_DIR + "/high-res";
    createDir(OUTPUT_DIR);
    createDir(lowResDir);
    createDir(highResDir);

    // Generate output filenames with timestamp for unique runs
    std::string timestamp = getTimestamp();
    std::string filename = config.generator + "-" + timestamp + ".gif";
    std::string lowResPath = lowResDir + "/" + filename;
    std::string highResPath = highResDir + "/" + filename;

    // Layout style based on device
    LEDLayoutStyle layoutStyle = (config.device == "hat") ? LEDLayoutStyle::STRIP : LEDLayoutStyle::GRID;

    // Configure LOW-RES renderer (exact pixels for AI)
    LEDRenderConfig lowResConfig;
    lowResConfig.ledWidth = deviceConfig.matrix.width;
    lowResConfig.ledHeight = deviceConfig.matrix.height;
    lowResConfig.ledSize = 1;
    lowResConfig.ledSpacing = 0;
    lowResConfig.padding = 0;
    lowResConfig.drawGlow = false;
    lowResConfig.style = layoutStyle;

    LEDImageRenderer lowResRenderer;
    lowResRenderer.configure(lowResConfig);

    // Configure HIGH-RES renderer (human readable)
    LEDRenderConfig highResConfig;
    highResConfig.ledWidth = deviceConfig.matrix.width;
    highResConfig.ledHeight = deviceConfig.matrix.height;
    highResConfig.ledSize = 8;
    highResConfig.ledSpacing = 2;
    highResConfig.padding = 4;
    highResConfig.drawGlow = false;
    highResConfig.style = layoutStyle;

    LEDImageRenderer highResRenderer;
    highResRenderer.configure(highResConfig);

    if (config.verbose) {
        std::cout << "  Low-res: " << lowResRenderer.getWidth() << "x"
                  << lowResRenderer.getHeight() << " -> " << lowResPath << std::endl;
        std::cout << "  High-res: " << highResRenderer.getWidth() << "x"
                  << highResRenderer.getHeight() << " -> " << highResPath << std::endl;
    }

    // Create GIF encoders
    GifEncoder lowResGif, highResGif;
    if (!lowResGif.begin(lowResPath, lowResRenderer.getWidth(), lowResRenderer.getHeight(), config.fps)) {
        std::cerr << "Failed to create: " << lowResPath << std::endl;
        return 1;
    }
    if (!highResGif.begin(highResPath, highResRenderer.getWidth(), highResRenderer.getHeight(), config.fps)) {
        std::cerr << "Failed to create: " << highResPath << std::endl;
        return 1;
    }

    // Calculate frame timing
    int frameIntervalMs = 1000 / config.fps;
    int totalFrames = config.durationMs / frameIntervalMs;

    if (config.verbose) {
        std::cout << "  Rendering " << totalFrames << " frames..." << std::endl;
    }

    // Initialize metrics calculator
    MetricsCalculator metrics;
    metrics.reset();
    std::vector<uint8_t> ledBuffer(numLeds * 3);

    // Render frames to both outputs
    for (int frame = 0; frame < totalFrames; frame++) {
        uint32_t timeMs = frame * frameIntervalMs;

        // Update simulated time
        SimulatorTime::setSimulatedTime(timeMs);

        // Get audio state for this time
        AudioControl audio = audioPattern.getAudioAt(timeMs);

        // Render frame through pipeline
        pipeline.render(audio);
        leds.show();

        // Render to both image buffers
        lowResRenderer.render(leds);
        highResRenderer.render(leds);

        // Add frames to both GIFs
        lowResGif.addFrame(lowResRenderer.getBuffer());
        highResGif.addFrame(highResRenderer.getBuffer());

        // Collect metrics from raw LED data
        for (int i = 0; i < numLeds; i++) {
            uint32_t color = leds.getPixelColor(i);
            ledBuffer[i * 3] = (color >> 16) & 0xFF;     // R
            ledBuffer[i * 3 + 1] = (color >> 8) & 0xFF;  // G
            ledBuffer[i * 3 + 2] = color & 0xFF;         // B
        }
        metrics.processFrame(ledBuffer.data(), numLeds);

        // Progress indicator
        if (config.verbose && frame % 30 == 0) {
            std::cout << "  Frame " << frame << "/" << totalFrames
                      << " (" << (100 * frame / totalFrames) << "%)" << std::endl;
        }
    }

    // Finish both GIFs
    lowResGif.close();
    highResGif.close();

    // Write params.json (for agent-assisted iteration)
    std::string paramsJsonPath = lowResDir + "/" + config.generator + "-" + timestamp + "-params.json";
    ParamParser::writeJson(paramsJsonPath, config.generator, paramOverrides, allParams);

    // Write metrics.json (quantitative feedback for optimization)
    std::string metricsJsonPath = lowResDir + "/" + config.generator + "-" + timestamp + "-metrics.json";
    VisualMetrics visualMetrics = metrics.compute();
    MetricsCalculator::writeJson(metricsJsonPath, visualMetrics);

    std::cout << "Created:" << std::endl;
    std::cout << "  " << lowResPath << " (" << GifEncoder::getFileSize(lowResPath) << " bytes)" << std::endl;
    std::cout << "  " << highResPath << " (" << GifEncoder::getFileSize(highResPath) << " bytes)" << std::endl;
    std::cout << "  " << paramsJsonPath << std::endl;
    std::cout << "  " << metricsJsonPath << std::endl;

    // Print key metrics summary
    std::cout << "\nMetrics summary:" << std::endl;
    std::cout << "  Brightness: avg=" << visualMetrics.avgBrightness
              << ", range=" << visualMetrics.dynamicRange << std::endl;
    std::cout << "  Activity: avg=" << visualMetrics.avgActivity
              << ", peak=" << visualMetrics.peakActivity << std::endl;
    std::cout << "  Color: saturation=" << visualMetrics.avgSaturation
              << ", hueSpread=" << visualMetrics.hueSpread << std::endl;
    std::cout << "  Lit pixels: " << visualMetrics.litPixelPercent << "%" << std::endl;

    return 0;
}
