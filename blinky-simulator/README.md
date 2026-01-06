# blinky-simulator

LED effect visualization simulator for the blinky-things Arduino firmware.

Renders Fire, Water, and Lightning generator effects to animated GIF previews. Designed for:
- **Visual preview** before flashing to device
- **AI-assisted iteration** on effect parameters
- **Testing** effect behavior with scripted audio patterns

## Features

- Fire, Water, Lightning generators (ported from firmware)
- Supports all device configurations (Tube, Hat, Bucket)
- Scripted audio patterns (BPM-synced beats, transients, silence)
- Animated GIF output with LED glow effects
- CLI interface for automation and AI integration
- MCP tool integration via blinky-serial-mcp

## Quick Start (Node.js - Recommended)

The easiest way to use the simulator is via Node.js (no C++ compiler needed):

```bash
cd blinky-simulator
npm install
npm run build
node dist/cli.js --help
```

### Quick Examples

```bash
# Fire effect preview
node dist/cli.js -g fire -o fire.gif

# Water effect with 5 second duration
node dist/cli.js -g water -t 5000 -o water.gif

# Lightning with blue hue shift
node dist/cli.js -g lightning -e hue --hue 0.6 -o lightning.gif
```

## Alternative: C++ Build

If you have a C++ compiler and want to run the actual firmware code:

### Prerequisites

**Windows:**
- Visual Studio 2019+ with C++ workload, or
- MinGW-w64 with g++

**macOS:**
- Xcode Command Line Tools: `xcode-select --install`

**Linux:**
- GCC: `sudo apt install g++`

### Build Commands

**Windows (batch):**
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

**CMake (if available):**
```bash
cd blinky-simulator
mkdir build && cd build
cmake ..
cmake --build .
```

## Usage

```bash
# Basic fire effect preview
./blinky-simulator -g fire -o fire.gif

# Water effect with 5 second duration
./blinky-simulator -g water -t 5000 -o water.gif

# Lightning with hue shift (blue lightning)
./blinky-simulator -g lightning -e hue --hue 0.6 -o lightning.gif

# Different device (bucket totem 16x8)
./blinky-simulator -g fire -d bucket -o bucket.gif

# Complex audio pattern
./blinky-simulator -g fire -p complex -o complex-fire.gif

# Silent (no audio) for baseline visualization
./blinky-simulator -g fire -p silence -o silent-fire.gif
```

## Options

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--generator` | `-g` | Generator: fire, water, lightning | fire |
| `--effect` | `-e` | Effect: none, hue | none |
| `--pattern` | `-p` | Audio pattern (see below) | steady-120bpm |
| `--output` | `-o` | Output GIF filename | preview.gif |
| `--device` | `-d` | Device: tube, hat, bucket | tube |
| `--duration` | `-t` | Duration in ms | 3000 |
| `--fps` | `-f` | Frames per second | 30 |
| `--led-size` | | LED circle size in pixels | 16 |
| `--hue` | | Hue shift for hue effect (0.0-1.0) | 0.0 |
| `--verbose` | `-v` | Verbose output | false |
| `--help` | `-h` | Show help | |

## Audio Patterns

Built-in patterns:
- `steady-120bpm` - Steady 120 BPM beat (default)
- `steady-90bpm` - Slower 90 BPM beat
- `steady-140bpm` / `fast` - Fast 140 BPM beat
- `silence` / `silent` - No audio (organic mode)
- `burst` / `bursts` - Random transient bursts
- `complex` - Complex pattern with building rhythm

Custom patterns can be loaded from CSV files with format:
```
# time_ms,energy,pulse,phase,rhythm_strength
0,0.3,0.0,0.0,0.8
500,0.8,1.0,0.0,0.8
...
```

## Device Configurations

| Device | Size | Layout | Best For |
|--------|------|--------|----------|
| tube | 4x15 | Vertical zigzag | Tube light |
| hat | 89x1 | Horizontal strip | LED hat |
| bucket | 16x8 | Horizontal matrix | Bucket totem |

## Integration with MCP

The simulator is designed to work with the blinky-serial-mcp server for AI-assisted parameter tuning:

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

## Architecture

The simulator reuses the actual firmware code:

```
blinky-things/generators/   → Fire.cpp, Water.cpp, Lightning.cpp
blinky-things/effects/      → HueRotationEffect.cpp
blinky-things/render/       → RenderPipeline.cpp, EffectRenderer.cpp
blinky-things/hal/mock/     → MockLedStrip.h (captures LED output)

blinky-simulator/include/   → ArduinoCompat.h (Arduino stubs)
                            → LEDImageRenderer.h (LED→image)
                            → GifEncoder.h (animated GIF output)
                            → AudioPatternLoader.h (scripted audio)
```

The Arduino compatibility layer (`ArduinoCompat.h`) provides stubs for:
- `millis()` - Controllable simulated time
- `random()` - Seeded random for reproducibility
- `constrain()`, `min()`, `max()` - Math helpers
- `Serial` - Debug output to stdout

## Output

Creates animated GIFs showing the LED visualization:
- Each LED rendered as a glowing circle
- Layout matches physical device arrangement
- Timing matches real-time playback (30 FPS default)
- File size typically 50-200 KB for 3-second clips

## License

Same license as the blinky_time project.
