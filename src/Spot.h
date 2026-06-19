#pragma once

#include <QPointF>

#include <vector>

struct ImageBuffer; // defined in ImagePipeline.h

struct Spot {
    QPointF destination; // buffer pixel coordinates
    QPointF source;      // buffer pixel coordinates
    double radius = 0.0; // pixels in original buffer
    double feather = 0.5; // 0..1 falloff band: inner = radius*(1-feather), outer = radius
    bool operator==(const Spot&) const = default;
};

// Weight for a pixel at `px` (buffer pixel coordinates): 1.0 inside the inner
// radius, 0.0 outside the outer radius, smoothstep blend in between.
float spotWeight(const Spot& spot, QPointF px);

// Apply all spots to a copy of `buf` and return it. `buf` is not mutated.
// For each pixel in each destination circle, blends the source-offset pixel
// with the original by the spot weight.
ImageBuffer applySpots(const ImageBuffer& buf, const std::vector<Spot>& spots);

// Auto-place a source circle for a newly placed spot at `destination`.
// Offsets right by `offset` buffer pixels, clamped inside the buffer.
QPointF autoSourcePosition(QPointF destination, double offset, int bufW, int bufH);
