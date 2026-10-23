#pragma once

#include <DevelopState.h>
#include <LensCorrection.h>
#include <NegativeSettings.h>
#include <Snapshots.h>

#include <QImage>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace arraw {

/// @brief Revision of the thumbnail recipe: the engine, the size and the encoding of a snapshot
/// thumbnail (ADR 056, decision 9).
///
/// Part of every render key. Raise it when a change makes a thumbnail look different for the same
/// settings (the develop engine, the fit, the colour conversion, the encoder), so that the images
/// already stored are made again. The lens database is deliberately not part of the key.
inline constexpr int snapshotThumbnailRevision = 1;

/// @brief Largest quality, then the ones tried in turn when the JPEG is over its byte limit.
inline constexpr int snapshotThumbnailQualities[] = {85, 75, 65, 55, 45, 35, 25, 15, 8};

/// @brief What a snapshot thumbnail is rendered from: a snapshot's identity and full recipe.
///
/// A value, copied out of the snapshot when the job is made, so a render never reads a session or
/// a sidecar and nothing that happens meanwhile can change what it renders.
struct SnapshotRecipe {
    /// @brief The snapshot the result is for.
    SnapshotId id;

    /// @brief Develop state to render.
    DevelopState state;

    /// @brief Negative settings to render with.
    NegativeSettings negative;

    friend bool operator==(const SnapshotRecipe&, const SnapshotRecipe&) = default;
};

/// @brief Copies a snapshot's identity and recipe.
/// @param snapshot Snapshot to copy from.
/// @return The recipe.
[[nodiscard]] SnapshotRecipe recipeOf(const Snapshot& snapshot);

/// @brief Works out the render key of a snapshot's thumbnail.
///
/// A hash of the source's identity (file name and size, neither the directory nor the
/// modification time, so a folder moved or synced to another machine keeps its thumbnails), the
/// develop state, the negative settings and ::arraw::snapshotThumbnailRevision. Neither the lens
/// database nor the snapshot's ID or name is part of it. Hexadecimal text, 64 characters.
/// @param file Photograph the snapshot belongs to.
/// @param state Develop state of the recipe.
/// @param negative Negative settings of the recipe.
/// @return The key, or nothing if the file cannot be looked at.
[[nodiscard]] std::optional<std::string> snapshotThumbnailKey(const std::filesystem::path& file,
                                                              const DevelopState& state,
                                                              const NegativeSettings& negative);

/// @brief Tells whether a snapshot has a thumbnail that is made for its recipe and file as they
/// are.
///
/// False when it has none (a damaged one is dropped by the reader), or one with another render
/// key, or when the file cannot be looked at: all of which want a regeneration.
/// @param file Photograph the snapshot belongs to.
/// @param snapshot Snapshot to check.
/// @return Whether the thumbnail is there and current.
[[nodiscard]] bool hasCurrentThumbnail(const std::filesystem::path& file, const Snapshot& snapshot);

/// @brief Encodes an image as a snapshot thumbnail: JPEG, within the size and byte limits.
///
/// Reduces the image to fit ::arraw::maximumSnapshotThumbnailSide on each side if it is larger
/// (never enlarges it), then tries ::arraw::snapshotThumbnailQualities from the best until the
/// JPEG fits ::arraw::maximumSnapshotThumbnailBytes.
/// @param image Image to encode; an alpha channel is dropped.
/// @param renderKey Key to record with it.
/// @param byteLimit Largest JPEG to accept, at most ::arraw::maximumSnapshotThumbnailBytes; lower
/// only by tests.
/// @return The validated thumbnail.
/// @throws std::invalid_argument if the image is null or the key is invalid.
/// @throws std::runtime_error if no quality fits the byte limit.
[[nodiscard]] SnapshotThumbnail
encodeSnapshotThumbnail(const QImage& image, std::string renderKey,
                        std::size_t byteLimit = maximumSnapshotThumbnailBytes);

/// @brief Renders a snapshot's thumbnail from its recipe.
///
/// Decodes the file at half size, develops it fitted inside
/// ::arraw::maximumSnapshotThumbnailSide on each side and encodes it
/// (::arraw::encodeSnapshotThumbnail). Reads nothing but the image file: not the sidecar, so what
/// the snapshot's settings are now cannot leak in. Uses no GPU. Safe from any thread.
/// @param file Photograph the snapshot belongs to.
/// @param recipe What to render.
/// @param lens Lens corrections to render with, as the caller resolved them; empty for none.
/// @param cancelled Asked between the steps; once true, the render stops. Empty for never.
/// @return The thumbnail, or nothing if it was cancelled.
/// @throws std::runtime_error if the file cannot be read, rendered or encoded within the limits.
/// @throws std::invalid_argument as ::arraw::develop.
[[nodiscard]] std::optional<SnapshotThumbnail>
renderSnapshotThumbnail(const std::filesystem::path& file, const SnapshotRecipe& recipe,
                        const LensCorrection& lens = {},
                        const std::function<bool()>& cancelled = {});

} // namespace arraw
