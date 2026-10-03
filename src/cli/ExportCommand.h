#pragma once

#include "Cli.h"
#include "SettingCodec.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <GeometrySettings.h>
#include <SettingDescriptors.h>

#include <QtCore/qcontainerfwd.h>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace arraw::cli {

/// @brief Splits a clockwise angle into quarter-turn and straighten settings.
/// @param geometry Settings whose rotation and straighten values are replaced.
/// @param degrees Finite clockwise angle relative to camera orientation, before flips.
/// @throws std::invalid_argument if the angle is not finite.
void setRotationAngle(GeometrySettings& geometry, double degrees);

/// @brief Reads the value of `--resize`, whose three forms are told apart by shape (ADR 007).
///
/// `2048` is the long edge, `2048x1365` a box to fit inside, and `50%` a scale,
/// which may be fractional (`12.5%`). A bare number is the long edge for good.
/// Sides are whole numbers of at least 1 that fit 32 bits; a percentage is a
/// plain decimal greater than 0.
/// @param spec The text after `--resize`.
/// @return A box (a long edge of N is N by N) or a scale factor (the percentage over 100).
/// @throws std::invalid_argument with a message for the person at the keyboard if @p spec
/// matches none of the forms.
[[nodiscard]] std::variant<RenderRequest::FitInside, RenderRequest::Scale>
parseResize(std::string_view spec);

/// @brief Tell whether a row is a ranged float setting, which export offers as an option.
///
/// Such a row needs help wording in the export command; a test holds the table to it.
/// @param descriptor Row of ::arraw::developSettingDescriptors.
[[nodiscard]] bool isRangedFloatSetting(const FieldDescriptor& descriptor);

/// @brief One setting the flags named, as the codec understands it.
struct SettingEdit {
    /// @brief Row of ::arraw::developSettingDescriptors naming the field.
    const FieldDescriptor* descriptor;

    /// @brief Value to give it.
    Encoded value;
};

/// @brief What `--crop` said: a rectangle, or automatic framing when empty.
struct CropEdit {
    /// @brief Rectangle asked for, or automatic framing when absent.
    std::optional<UprightCropRect> rectangle;
};

/// @brief The geometry flags the command line was given.
struct GeometryEdits {
    /// @brief `--rotate`: clockwise degrees, replacing the quarter-turn and the straighten.
    std::optional<double> rotate;

    /// @brief `--flip-horizontal` (true) or `--no-flip-horizontal` (false).
    std::optional<bool> flipHorizontal;

    /// @brief `--flip-vertical` (true) or `--no-flip-vertical` (false).
    std::optional<bool> flipVertical;

    /// @brief `--crop`.
    std::optional<CropEdit> crop;

    /// @brief `--crop-aspect`.
    std::optional<CropAspect> aspect;
};

/// @brief What the flags of an export say to change in each photograph's settings.
///
/// A partial edit rather than a settings value, so that whatever a flag does not
/// name stays as the photograph's sidecar has it (ADR 006).
struct ExportEdits {
    /// @brief Ordered edits of the settings the table describes.
    std::vector<SettingEdit> settings;

    /// @brief Edits of the geometry, which may send an explicit crop back to automatic framing.
    GeometryEdits geometry;
};

/// @brief Reads the develop flags of an export command line.
/// @param flags The command's arguments after its name; only develop flags are used.
/// @param err Where a usage problem is reported.
/// @return The edits, or `std::nullopt` after reporting the problem.
[[nodiscard]] std::optional<ExportEdits> readExportEdits(const std::vector<std::string>& flags,
                                                         std::ostream& err);

/// @brief Puts the flags' edits on top of a photograph's own settings.
/// @param base Settings the photograph came with.
/// @param edits What the flags said.
/// @param log Where the codec's warnings go; a value the flags gave is already in range.
/// @param subject Photograph the settings are for.
/// @param raw Whether the photograph is a RAW, which decides the settings a render does not read.
/// @return @p base with the edits applied.
[[nodiscard]] DevelopSettings applyEdits(DevelopSettings base, const ExportEdits& edits,
                                         DiagnosticLog& log, const std::filesystem::path& subject,
                                         bool raw = true);

/// @brief Renders images and writes them out.
///
/// `arraw-cli export <input>... -o <dir> [options]`. Inputs are files rather
/// than directories, every input is attempted so one bad frame cannot abandon a
/// batch, and an existing output is refused unless replacing it was asked for.
/// See ADR 006.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out Help, when it was asked for; nothing else.
/// @param err Progress, warnings, and errors.
/// @param start Starts the Qt application, a `QCoreApplication`, once the
/// arguments are good.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if any input could not
/// be exported, or ::arraw::cli::UsageError if the arguments were wrong.
[[nodiscard]] int runExportCommand(const QStringList& arguments, std::ostream& out,
                                   std::ostream& err, const StartApplication& start);

} // namespace arraw::cli
