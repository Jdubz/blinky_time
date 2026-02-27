#pragma once

#include "../devices/DeviceConfig.h"
#include "../generators/Generator.h"  // For GeneratorType enum
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
class MatrixBackground;
class LinearBackground;

// Forward declare BackgroundStyle (defined in MatrixBackground.h)
enum class BackgroundStyle;

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
     * Create spawn region appropriate for this layout and generator
     * @param layout The device layout type
     * @param generator What kind of generator (fire, water, lightning)
     * @param width Grid width
     * @param height Grid height
     * @param buffer Memory buffer for placement new (minimum 32 bytes)
     * @return Pointer to created region
     */
    static SpawnRegion* createSpawnRegion(
        LayoutType layout, GeneratorType generator,
        uint16_t width, uint16_t height, void* buffer);

    /**
     * Create boundary behavior for this layout and generator
     * @param layout The device layout type
     * @param generator What kind of generator
     * @param wrap Whether to wrap edges
     * @param buffer Memory buffer for placement new (minimum 32 bytes)
     * @return Pointer to created behavior
     */
    static BoundaryBehavior* createBoundary(
        LayoutType layout, GeneratorType generator,
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
     * Create background model for this layout and effect style
     * @param layout The device layout type
     * @param style The background style (FIRE, WATER, LIGHTNING)
     * @param buffer Memory buffer for placement new (minimum 64 bytes)
     * @return Pointer to created background model
     */
    static BackgroundModel* createBackground(
        LayoutType layout, BackgroundStyle style, void* buffer);

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
