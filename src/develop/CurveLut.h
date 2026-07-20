#pragma once
#include <array>
#include <vector>
#include <QPointF>

struct GlobalAdjustment;

// Tone-curve LUT from control points in [0,1]² — model→GPU numbers, so it
// lives in develop/, not pipeline/ (docs/adr/0041). Moved from ImagePipeline
// so render-side consumers need no pipeline dependency (docs/adr/0049).
std::array<float, 256> computeCurveLUT(const std::vector<QPointF>& pts);

// The packed 256×RGBA (Luma, R, G, B) texture RendererCore::setCurveLut
// expects, built from the four curves in `params`.
std::array<float, 256 * 4> curveLutRgba(const GlobalAdjustment& params);
