#pragma once

#include <NegativeSettings.h>

namespace arraw {

struct ImageMetadata;

/// @brief How a decode trades completeness for speed, and which negative it makes.
struct DecodeOptions {
    /// @brief Whether to decode a RAW at half its width and height.
    ///
    /// The RAW decoder then combines each 2x2 block of the sensor into one
    /// pixel instead of demosaicing, which costs a fraction of a full decode
    /// and suits a render that is small anyway (ADR 031). The result is an
    /// ordinary decoded buffer: same encoding, orientation and applied white
    /// balance, with each side `size / 2` (rounded down, as the decoder does),
    /// whatever the file holds (one already demosaiced, a linear DNG, is
    /// halved afterwards, without the speed-up), so ::arraw::develop takes it
    /// unchanged. Its pixel scale is 2 (::arraw::ImageBuffer::pixelScale), so
    /// noise reduction shrinks its reach to match. Its size is then not the one
    /// ::arraw::readImageMetadata declares, so plan against the buffer
    /// (`planFor(buffer, ...)`), not the metadata. Ignored for anything that is
    /// not a RAW: those decode in full, and ::arraw::halved is there to reduce them.
    bool halfSize = false;

    /// @brief Demosaic algorithm of a full-size decode of a mosaiced RAW.
    ///
    /// Ignored for anything else, and for a half-size decode, which does not demosaic.
    Demosaic demosaic = Demosaic::Ahd;

    friend bool operator==(const DecodeOptions&, const DecodeOptions&) = default;
};

/// @brief Gives the form of options that names what a decode did, not what it was asked.
///
/// Replaces what the decode never read by its default: the demosaic for a
/// non-RAW, a half-size decode, or a sensor without a mosaic, and the half-size flag for a
/// non-RAW. Two requests that decode to the same pixels have the same canonical form (ADR 049).
/// @param options Options as asked for.
/// @param metadata What the file declares about itself.
/// @return The canonical options.
[[nodiscard]] DecodeOptions canonical(DecodeOptions options, const ImageMetadata& metadata);

} // namespace arraw
