# Arduino Standard Versioning Implementation

## Overview

This implements **Arduino Library Specification 1.5+** versioning, which is directly analogous to **npm's semantic versioning** in JavaScript. The system follows industry standards used by major Arduino libraries like ESP32, WiFi, and Adafruit libraries.

## Semantic Versioning (Same as JavaScript/npm)

```
MAJOR.MINOR.PATCH
  1.0.1
  │ │ │
  │ │ └── Bug fixes, small improvements (backwards compatible)
  │ └──── New features (backwards compatible)  
  └────── Breaking API changes
```

## Arduino vs JavaScript Versioning Comparison

| Aspect | Arduino (C++) | JavaScript (npm) | Implementation |
|--------|---------------|------------------|----------------|
| **Version Format** | `1.0.1` | `1.0.1` | ✅ Same |
| **Version Number** | `10001` | N/A | ✅ Arduino numeric comparison |
| **Range Checking** | `VERSION_CHECK(1,0,0)` | `semver.gte('1.0.0')` | ✅ Similar API |
| **Prerelease** | `1.0.1-alpha.1` | `1.0.1-alpha.1` | ✅ Supported |
| **Build Metadata** | `1.0.1+20250925` | `1.0.1+20250925` | ✅ Git commit |
| **Library Registry** | `library.properties` | `package.json` | ✅ Implemented |

## File Structure

```
blinky-things/
├── core/Version.h              # Version definitions (like package.json)
├── library.properties          # Arduino library metadata  
└── examples/                   # Example sketches
```

## Version Definition System

### Core Macros (Arduino Standard)
```cpp
#define BLINKY_VERSION_MAJOR 1
#define BLINKY_VERSION_MINOR 0  
#define BLINKY_VERSION_PATCH 1
#define BLINKY_VERSION_NUMBER 10001      // 1*10000 + 0*100 + 1
#define BLINKY_VERSION_STRING "1.0.1"
```

### Version Comparison (like semver.js)
```cpp
// Arduino style - numeric comparison
#define BLINKY_VERSION_CHECK(major, minor, patch) \
    (BLINKY_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

// Usage examples
#if BLINKY_VERSION_CHECK(1, 0, 0)
    // Code for version 1.0.0 and above
#endif

// Runtime checks
if (BLINKY_VERSION_CHECK(1, 1, 0)) {
    Serial.println("Has new features from v1.1.0+");
}
```

### C++ Namespace Functions (like semver API)
```cpp
// Similar to JavaScript: semver.gte('1.0.0')
BlinkyVersion::isAtLeast(1, 0, 0)        // >= 1.0.0
BlinkyVersion::isGreaterThan(1, 0, 0)    // > 1.0.0

// Component access (like semver.major('1.2.3'))  
BlinkyVersion::getMajor()                // Returns 1
BlinkyVersion::getMinor()                // Returns 0
BlinkyVersion::getPatch()                // Returns 1
```

## Serial Console Commands

### Version Information
```
version                         # Full version display
version check 1.0.0            # Test if current >= 1.0.0
version compare 1.1.0          # Compare current to 1.1.0
```

### Sample Output
```
=== VERSION INFORMATION ===
Blinky Time v1.0.1 (staging)
Semantic Version: 1.0.1
Version Number: 10001
Components: 1.0.1

Build: Sep 25 2025 14:23:45
Git Branch: staging  
Git Commit: 89e0c47

Device Type: 2
Device Name: Tube Light
Hardware: XIAO nRF52840 Sense

Version Checks:
  >= 1.0.0: YES
  >= 1.1.0: NO
  >= 2.0.0: NO
```

## Build System Integration

### Automatic Updates
```bash
make version                    # Update from VERSION file
make compile DEVICE=2           # Auto-updates version first
```

### Multi-File Sync
- `VERSION` file → `core/Version.h` → `library.properties` → Git info
- Single source of truth with automatic propagation

## Arduino Library Standards Compliance

### library.properties (Arduino Registry Format)
```ini
name=Blinky Time
version=1.0.1                   # Synced from VERSION file
author=Blinky Time Project Contributors
architectures=nrf52
depends=Adafruit NeoPixel
category=Display
```

### Version Header Standards
- Follows Arduino Library Specification 1.5+
- Compatible with Arduino IDE Library Manager
- Supports PlatformIO library registry
- Standard macros used by major libraries (ESP32, WiFi, etc.)

## Development Workflow Examples

### Feature Development (Minor Version)
```bash
# Implement new feature
echo "1.1.0" > VERSION
make upload DEVICE=2
# Serial: version → Shows "Blinky Time v1.1.0"
```

### Bug Fix (Patch Version)  
```bash
# Fix bug
echo "1.0.2" > VERSION
make upload DEVICE=2
# Serial: version → Shows "Blinky Time v1.0.2"
```

### Breaking Change (Major Version)
```bash
# Breaking API change
echo "2.0.0" > VERSION  
make upload DEVICE=2
# Serial: version → Shows "Blinky Time v2.0.0"
```

### Version-Dependent Code
```cpp
void setup() {
    #if BLINKY_VERSION_CHECK(2, 0, 0)
        // New API available
        initializeNewFeatures();
    #else
        // Legacy compatibility
        initializeLegacyMode();
    #endif
    
    // Runtime version checks
    if (BlinkyVersion::isAtLeast(1, 1, 0)) {
        enableAdvancedFeatures();
    }
}
```

## Benefits vs Manual Versioning

✅ **Automated**: No manual version updates in multiple files  
✅ **Consistent**: Same format across Arduino ecosystem  
✅ **Comparable**: Numeric comparison like major libraries  
✅ **Traceable**: Git integration for exact build identification  
✅ **Standard**: Compatible with Arduino Library Manager  
✅ **Runtime**: Version checks available at runtime  
✅ **Semantic**: Clear meaning for breaking vs non-breaking changes  

This system provides the same level of version management sophistication as npm/JavaScript while following Arduino community standards and best practices.