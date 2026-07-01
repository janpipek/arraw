#include "MainWindowZoom.h"

#include <cmath>

namespace {
constexpr float kZoomPresetEpsilon = 0.005f;
}

int matchingZoomPresetIndex(float pixelZoom) {
    for (size_t i = 0; i < kZoomPresets.size(); ++i)
        if (std::abs(pixelZoom - kZoomPresets[i]) < kZoomPresetEpsilon)
            return int(i);
    return -1;
}
