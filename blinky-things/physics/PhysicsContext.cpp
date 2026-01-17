#include "PhysicsContext.h"
#include "MatrixPropagation.h"
#include "LinearPropagation.h"
#include "EdgeSpawnRegion.h"
#include "RandomSpawnRegion.h"
#include "CenterSpawnRegion.h"
#include "KillBoundary.h"
#include "BounceBoundary.h"
#include "WrapBoundary.h"
#include "MatrixForceAdapter.h"
#include "LinearForceAdapter.h"
#include "MatrixBackground.h"
#include "LinearBackground.h"

// Compile-time buffer size validation
// These assertions ensure physics components fit in their allocated buffers.
// Buffer sizes are defined in ParticleGenerator.h and generator headers.

// Propagation models: propagationBuffer_[64]
static_assert(sizeof(MatrixPropagation) <= 64, "MatrixPropagation exceeds 64-byte buffer");
static_assert(sizeof(LinearPropagation) <= 64, "LinearPropagation exceeds 64-byte buffer");

// Spawn regions: spawnBuffer_[32]
static_assert(sizeof(EdgeSpawnRegion) <= 32, "EdgeSpawnRegion exceeds 32-byte buffer");
static_assert(sizeof(RandomSpawnRegion) <= 32, "RandomSpawnRegion exceeds 32-byte buffer");
static_assert(sizeof(CenterSpawnRegion) <= 32, "CenterSpawnRegion exceeds 32-byte buffer");

// Boundary behaviors: boundaryBuffer_[32]
static_assert(sizeof(KillBoundary) <= 32, "KillBoundary exceeds 32-byte buffer");
static_assert(sizeof(BounceBoundary) <= 32, "BounceBoundary exceeds 32-byte buffer");
static_assert(sizeof(WrapBoundary) <= 32, "WrapBoundary exceeds 32-byte buffer");

// Force adapters: forceBuffer_[48]
static_assert(sizeof(MatrixForceAdapter) <= 48, "MatrixForceAdapter exceeds 48-byte buffer");
static_assert(sizeof(LinearForceAdapter) <= 48, "LinearForceAdapter exceeds 48-byte buffer");

// Background models: backgroundBuffer_[64]
static_assert(sizeof(MatrixBackground) <= 64, "MatrixBackground exceeds 64-byte buffer");
static_assert(sizeof(LinearBackground) <= 64, "LinearBackground exceeds 64-byte buffer");

PropagationModel* PhysicsContext::createPropagation(
    LayoutType layout, uint16_t width, uint16_t height,
    bool wrap, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            return new (buffer) LinearPropagation(wrap);

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            return new (buffer) MatrixPropagation();
    }
}

SpawnRegion* PhysicsContext::createSpawnRegion(
    LayoutType layout, GeneratorType generator,
    uint16_t width, uint16_t height, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            // Linear layouts use random spawn for all effects
            return new (buffer) RandomSpawnRegion(width, height);

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            // Matrix layouts use edge spawning based on effect type
            switch (generator) {
                case GeneratorType::FIRE:
                    // Fire spawns from bottom, rises up
                    return new (buffer) EdgeSpawnRegion(Edge::BOTTOM, width, height);

                case GeneratorType::WATER:
                    // Water spawns from top, falls down
                    return new (buffer) EdgeSpawnRegion(Edge::TOP, width, height);

                case GeneratorType::LIGHTNING:
                default:
                    // Lightning spawns randomly
                    return new (buffer) RandomSpawnRegion(width, height);
            }
    }
}

BoundaryBehavior* PhysicsContext::createBoundary(
    LayoutType layout, GeneratorType generator,
    bool wrap, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            if (wrap) {
                // Circular arrangement - wrap around
                return new (buffer) WrapBoundary(true, false);
            } else {
                // Non-circular - bounce at ends
                return new (buffer) BounceBoundary(0.8f);
            }

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            // Matrix behavior depends on effect type
            switch (generator) {
                case GeneratorType::FIRE:
                case GeneratorType::WATER:
                    // Fire/water particles die when leaving bounds
                    return new (buffer) KillBoundary();

                case GeneratorType::LIGHTNING:
                default:
                    // Lightning bolts bounce
                    return new (buffer) BounceBoundary(0.8f);
            }
    }
}

ForceAdapter* PhysicsContext::createForceAdapter(
    LayoutType layout, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            return new (buffer) LinearForceAdapter();

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            return new (buffer) MatrixForceAdapter();
    }
}

BackgroundModel* PhysicsContext::createBackground(
    LayoutType layout, BackgroundStyle style, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            return new (buffer) LinearBackground(style);

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            return new (buffer) MatrixBackground(style);
    }
}
