#pragma once
#include <vector>

// Linear float32 RGB image buffer, interleaved, [0..1] nominal. The working
// pixel container used everywhere from decode to render (docs/adr/0001).
struct ImageBuffer {
    std::vector<float> data;
    int width = 0;
    int height = 0;

    bool valid() const { return !data.empty() && width > 0 && height > 0; }
};
