#pragma once

#include "../devices/DeviceConfig.h"
#include "PropagationModel.h"
#include "SpawnRegion.h"
#include "BoundaryBehavior.h"
#include "ForceAdapter.h"
#include "BackgroundModel.h"

// Forward declarations for concrete implementations
class MatrixPropagation;
class LinearPropagation;
class EdgeSpawnRegion;
class RandomSpawnRegion;
class KillBoundary;
class WrapBoundary;
class MatrixForceAdapter;
class LinearForceAdapter;

/**
 * EffectType - What kind of effect is being rendered
 * Used by PhysicsContext to choose appropriate spawn/boundary behavior
 */
enum class EffectType {
    FIRE,       // Rises from source, killed at opposite end
    WATER,      // Falls from source, splashes at opposite end
    LIGHTNING   // Random positions, bounces
};

/**
 * PhysicsContext - Factory for layout-aware physics components
 *
 * Creates appropriate physics models based on device layout type.
 * Generators use this to get layout-appropriate behavior without
 * needing to know the specific layout type.
 *
 * Uses placement new with caller-provided buffers to avoid
 * heap allocation on embedded systems.
 */
class PhysicsContext {
public:
    /**
     * Create propagation model for this layout
     * @param layout The device layout type
     * @param width Grid width
     * @param height Grid height
     * @param wrap Whether to wrap edges (for circular layouts)
     * @param buffer Memory buffer for placement new (minimum 64 bytes)
     * @return Pointer to created model
     */
    static PropagationModel* createPropagation(
        LayoutType layout, uint16_t width, uint16_t height,
        bool wrap, void* buffer);

    /**
     * Create spawn region appropriate for this layout and effect
     * @param layout The device layout type
     * @param effect What kind of effect (fire, water, lightning)
     * @param width Grid width
     * @param height Grid height
     * @param buffer Memory buffer for placement new (minimum 32 bytes)
     * @return Pointer to created region
     */
    static SpawnRegion* createSpawnRegion(
        LayoutType layout, EffectType effect,
        uint16_t width, uint16_t height, void* buffer);

    /**
     * Create boundary behavior for this layout and effect
     * @param layout The device layout type
     * @param effect What kind of effect
     * @param wrap Whether to wrap edges
     * @param buffer Memory buffer for placement new (minimum 32 bytes)
     * @return Pointer to created behavior
     */
    static BoundaryBehavior* createBoundary(
        LayoutType layout, EffectType effect,
        bool wrap, void* buffer);

    /**
     * Create force adapter for this layout
     * @param layout The device layout type
     * @param buffer Memory buffer for placement new (minimum 48 bytes)
     * @return Pointer to created adapter
     */
    static ForceAdapter* createForceAdapter(
        LayoutType layout, void* buffer);

    /**
     * Check if primary axis is vertical for this layout
     * MATRIX: vertical (Y), LINEAR: horizontal (X)
     */
    static bool isPrimaryAxisVertical(LayoutType layout) {
        return layout == MATRIX_LAYOUT;
    }

    /**
     * Check if this layout should wrap edges by default
     */
    static bool shouldWrapByDefault(LayoutType layout) {
        return layout == LINEAR_LAYOUT;  // Hat brim is circular
    }
};
