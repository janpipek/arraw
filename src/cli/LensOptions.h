#pragma once

#include <LensCorrection.h>
#include <LensDatabase.h>
#include <LensMatch.h>
#include <Photo.h>

#include <QCommandLineParser>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string_view>

/// @file
/// @brief The lens options `export` and `info` share (ADR 050).

namespace arraw::cli {

/// @brief What the lens flags of a command said.
struct LensOptions {
    /// @brief Corrections `--lens-corrections` named, applied over each RAW's own settings; none
    /// keeps them.
    std::optional<LensKinds> corrections;

    /// @brief Lens `--lens` named, recorded as chosen over what each RAW had; none keeps it.
    std::optional<LensIdentity> lens;

    /// @brief Directory `--lens-database` named; none uses the process's database.
    std::optional<std::filesystem::path> database;
};

/// @brief Declares `--lens-database`, and with @p edits also `--lens-corrections` and `--lens`.
/// @param parser Parser of the command.
/// @param edits Whether the command may change the lens settings it reports on or renders.
void addLensOptions(QCommandLineParser& parser, bool edits);

/// @brief Reads the lens flags.
/// @param parser Parser that parsed the command's arguments.
/// @param edits Whether the command declared the flags of ::arraw::cli::addLensOptions with
/// @p edits.
/// @param command Name of the command, for usage errors.
/// @param err Where a usage error goes.
/// @param code Set to the exit status when nothing is returned.
/// @return The options, or nothing after a usage error.
[[nodiscard]] std::optional<LensOptions> readLensOptions(const QCommandLineParser& parser,
                                                         bool edits, std::string_view command,
                                                         std::ostream& err, int& code);

/// @brief The lens database of a command, loaded when it is first wanted and not before.
///
/// A batch whose photographs ask for nothing of a lens never loads it. The directory
/// `--lens-database` named is used if there was one, else the process's database.
class LensDatabaseSource {
public:
    /// @brief Remembers where to load the database from.
    /// @param directory Directory of lensfun files, or none for the process's database.
    explicit LensDatabaseSource(std::optional<std::filesystem::path> directory = std::nullopt);

    /// @brief Gives the database, loading it on the first call.
    [[nodiscard]] const LensDatabase& get();

private:
    std::optional<std::filesystem::path> directory_;
    std::optional<LensDatabase> loaded_;
};

/// @brief Applies the flags to a photograph, before it is rendered.
///
/// Sets the three switches the flags named and records the lens `--lens` named as chosen. A
/// photograph that is not a RAW is returned as it was.
/// @param photo Photograph as opened.
/// @param options What the flags said.
/// @param database Where the lens is looked up.
/// @return The photograph with the flags applied.
/// @throws std::runtime_error if `--lens` names a lens the database does not list.
[[nodiscard]] Photo applyLensOptions(const Photo& photo, const LensOptions& options,
                                     LensDatabaseSource& database);

} // namespace arraw::cli
