#include "PhysicsContext.h"
#include "MatrixPropagation.h"
#include "LinearPropagation.h"
#include "EdgeSpawnRegion.h"
#include "RandomSpawnRegion.h"
#include "KillBoundary.h"
#include "BounceBoundary.h"
#include "WrapBoundary.h"
#include "MatrixForceAdapter.h"
#include "LinearForceAdapter.h"

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
    LayoutType layout, EffectType effect,
    uint16_t width, uint16_t height, void* buffer) {

    switch (layout) {
        case LINEAR_LAYOUT:
            // Linear layouts use random spawn for all effects
            return new (buffer) RandomSpawnRegion(width, height);

        case MATRIX_LAYOUT:
        case RANDOM_LAYOUT:
        default:
            // Matrix layouts use edge spawning based on effect type
            switch (effect) {
                case EffectType::FIRE:
                    // Fire spawns from bottom, rises up
                    return new (buffer) EdgeSpawnRegion(Edge::BOTTOM, width, height);

                case EffectType::WATER:
                    // Water spawns from top, falls down
                    return new (buffer) EdgeSpawnRegion(Edge::TOP, width, height);

                case EffectType::LIGHTNING:
                default:
                    // Lightning spawns randomly
                    return new (buffer) RandomSpawnRegion(width, height);
            }
    }
}

BoundaryBehavior* PhysicsContext::createBoundary(
    LayoutType layout, EffectType effect,
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
            switch (effect) {
                case EffectType::FIRE:
                case EffectType::WATER:
                    // Fire/water particles die when leaving bounds
                    return new (buffer) KillBoundary();

                case EffectType::LIGHTNING:
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
