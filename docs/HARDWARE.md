# Hardware Documentation

This directory contains hardware-specific documentation for Blinky Time LED fire effect controllers.

## Device Configurations

### nRF52840 XIAO Sense
- **Tube Light Setup** - 4x15 zigzag matrix installation guide
- **Hat Installation** - Wearable 89-LED string configuration  
- **Bucket Totem** - 16x8 matrix display setup

## Wiring Diagrams

### Basic Connections
```
nRF52840 XIAO Sense Pinout:
├── D10 (GPIO) → LED Data Pin
├── 3V3 → LED Power (VCC)
├── GND → LED Ground
├── Built-in Mic → Audio input
└── Built-in IMU → Motion sensing
```

### Power Requirements
- **LED Current**: ~60mA per LED at full brightness
- **Controller Current**: ~50mA during operation
- **Battery Recommendation**: 3.7V Li-Po, 1000mAh+ for portable use

## LED Strip Specifications

### Supported Types
- **WS2812B** (NeoPixel) - Most common, NEO_GRB color order
- **SK6812** - Similar to WS2812B with white channel option
- **WS2813** - Backup data line for improved reliability

### Color Order Configuration
Critical setting in device configs:
```cpp
// Most common for WS2812B strips
NEO_GRB + NEO_KHZ800

// Alternative for some SK6812 variants  
NEO_RGB + NEO_KHZ800
```

## Installation Notes

### Zigzag Matrix Wiring
For tube lights and matrix displays, LEDs are wired in a zigzag pattern:
```
Column 0: LEDs 0-14 (bottom to top)
Column 1: LEDs 29-15 (top to bottom) 
Column 2: LEDs 30-44 (bottom to top)
Column 3: LEDs 59-45 (top to bottom)
```

### Enclosure Considerations
- **Waterproofing**: IP65 rating recommended for outdoor use
- **Heat Dissipation**: Provide ventilation for high LED counts
- **Access Ports**: Programming connector and charging port access

## Troubleshooting

### Common Issues
1. **Wrong Color Order** - Check NEO_RGB vs NEO_GRB in config
2. **Power Supply** - Insufficient current causes dim/flickering LEDs
3. **Data Signal** - Use proper gauge wire for data connections
4. **Ground Loops** - Ensure common ground between controller and LEDs

### Testing Procedures
1. **Single LED Test** - Verify basic connectivity
2. **Color Order Test** - Check red/green/blue display correctly  
3. **Full Matrix Test** - Validate zigzag mapping pattern
4. **Audio Test** - Confirm microphone responsiveness