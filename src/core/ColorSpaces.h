#pragma once

#include <ColorEncoding.h>

/// @brief Fixed transforms between the colour spaces arraw names.
namespace arraw::colorspaces {

/// @brief Linear sRGB (Rec.709 primaries) to linear Rec.2020.
///
/// Both spaces are defined against D65, so this is a change of primaries with
/// no chromatic adaptation: the product of sRGB-to-XYZ and XYZ-to-Rec.2020.
/// Every row sums to one, which is the same statement as white staying white.
inline constexpr Matrix3 srgbToWorking{{0.6274039F, 0.3292830F, 0.0433131F, //
                                        0.0690973F, 0.9195404F, 0.0113623F, //
                                        0.0163914F, 0.0880133F, 0.8955953F}};

} // namespace arraw::colorspaces
