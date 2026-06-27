#pragma once

#include "DevelopGroup.h"
#include "FieldSpec.h"
#include "ImagePipeline.h"

#include <optional>
#include <vector>

#include <QString>

// A Develop Parameter is a single addressable develop control — the finest unit
// of develop state, one notch below a DevelopGroup. Every parameter belongs to
// exactly one group, and the groups partition the parameters. This is the single
// source of truth for naming an individual control ([[spot-for-algorithms]]),
// shared by the History panel (labelling each edit step), and reusable by future
// granular presets. Per-image state (Local Adjustments, Spots, the grain seed)
// has its own undo commands and is deliberately not enumerated here.
enum class DevelopParameter {
    // White Balance
    Temperature,
    Tint,
    // Tone
    Exposure,
    Contrast,
    Highlights,
    Shadows,
    Whites,
    Blacks,
    // Colour
    Saturation,
    Vibrance,
    // Tone Curve (one per channel)
    CurveLuma,
    CurveRed,
    CurveGreen,
    CurveBlue,
    // HSL — 8 bands × 3 components, band-major. Keep this block contiguous: the
    // band/component is derived from the offset from HslRedHue (see the .cpp).
    HslRedHue,
    HslRedSaturation,
    HslRedLuminance,
    HslOrangeHue,
    HslOrangeSaturation,
    HslOrangeLuminance,
    HslYellowHue,
    HslYellowSaturation,
    HslYellowLuminance,
    HslGreenHue,
    HslGreenSaturation,
    HslGreenLuminance,
    HslAquaHue,
    HslAquaSaturation,
    HslAquaLuminance,
    HslBlueHue,
    HslBlueSaturation,
    HslBlueLuminance,
    HslPurpleHue,
    HslPurpleSaturation,
    HslPurpleLuminance,
    HslMagentaHue,
    HslMagentaSaturation,
    HslMagentaLuminance,
    // Detail
    Demosaic,
    Sharpening,
    ColorNoiseReduction,
    ColorNoiseReductionSmoothness,
    // Lens Corrections
    LensDistortion,
    LensVignetting,
    LensChromaticAberration,
    // Geometry
    Orientation,
    Straighten,
    Crop,
    // Effects
    PostCropVignetteAmount,
    PostCropVignetteMidpoint,
    PostCropVignetteFeather,
    GrainAmount,
    GrainSize,
    GrainRoughness,
    Count_, // sentinel — keep last
};

inline constexpr int kDevelopParameterCount = static_cast<int>(DevelopParameter::Count_);

// Stable machine key for a parameter — never localised, never reordered (safe as
// a serialisation key for granular presets, mirroring developGroupKey).
const char* developParameterKey(DevelopParameter p);

// Human-readable label, shown in the History panel. Localisable.
QString developParameterLabel(DevelopParameter p);

// The group this parameter belongs to. The parameter→group map and applyGroups'
// group→field map are kept consistent by a partition test (test_DevelopParameter).
DevelopGroup developParameterGroup(DevelopParameter p);

// The slider number-handling spec (range, scales, format) for a scalar
// parameter, or nullopt for parameters with no single numeric value (curves,
// crop, orientation, the boolean lens toggles). This is the single source of
// truth for slider numerics, also consumed by AdjustmentPanel's sliders.
std::optional<FieldSpec> developParameterSpec(DevelopParameter p);

// The current stored value of a scalar parameter (0 for non-scalar parameters).
float developParameterValue(DevelopParameter p, const GlobalAdjustment& state);

// A short human value for the parameter in this state, formatted exactly as the
// panel shows it ("+1.00 EV", "+9.0°", "6200 K", "on"); empty for parameters with
// no single value (a curve, crop, or orientation edit).
QString developParameterValueText(DevelopParameter p, const GlobalAdjustment& state);

// Does this single parameter's backing field differ between two develop states?
bool developParameterDiffers(
    DevelopParameter p, const GlobalAdjustment& before, const GlobalAdjustment& after);

// Every parameter whose field changed between two states, in enum order.
std::vector<DevelopParameter> changedParameters(
    const GlobalAdjustment& before, const GlobalAdjustment& after);

// One human label for a develop edit, derived from what actually changed:
//   • exactly one parameter  → that parameter's label ("Exposure", "Blue Hue")
//   • several in one group    → the group's label ("White Balance")
//   • spanning groups / none  → a generic fallback ("Adjust")
// This is what an undo command shows in the History list.
QString developChangeLabel(const GlobalAdjustment& before, const GlobalAdjustment& after);
