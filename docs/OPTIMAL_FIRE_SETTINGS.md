# Optimal Fire Effect Settings

**Commit:** `91756f7 just fire`
**Date:** September 17, 2025
**Description:** Pure fire effect without torch simulation - optimal visual levels and music responsiveness

## Key Parameters

### Hardware/Visual Settings
- **LED Brightness:** `80` (in fire-totem.ino)
- **Auto-gain Target:** `0.55` (55% of visual range)

### Fire Engine Parameters
- **Base Cooling:** `80` (reduced for less aggressive cooling)
- **Spark Chance:** `0.4` (moderate spark generation)
- **Audio Spark Boost:** `0.3` (good music responsiveness)
- **Audio Heat Boost Max:** `60`
- **Cooling Audio Bias:** `-20` (taller flames on loud parts)
- **Bottom Rows for Sparks:** `1`

### Code Simplifications
- All IMU/torch simulation effects **DISABLED**
- Simple random cooling (no turbulence/height factors)
- Basic upward propagation (no complex turbulence)
- Clean spark injection (no clustering/motion effects)

## Result
- Nice visual brightness levels (not too bright/dim)
- Excellent music responsiveness
- Dynamic flame behavior without chaos
- Clean, stable fire effect

## Notes
These settings provide the perfect balance between visual appeal and audio responsiveness. The fire responds well to music dynamics without being overwhelmed by complex physics simulation.