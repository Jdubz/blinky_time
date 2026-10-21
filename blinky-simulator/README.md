# blinky-simulator

LED effect visualization simulator for the blinky-things Arduino firmware.

Renders the Fire, Water, PlasmaGlobe ("lightning") and Audio generators to animated GIF previews, plus visual metrics.

## Purpose

The simulator exists to **test the actual firmware code** without hardware. This is critical because:

1. **Same code, same behavior** - The simulator compiles and runs the exact same C++ generators (`blinky-things/generators/*.cpp`) that run on the device. Any change to the Arduino sketch automatically changes the simulation.

2. **Catch bugs before flashing** - Firmware bugs can be identified visually before uploading to hardware, avoiding the slow flash-test-debug cycle.

3. **AI-assisted iteration** - Parameter tuning can be done with rapid visual feedback, generating GIF previews in seconds.

4. **No hardware divergence** - Unlike a reimplementation (e.g., TypeScript port), the simulator can never drift out of sync with the firmware.

## Architecture

The simulator uses Arduino compatibility shims to compile the firmware on desktop:

```
blinky-things/generators/   --> Fire.cpp, Water.cpp, PlasmaGlobe.cpp, Audio.cpp (ACTUAL CODE)
blinky-things/particles/, physics/, math/, types/ --> shared generator code (ACTUAL CODE)
blinky-things/effects/      --> HueRotationEffect.cpp (ACTUAL CODE)
blinky-things/render/       --> RenderPipeline.cpp (ACTUAL CODE)
blinky-things/hal/mock/     --> MockLedStrip.h (captures LED output)

blinky-simulator/include/   --> ArduinoCompat.h (Arduino stubs for desktop)
                            --> LEDImageRenderer.h (LED state to image)
                            --> GifEncoder.h (animated GIF output)
                            --> AudioPatternLoader.h (scripted audio patterns)
                            --> MetricsCalculator.h (visual metrics)
                            --> ParamParser.h (--params overrides, params.json)
```

The same stubs are used by the host tests in `tests/` (timing, tracker, parity). When you add a generator source file, add it to `CMakeLists.txt`, `build.sh`, `build.bat` and `build_vs.cmd`.

The `ArduinoCompat.h` layer provides desktop implementations for:
- `millis()` - Controllable simulated time
- `random()` - Seeded random for reproducibility
- `constrain()`, `min()`, `max()` - Math helpers
- `Serial` - Debug output to stdout

## Prerequisites

A C++ compiler is required:

**Windows:**
- Visual Studio 2019+ with C++ Desktop workload, or
- MinGW-w64 with g++

**macOS:**
```bash
xcode-select --install
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt install g++ cmake
```

## Build

**Windows (Visual Studio):**
```cmd
cd blinky-simulator
build.bat
```

**Unix/macOS:**
```bash
cd blinky-simulator
chmod +x build.sh
./build.sh
```

**CMake (cross-platform):**
```bash
cd blinky-simulator
cmake -B build -S .
cmake --build build
```

## Usage

`-o` takes an output **directory** (default `previews`). Each run overwrites the files in it, so use a different directory to keep A/B results.

```bash
# Fire effect preview (default) -> previews/
./build/blinky-simulator -g fire

# Water effect with 5 second duration
./build/blinky-simulator -g water -t 5000 -o previews/water

# Plasma globe ("lightning") with blue hue shift
./build/blinky-simulator -g lightning -e hue --hue 0.6 -o previews/lightning

# Different device configurations
./build/blinky-simulator -g fire -d bucket -o previews/bucket  # 16x8 matrix
./build/blinky-simulator -g fire -d tube -o previews/tube      # 4x15 vertical
./build/blinky-simulator -g fire -d hat -o previews/hat        # 89 LED strip

# Different audio patterns
./build/blinky-simulator -g fire -p complex -o previews/complex
./build/blinky-simulator -g fire -p silence -o previews/silent

# Override generator parameters
./build/blinky-simulator -g fire --params "baseSpawnChance=0.15,gravity=-12"
```

## Options

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--generator` | `-g` | Generator: fire, water, lightning (alias plasma), audio | fire |
| `--effect` | `-e` | Effect: none, hue | none |
| `--pattern` | `-p` | Audio pattern (see below) | steady-120bpm |
| `--output` | `-o` | Output directory | previews |
| `--device` | `-d` | Device: tube, hat, bucket | bucket |
| `--duration` | `-t` | Duration in ms | 3000 |
| `--fps` | `-f` | Frames per second | 30 |
| `--params` | | Generator parameter overrides (`key=val,...`) | |
| `--hue` | | Hue shift for hue effect (0.0-1.0) | 0.0 |
| `--verbose` | `-v` | Verbose output | false |
| `--help` | `-h` | Show help | |

## Audio Patterns

Built-in patterns for testing rhythm/transient response:

| Pattern | Description |
|---------|-------------|
| `steady-120bpm` | Steady 120 BPM beat (default) |
| `steady-90bpm` | Slower 90 BPM |
| `steady-140bpm` | Fast 140 BPM |
| `silence` | No audio (tests organic/ambient mode) |
| `burst` | Random transient bursts |
| `complex` | Building rhythm with variations |
| `realistic` | Music-like levels and onsets |
| `ambient` | Quiet, low-level audio |

Any other value is read as a file path. Warning: an unknown name or unreadable file silently falls back to `steady-120bpm`, and `meta.json` still records the name you passed. Run with `-v` to see which pattern was actually loaded.

Custom patterns can be loaded from CSV files:
```csv
# time_ms,energy,pulse,phase,rhythm_strength
0,0.3,0.0,0.0,0.8
500,0.8,1.0,0.0,0.8
```

## Device Configurations

| Device | Size | Layout | Physical Form |
|--------|------|--------|---------------|
| bucket | 16x8 | Matrix (default) | Bucket totem |
| tube | 4x15 | Vertical zigzag | Tube light |
| hat | 89x1 | Horizontal strip | LED hat |

## Output

Each run writes into the output directory (`previews/` by default, gitignored):

| File | Content |
|------|---------|
| `low-res.gif` | Exact LED pixels, one pixel per LED (for AI analysis) |
| `high-res.gif` | Scaled, human-readable preview |
| `params.json` | Generator parameters used |
| `metrics.json` | Visual metrics (brightness, activity, color) |
| `meta.json` | Run configuration (generator, device, duration, ...) |

Layout matches the physical device arrangement.

## Integration with MCP

Works with blinky-serial-mcp for AI-assisted parameter tuning:

```javascript
// MCP tool call (blinky-serial-mcp/src/index.ts)
render_preview({
  generator: "fire",
  params: "baseSpawnChance=0.15,gravity=-12",
  pattern: "steady-120bpm",
  device: "bucket",
  duration_ms: 3000,
  output_dir: "previews/fire-test"
})
```

The MCP tool runs `build/blinky-simulator`, so build the simulator first (`./build.sh`). Its `generator` option does not offer `audio` yet.

## License

Same license as the blinky_time project.
