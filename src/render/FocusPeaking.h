#pragma once
#include <array>

// Discrete sensitivity levels for the Focus Peaking overlay (docs/adr/0058):
// no universal "sharp" threshold exists, unlike Clipping's physical 1.0/0.0
// boundary, so the user picks how aggressively edges are flagged.
enum class FocusPeakingSensitivity { Low, Mid, High };

// Sobel gradient-magnitude threshold per level, in the same [0, ~1.4] units
// shaders/peaking_edge.frag computes (docs/adr/0058): LOWER threshold means
// MORE edges are flagged (more sensitive). One shared table — not hand-copied
// into the View submenu and the shader path — so retuning a level, or adding
// one, is a one-line change (docs/adr/0058's "leave room for further change").
// Starting values, tune after visual testing on real photos.
constexpr std::array<float, 3> kFocusPeakingThresholds = {0.35f, 0.22f, 0.10f};
