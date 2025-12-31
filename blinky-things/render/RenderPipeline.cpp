#include "RenderPipeline.h"
#include <new>

RenderPipeline::RenderPipeline()
    : fire_(nullptr), water_(nullptr), lightning_(nullptr),
      currentGenerator_(nullptr), generatorType_(GeneratorType::FIRE),
      noOp_(nullptr), hueRotation_(nullptr),
      currentEffect_(nullptr), effectType_(EffectType::NONE),
      pixelMatrix_(nullptr), renderer_(nullptr),
      initialized_(false), width_(0), height_(0) {
}

RenderPipeline::~RenderPipeline() {
    delete renderer_;
    delete pixelMatrix_;
    delete hueRotation_;
    delete noOp_;
    delete lightning_;
    delete water_;
    delete fire_;
}

bool RenderPipeline::begin(const DeviceConfig& config, ILedStrip& leds, LEDMapper& mapper) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;

    // Create pixel matrix
    pixelMatrix_ = new(std::nothrow) PixelMatrix(width_, height_);
    if (!pixelMatrix_ || !pixelMatrix_->isValid()) {
        return false;
    }

    // Create all generators
    fire_ = new(std::nothrow) Fire();
    if (!fire_ || !fire_->begin(config)) {
        return false;
    }

    water_ = new(std::nothrow) Water();
    if (!water_ || !water_->begin(config)) {
        return false;
    }

    lightning_ = new(std::nothrow) Lightning();
    if (!lightning_ || !lightning_->begin(config)) {
        return false;
    }

    // Create all effects
    noOp_ = new(std::nothrow) NoOpEffect();
    if (!noOp_) {
        return false;
    }
    noOp_->begin(width_, height_);

    hueRotation_ = new(std::nothrow) HueRotationEffect(0.0f, 0.0f);
    if (!hueRotation_) {
        return false;
    }
    hueRotation_->begin(width_, height_);

    // Create renderer
    renderer_ = new(std::nothrow) EffectRenderer(leds, mapper);
    if (!renderer_) {
        return false;
    }

    // Set defaults: Fire generator, no effect
    currentGenerator_ = fire_;
    generatorType_ = GeneratorType::FIRE;
    currentEffect_ = noOp_;
    effectType_ = EffectType::NONE;

    initialized_ = true;
    return true;
}

void RenderPipeline::render(const AudioControl& audio) {
    if (!initialized_ || !currentGenerator_ || !pixelMatrix_ || !renderer_) {
        return;
    }

    // Generate -> Effect -> Render
    currentGenerator_->generate(*pixelMatrix_, audio);

    if (currentEffect_) {
        currentEffect_->apply(pixelMatrix_);
    }

    renderer_->render(*pixelMatrix_);
}

bool RenderPipeline::setGenerator(GeneratorType type) {
    if (!initialized_) return false;

    Generator* newGen = nullptr;
    switch (type) {
        case GeneratorType::FIRE:
            newGen = fire_;
            break;
        case GeneratorType::WATER:
            newGen = water_;
            break;
        case GeneratorType::LIGHTNING:
            newGen = lightning_;
            break;
        default:
            return false;
    }

    if (newGen) {
        // Reset new generator to clean state
        newGen->reset();
        currentGenerator_ = newGen;
        generatorType_ = type;
        return true;
    }
    return false;
}

GeneratorType RenderPipeline::getGeneratorType() const {
    return generatorType_;
}

const char* RenderPipeline::getGeneratorName() const {
    if (currentGenerator_) {
        return currentGenerator_->getName();
    }
    return "None";
}

bool RenderPipeline::setEffect(EffectType type) {
    if (!initialized_) return false;

    switch (type) {
        case EffectType::NONE:
            currentEffect_ = noOp_;
            effectType_ = EffectType::NONE;
            return true;

        case EffectType::HUE_ROTATION:
            currentEffect_ = hueRotation_;
            effectType_ = EffectType::HUE_ROTATION;
            return true;

        default:
            return false;
    }
}

EffectType RenderPipeline::getEffectType() const {
    return effectType_;
}

const char* RenderPipeline::getEffectName() const {
    switch (effectType_) {
        case EffectType::NONE:
            return "None";
        case EffectType::HUE_ROTATION:
            return "HueRotation";
        default:
            return "Unknown";
    }
}

// Parameter access - returns mutable pointers to generator params
FireParams* RenderPipeline::getFireParams() {
    return fire_ ? &fire_->getParamsMutable() : nullptr;
}

WaterParams* RenderPipeline::getWaterParams() {
    return water_ ? &water_->getParamsMutable() : nullptr;
}

LightningParams* RenderPipeline::getLightningParams() {
    return lightning_ ? &lightning_->getParamsMutable() : nullptr;
}

// Apply methods - params are modified in-place, so these are no-ops
// Kept for API consistency if future implementations need explicit apply
void RenderPipeline::applyFireParams() {
    // Parameters modified via getFireParams() take effect immediately
}

void RenderPipeline::applyWaterParams() {
    // Parameters modified via getWaterParams() take effect immediately
}

void RenderPipeline::applyLightningParams() {
    // Parameters modified via getLightningParams() take effect immediately
}

// Static helpers for listing options
const char* RenderPipeline::getGeneratorNameByIndex(int index) {
    switch (index) {
        case 0: return "fire";
        case 1: return "water";
        case 2: return "lightning";
        default: return nullptr;
    }
}

const char* RenderPipeline::getEffectNameByIndex(int index) {
    switch (index) {
        case 0: return "none";
        case 1: return "hue";
        default: return nullptr;
    }
}

GeneratorType RenderPipeline::getGeneratorTypeByIndex(int index) {
    switch (index) {
        case 0: return GeneratorType::FIRE;
        case 1: return GeneratorType::WATER;
        case 2: return GeneratorType::LIGHTNING;
        default: return GeneratorType::FIRE;
    }
}

EffectType RenderPipeline::getEffectTypeByIndex(int index) {
    switch (index) {
        case 0: return EffectType::NONE;
        case 1: return EffectType::HUE_ROTATION;
        default: return EffectType::NONE;
    }
}
