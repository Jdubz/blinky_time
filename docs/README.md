# 📚 Blinky Time Documentation

Complete documentation for the Blinky Time LED Fire Effect Controller.

## 🗂️ Quick Links

### Setup & Configuration
- [**BUILD_GUIDE.md**](BUILD_GUIDE.md) - Complete build and setup instructions
- [**HARDWARE.md**](HARDWARE.md) - Supported hardware and wiring diagrams
- [**OPTIMAL_FIRE_SETTINGS.md**](OPTIMAL_FIRE_SETTINGS.md) - Fire effect configuration guide

### Architecture & Development
- [**AUDIO_ARCHITECTURE.md**](AUDIO_ARCHITECTURE.md) - AudioController architecture overview
- [**GENERATOR_EFFECT_ARCHITECTURE.md**](GENERATOR_EFFECT_ARCHITECTURE.md) - System architecture overview
- [**DEVELOPMENT.md**](DEVELOPMENT.md) - Development guide (config management, safety procedures)
- [**SAFETY.md**](SAFETY.md) - Safety mechanisms and guidelines
- [**TESTING_SUMMARY.md**](TESTING_SUMMARY.md) - Testing framework and procedures
- [**PLATFORM_FIX.md**](PLATFORM_FIX.md) - Platform bug fix documentation

### Planning & Next Steps
- [**IMPROVEMENT_PLAN.md**](IMPROVEMENT_PLAN.md) - Roadmap and action items
- [**MULTI_HYPOTHESIS_TRACKING_PLAN.md**](MULTI_HYPOTHESIS_TRACKING_PLAN.md) - Multi-hypothesis tempo tracking design
- [**BLUETOOTH_IMPLEMENTATION_PLAN.md**](BLUETOOTH_IMPLEMENTATION_PLAN.md) - Bluetooth/BLE support plan

### Audio Analysis & Testing
- [**AUDIO-TUNING-GUIDE.md**](AUDIO-TUNING-GUIDE.md) - Main testing guide with all tunable parameters
- [**FREQUENCY_DETECTION.md**](FREQUENCY_DETECTION.md) - Frequency-specific percussion detection

## 📋 Documentation Overview

| Document | Purpose | Audience |
|----------|---------|----------|
| [BUILD_GUIDE.md](BUILD_GUIDE.md) | Step-by-step setup instructions | New users |
| [HARDWARE.md](HARDWARE.md) | Hardware specs and wiring | Users & developers |
| [OPTIMAL_FIRE_SETTINGS.md](OPTIMAL_FIRE_SETTINGS.md) | Fire effect tuning guide | Users |
| [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md) | AudioController design and architecture | Developers |
| [GENERATOR_EFFECT_ARCHITECTURE.md](GENERATOR_EFFECT_ARCHITECTURE.md) | System design and architecture | Developers |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Development guide and safety procedures | Developers |
| [SAFETY.md](SAFETY.md) | Safety mechanisms documentation | Developers |
| [TESTING_SUMMARY.md](TESTING_SUMMARY.md) | Test framework overview | Developers |
| [AUDIO-TUNING-GUIDE.md](AUDIO-TUNING-GUIDE.md) | Audio parameter tuning guide | Developers |
| [PLATFORM_FIX.md](PLATFORM_FIX.md) | Platform bug analysis | Developers |
| [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md) | Roadmap and action items | Contributors |
| [MULTI_HYPOTHESIS_TRACKING_PLAN.md](MULTI_HYPOTHESIS_TRACKING_PLAN.md) | Multi-hypothesis tempo tracking | Developers |
| [BLUETOOTH_IMPLEMENTATION_PLAN.md](BLUETOOTH_IMPLEMENTATION_PLAN.md) | Bluetooth/BLE support plan | Developers |

## 🚀 Getting Started

1. **New Users:** Start with [BUILD_GUIDE.md](BUILD_GUIDE.md)
2. **Architecture Overview:** Read [../CLAUDE.md](../CLAUDE.md#system-architecture-overview) for complete system architecture
3. **Developers:** Study [GENERATOR_EFFECT_ARCHITECTURE.md](GENERATOR_EFFECT_ARCHITECTURE.md) and [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md)
4. **Contributors:** Check [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md)
5. **Safety First:** Review [DEVELOPMENT.md](DEVELOPMENT.md) and [SAFETY.md](SAFETY.md) before flashing firmware

## 📝 Contributing to Documentation

**Guidelines:**
- Keep docs actionable and concise
- Include code examples where helpful
- Update the improvement plan with new tasks
- Follow existing formatting conventions

**Documentation Hygiene (per CLAUDE.md):**
- Only create docs with future value (architecture, plans, testing procedures)
- DO NOT create vanity docs (completed work summaries, post-mortems)
- Archive completed analysis docs after extracting actionable items
- Use git history for "what we did" - docs should focus on "what to do"

**Recent Consolidation (January 2026):**
- Moved root docs to docs/ (DEVELOPMENT.md, SAFETY.md, AUDIO_ARCHITECTURE.md)
- Removed completed assessments (AUDIO_IMPROVEMENTS_PLAN, etc.)
- Merged Q&A into tracking plans
- Added comprehensive architecture overview to CLAUDE.md

---

*Last updated: January 3, 2026*
