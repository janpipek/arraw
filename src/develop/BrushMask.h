#pragma once

#include <QPointF>

#include <cstdint>
#include <span>
#include <vector>

// The painted Brush Mask: a buffer-space R8 raster stamped from freehand strokes
// (docs/adr/0047). Pure stroke maths live here for headless TDD; the GPU only
// samples the finished raster (at vUV) and never paints.

// A single-channel mask raster in corrected-buffer space. `data` is row-major,
// width*height bytes, 0..255 mapping to mask weight 0..1.
struct BrushRaster {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
    bool operator==(const BrushRaster&) const = default;
};

// One stroke's brush settings (docs/adr/0047). `radius`/`center` units are buffer
// pixels; `feather` (0..1) softens the edge; `flow` (0..1) is the per-stroke
// opacity ceiling; `erase` subtracts coverage instead of adding it.
struct BrushDab {
    double radius = 0.0;
    double feather = 0.5;
    double flow = 1.0;
    bool erase = false;
};

// Coverage in [0,1] of one brush dab at distance `dist` (buffer pixels) from its
// centre: 1.0 within the hard core, smoothstep down to 0 at `radius`. `feather`
// (0..1) is the soft-band fraction of the radius — inner core = radius*(1-feather),
// so feather=0 is a hard edge and feather=1 is soft all the way to the centre.
float brushDabProfile(double dist, double radius, double feather);

// Stamp one stroke onto `base` and return the new raster (`base` is not mutated).
// `path` is the polyline of dab centres in buffer pixels; dabs are interpolated
// along it so a dragged stroke leaves no gaps. Within the stroke, dab coverage
// combines by max() capped at `brush.flow`; the stroke then composites onto base —
// Add: min(1, base + coverage); Erase: max(0, base - coverage) (docs/adr/0047).
BrushRaster stampStroke(
    const BrushRaster& base, std::span<const QPointF> path, const BrushDab& brush);
