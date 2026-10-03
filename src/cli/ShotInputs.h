#pragma once

#include <Diagnostics.h>
#include <MarksFilter.h>
#include <Shot.h>

#include <QtCore/qcontainerfwd.h>

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <vector>

class QCommandLineParser;

namespace arraw::cli {

/// @brief One photograph a command works on.
struct ShotInput {
    /// @brief File to work on: the file as given, or the primary of a shot found in a folder.
    std::filesystem::path path;

    /// @brief The shot it is the primary of, when it came out of a folder.
    std::optional<Shot> shot;
};

/// @brief What the inputs of a command line come to.
struct ExpandedInputs {
    /// @brief The photographs, in the order given, a folder's shots in natural order.
    std::vector<ShotInput> photographs;

    /// @brief Folders that could not be read; each is reported and counts as a failed input.
    std::size_t unreadableFolders = 0;
};

/// @brief Replaces each folder among the inputs by the primary files of its shots (ADR 029).
///
/// A file is kept as given, with no shot: it behaves as it always did. A folder
/// that cannot be read is reported as ::arraw::Notice::InputFailed, and one with
/// no photographs says so with ::arraw::Notice::NoPhotographs; neither stops the rest.
/// @param inputs Files and folders, as the command line gave them.
/// @param log Where the folders' problems go.
/// @return The photographs to work on.
[[nodiscard]] ExpandedInputs expandInputs(const std::vector<std::filesystem::path>& inputs,
                                          DiagnosticLog& log);

/// @brief Declares `--min-rating`, `--rejected` and `--label` on a command's parser.
/// @param parser Parser of the command.
void addMarksFilterOptions(QCommandLineParser& parser);

/// @brief Reads the filter options.
/// @param parser Parsed command line.
/// @param command Word that selects the command, for the usage message.
/// @param err Where a usage problem is reported.
/// @param code Receives ::arraw::cli::UsageError when the result is empty.
/// @return The filter, inactive when no option was given, or nothing after reporting the problem:
/// a rating that is not 1 to 5, a name that is no colour label, or `--rejected` with
/// `--min-rating`.
[[nodiscard]] std::optional<MarksFilter> readMarksFilter(const QCommandLineParser& parser,
                                                         std::string_view command,
                                                         std::ostream& err, int& code);

/// @brief Tells whether a photograph passes a filter, by the marks its sidecar holds.
///
/// A photograph without a sidecar, or whose sidecar is ignored, has the default
/// marks: no rating and no label.
/// @param filter Filter to apply.
/// @param photograph File to test.
/// @param useSidecar Whether the sidecar is read; false is `--no-sidecar`.
/// @return `true` if the photograph is wanted; always for an inactive filter, which reads nothing.
/// @throws std::runtime_error if the sidecar cannot be read, so that the marks are unknown.
[[nodiscard]] bool passesFilter(const MarksFilter& filter, const std::filesystem::path& photograph,
                                bool useSidecar);

} // namespace arraw::cli
