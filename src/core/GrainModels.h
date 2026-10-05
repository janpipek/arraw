#pragma once

#include <EffectsSettings.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace arraw {

struct FrameMapping;

/// @brief Seed a GrainSettings::seed of zero renders with: one fixed pattern for documents without
/// one.
inline constexpr std::uint32_t unseededGrainSeed = 0x2545f491U;

/// @brief Strongest grain, as the deviation it adds in the perceptual coordinate (ADR 010).
///
/// `main`'s 0.08 of the encoded range at an amount of a hundred (its ADR 0026).
inline constexpr float strongestGrainDeviation = 0.08F;

/// @brief Finest and coarsest grain, as a fraction of the cropped frame's long edge.
///
/// `main`'s half a pixel to four pixels of a 2048-pixel edge; Size spreads
/// between them on a logarithmic scale.
inline constexpr double finestGrain = 0.5 / 2048.0;

/// @copydoc finestGrain
inline constexpr double coarsestGrain = 4.0 / 2048.0;

/// @brief Cell sizes, in output pixels, over which a scale of grain fades in.
///
/// A lattice whose cells are a pixel or less is left out: its values are
/// spaced closer than the pixels that would sample them, so it could only
/// alias into another pattern. One whose cells are two pixels or more is
/// drawn in full. In between it fades by a smoothstep; there, too, every
/// pixel samples its cell at nearly the same place, so the grain's contrast
/// would beat with the pixel grid at full weight. What the fade takes away
/// is not lost: the part of it a box filter of the output pixel would keep
/// goes into a substitute lattice of ::arraw::grainSubstituteCell pixels a
/// cell. This is what lets a preview soften grain without moving what it
/// can show (ADR 038); the substitute itself differs between output scales.
inline constexpr double grainFadeStart = 1.0;

/// @copydoc grainFadeStart
inline constexpr double grainFadeEnd = 2.0;

/// @brief Size, in output pixels, of the cells of the lattice that stands in for grain too fine
/// to draw.
///
/// Two pixels, the finest a lattice can be drawn at without aliasing. Its
/// cells sit on the crop frame, so they stay put as a render pans, but they
/// are another pattern at every output scale: they stand in for detail no
/// render at that scale can show.
inline constexpr double grainSubstituteCell = 2.0;

/// @brief The grain's part of the effects block, resolved: what any model honours.
///
/// An amount of zero resolves to the default, off, whatever the other
/// settings, so that they neither change pixels nor the plan.
struct GrainPlan {
    /// @brief Whether the grain changes anything.
    bool active = false;

    /// @brief Algorithm that draws it.
    GrainModel model = GrainModel::ValueNoise;

    /// @brief Standard deviation of the grain in the perceptual coordinate, at full detail.
    float deviation = 0.0F;

    /// @brief Size of one grain as a fraction of the cropped frame's long edge.
    float size = 0.0F;

    /// @brief How clumped it is, zero (one scale) to one (clustered).
    float roughness = 0.0F;

    /// @brief Seed of the pattern; never zero (see ::arraw::unseededGrainSeed).
    std::uint32_t seed = 0;

    friend bool operator==(const GrainPlan&, const GrainPlan&) = default;
};

/// @brief Most lattices a grain model may lay over a render.
inline constexpr std::size_t grainLayerCount = 4;

/// @brief Lattices of the value-noise model's own scales, finest first; the next layer is its
/// pixel-scale substitute.
inline constexpr std::size_t valueNoiseLatticeCount = 3;

/// @brief Index of the value-noise model's pixel-scale substitute in GrainPlacement::layers.
inline constexpr std::size_t valueNoiseSubstituteLayer = valueNoiseLatticeCount;

/// @brief One lattice of a grain model, placed on a render's output pixels.
///
/// Output pixel `(x, y)` lies at lattice position `cell + fraction + (x, y) * delta`,
/// with the integer cell kept apart from the float remainder so that the
/// float part stays small, and exact enough, at any zoom: the remainder is at
/// most the lattice cells one render spans. Both backends evaluate
/// `fraction + x * delta` in float with the same operations, so they find the
/// same cells and the same lattice values bit for bit.
struct GrainLayer {
    /// @brief Lattice cell holding output pixel (0, 0)'s centre.
    std::array<std::int32_t, 2> cell{};

    /// @brief Where in that cell the centre lies, from zero to one.
    std::array<float, 2> fraction{};

    /// @brief Lattice cells one output pixel spans along each axis.
    std::array<float, 2> delta{};

    /// @brief What the layer's unit noise is multiplied by: its share of the deviation, faded by
    /// its band-limit; zero leaves the layer out.
    float weight = 0.0F;

    /// @brief Seed of the layer's lattice values.
    std::uint32_t seed = 0;

    friend bool operator==(const GrainLayer&, const GrainLayer&) = default;
};

/// @brief A grain plan placed on one render's output pixels.
///
/// Worked out once per render from the plan and the frame mapping, in double,
/// by the model; what the model's per-pixel function and its shader read.
struct GrainPlacement {
    /// @brief The model's lattices; unused ones have a weight of zero.
    std::array<GrainLayer, grainLayerCount> layers{};

    friend bool operator==(const GrainPlacement&, const GrainPlacement&) = default;
};

/// @brief Resolves the grain settings into the plan's block.
/// @param settings Grain settings; out-of-range values are clamped (ADR 008).
/// @return The block; off when the amount is zero.
/// @throws std::invalid_argument if a value is not finite or the model is not one of
/// ::arraw::GrainModel.
[[nodiscard]] GrainPlan grainPlanFor(const GrainSettings& settings);

/// @brief Places a grain plan on a render, through its model.
/// @param plan Resolved grain, active.
/// @param mapping Where the render's pixels lie in the crop frame.
/// @return The placement the model's per-pixel function reads.
[[nodiscard]] GrainPlacement grainPlacementOf(const GrainPlan& plan, const FrameMapping& mapping);

/// @brief Gives the grain at an output pixel, through the plan's model.
///
/// The Effects pass calls this and nothing model-specific.
/// @param plan Resolved grain, active.
/// @param placement Its placement on the render.
/// @param column Output column.
/// @param row Output row.
/// @return What to add in the perceptual coordinate: zero mean, with the plan's deviation.
[[nodiscard]] float grainAt(const GrainPlan& plan, const GrainPlacement& placement,
                            std::uint32_t column, std::uint32_t row);

/// @brief Hashes a lattice cell and a seed into 32 random bits.
///
/// `main`'s grain hash, kept so that its grain carries over. Integer arithmetic
/// modulo 2^32, the same on both backends.
/// @param x Cell column, its bits as unsigned.
/// @param y Cell row, its bits as unsigned.
/// @param seed Lattice seed.
/// @return The bits.
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] constexpr std::uint32_t grainHash(std::uint32_t x, std::uint32_t y,
                                                std::uint32_t seed) noexcept {
    std::uint32_t h = (x * 0x8da6b343U) ^ (y * 0xd8163841U) ^ (seed * 0xcb1ab31fU);
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    h *= 0x846ca68bU;
    return h ^ (h >> 16U);
}

/// @brief Places the value-noise model's lattices on a render.
///
/// Up to three lattices, at `1`, `1 / 0.53` and `1 / 0.23` grains per cell as
/// `main` has them, each with its own seed. Roughness mixes from the finest
/// alone to `0.6 : 0.3 : 0.1` of the three, and the mix is scaled to the
/// plan's deviation. Each lattice is then faded by the size of its cells in
/// output pixels (::arraw::grainFadeStart), which leaves out what the render
/// is too coarse to show. The variance a box filter of the output pixel would
/// keep of what the fade left out goes into a fourth lattice, the substitute,
/// of ::arraw::grainSubstituteCell pixels a cell anchored to the crop frame:
/// so a render that leaves out fine grain is as grainy, statistically, as a
/// larger render downscaled to its size.
/// @param plan Resolved grain.
/// @param mapping Where the render's pixels lie in the crop frame.
/// @return The placement.
[[nodiscard]] GrainPlacement valueNoisePlacement(const GrainPlan& plan,
                                                 const FrameMapping& mapping);

/// @brief Draws the value-noise model at an output pixel.
///
/// Each lattice holds a value from -0.5 to 0.5 per cell, from ::arraw::grainHash,
/// interpolated with smoothstep weights; their weighted sum is the grain.
/// @param placement From ::arraw::valueNoisePlacement.
/// @param column Output column.
/// @param row Output row.
/// @return The grain to add in the perceptual coordinate.
///
/// Mirrored by `src/gpu/shaders/effects.frag`, which must change with it.
[[nodiscard]] float valueNoiseGrain(const GrainPlacement& placement, std::uint32_t column,
                                    std::uint32_t row);

} // namespace arraw
