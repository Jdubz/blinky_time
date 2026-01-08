# blinky-simulator

LED effect visualization simulator for the blinky-things Arduino firmware.

Renders Fire, Water, and Lightning generator effects to animated GIF previews.

## Purpose

The simulator exists to **test the actual firmware code** without hardware. This is critical because:

1. **Same code, same behavior** - The simulator compiles and runs the exact same C++ generators (`blinky-things/generators/*.cpp`) that run on the device. Any change to the Arduino sketch automatically changes the simulation.

2. **Catch bugs before flashing** - Firmware bugs can be identified visually before uploading to hardware, avoiding the slow flash-test-debug cycle.

3. **AI-assisted iteration** - Parameter tuning can be done with rapid visual feedback, generating GIF previews in seconds.

4. **No hardware divergence** - Unlike a reimplementation (e.g., TypeScript port), the simulator can never drift out of sync with the firmware.

## Architecture

The simulator uses Arduino compatibility shims to compile the firmware on desktop:

```
blinky-things/generators/   --> Fire.cpp, Water.cpp, Lightning.cpp (ACTUAL CODE)
blinky-things/effects/      --> HueRotationEffect.cpp (ACTUAL CODE)
blinky-things/render/       --> RenderPipeline.cpp (ACTUAL CODE)
blinky-things/hal/mock/     --> MockLedStrip.h (captures LED output)

blinky-simulator/include/   --> ArduinoCompat.h (Arduino stubs for desktop)
                            --> LEDImageRenderer.h (LED state to image)
                            --> GifEncoder.h (animated GIF output)
                            --> AudioPatternLoader.h (scripted audio patterns)
```

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

```bash
# Fire effect preview (default)
./build/blinky-simulator -g fire -o fire.gif

# Water effect with 5 second duration
./build/blinky-simulator -g water -t 5000 -o water.gif

# Lightning with blue hue shift
./build/blinky-simulator -g lightning -e hue --hue 0.6 -o lightning.gif

# Different device configurations
./build/blinky-simulator -g fire -d bucket -o bucket.gif  # 16x8 matrix
./build/blinky-simulator -g fire -d tube -o tube.gif      # 4x15 vertical
./build/blinky-simulator -g fire -d hat -o hat.gif        # 89 LED strip

# Different audio patterns
./build/blinky-simulator -g fire -p complex -o complex-fire.gif
./build/blinky-simulator -g fire -p silence -o silent-fire.gif
```

## Options

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--generator` | `-g` | Generator: fire, water, lightning | fire |
| `--effect` | `-e` | Effect: none, hue | none |
| `--pattern` | `-p` | Audio pattern (see below) | steady-120bpm |
| `--output` | `-o` | Output GIF filename | preview.gif |
| `--device` | `-d` | Device: tube, hat, bucket | bucket |
| `--duration` | `-t` | Duration in ms | 3000 |
| `--fps` | `-f` | Frames per second | 30 |
| `--led-size` | | LED circle size in pixels | 16 |
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

- Animated GIF with LED glow effects
- Layout matches physical device arrangement
- 30 FPS default (matches firmware update rate)
- Typical size: 50-200 KB for 3-second clips
- Output to `previews/` folder (gitignored)

## Integration with MCP

Works with blinky-serial-mcp for AI-assisted parameter tuning:

```javascript
// MCP tool call
render_preview({
  generator: "fire",
  settings: { gravity: -2.5, sparkSpread: 0.8 },
  audio_pattern: "steady-120bpm",
  duration_ms: 3000,
  output_path: "preview.gif"
})
```

## License

Same license as the blinky_time project.
