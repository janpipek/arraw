#pragma once

#include <Diagnostics.h>
#include <ImageExport.h>

#include <QByteArray>

namespace arraw {

/// @brief What an encoded export says about itself, which the source's metadata must not
/// contradict.
struct EncodedFacts {
    /// @brief Pixel width of the encoded image.
    int width = 0;

    /// @brief Pixel height of the encoded image.
    int height = 0;

    /// @brief Whether the output encoding is sRGB, which EXIF's ColorSpace can name.
    bool srgb = true;
};

/// @brief Tells whether a selection carries anything at all.
/// @param selection Groups chosen.
/// @return True if at least one group is on.
[[nodiscard]] bool carriesAnything(const MetadataSelection& selection) noexcept;

/// @brief Adds the chosen metadata to an encoded JPEG, PNG or TIFF.
///
/// Works on the bytes in memory, so a failure leaves nothing on disk. The ICC
/// profile and pixels already in the file are untouched. Which tags each group
/// copies is ADR 032's.
/// @param encoded The complete encoded file.
/// @param metadata Source, marks and groups.
/// @param facts Dimensions and colour space of the encoded image.
/// @return The file with metadata, or a null array when there was nothing to carry,
/// in which case @p encoded stands.
/// @throws std::invalid_argument if the rating is outside -1 to 5.
/// @param log Receives a ::arraw::Notice::MetadataNotCarried warning for each thing left
/// out because the source, its sidecar or a tag could not be read or copied; what can be
/// carried still is.
/// @throws std::runtime_error naming the cause if the metadata cannot be written into
/// @p encoded.
[[nodiscard]] QByteArray embedMetadata(const QByteArray& encoded, const ExportMetadata& metadata,
                                       const EncodedFacts& facts, DiagnosticLog& log);

} // namespace arraw
