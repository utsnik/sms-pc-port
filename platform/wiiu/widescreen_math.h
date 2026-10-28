#pragma once

#include <cstdint>

namespace gx {

struct TargetScissor {
    uint32_t left, top, width, height;
};

// Frame-start clears must not inherit a copy-clear rectangle from the prior
// widescreen frame. Keep this calculation host-testable and use the actual
// render-target dimensions, including reduced scene targets.
static inline TargetScissor fullTargetScissor(uint32_t width, uint32_t height) {
    return {0, 0, width, height};
}

}  // namespace gx
