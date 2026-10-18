#pragma once

#include <LensCorrection.h>
#include <LensDatabase.h>
#include <LensMatch.h>
#include <NegativeSettings.h>
#include <Photo.h>

#include <QString>

#include <functional>
#include <optional>
#include <vector>

/// @file
/// @brief What the application does with a photograph's lens corrections: resolving them for a
/// render and for the Negative group (ADR 050, step 7).

namespace arraw::app {

/// @brief Gives the lens database, loading it on the first call.
///
/// What the window and the export queue call off the interface thread; tests give a fixture's.
using LensDatabaseSource = std::function<const LensDatabase&()>;

/// @brief Gives the process's lens database, as ::arraw::LensDatabase::system.
/// @return A source for it.
[[nodiscard]] LensDatabaseSource systemLensDatabase();

/// @brief What the lens rows of the Negative group show for a photograph.
struct LensStatus {
    /// @brief Whether the photograph's corrections can be set: a RAW.
    bool applicable = false;

    /// @brief Whether the lens database is still being loaded, so nothing can be set yet.
    bool loading = false;

    /// @brief The line under the switches: the lens found and how, or why there is no profile.
    QString text;

    /// @brief A second line, such as a database updated since the lens was recorded, or a
    /// correction the lens has no calibration for; empty for none.
    QString note;

    /// @brief Whether a lens was found, recorded or matched.
    bool found = false;

    /// @brief Whether "Choose lens…" can be offered: the database is loaded.
    bool canChoose = false;

    /// @brief Whether a lens is recorded, which "Reset to automatic" clears.
    bool canReset = false;

    /// @brief The lenses that fitted equally well, when two did; the picker lists them first.
    std::vector<LensIdentity> candidates;
};

/// @brief A photograph's lens corrections as the window needs them.
struct LensView {
    /// @brief What a render applies; empty when no switch is on or nothing resolved.
    LensCorrection correction;

    /// @brief What the Negative group shows.
    LensStatus status;
};

/// @brief Names a lens for a person: its model, with its maker unless the model begins with it.
/// @param lens Lens as lensfun names it.
/// @return The name.
[[nodiscard]] QString lensDisplayName(const LensIdentity& lens);

/// @brief Resolves a photograph's lens corrections and the words that go with them.
///
/// The correction is resolved only when a switch is on (::arraw::lensCorrectionFor); the status
/// names the lens the photograph's corrections use, recorded or matched, and else says why there
/// is no profile (the notices of ::arraw::LensProblem, in words). Reads the lens database, so it
/// takes some milliseconds when it has to match a lens: for the interface thread only when the
/// result is cached, as the window does.
/// @param photo Document to resolve.
/// @param database Database to look in; null while it loads, which gives a status that says so
/// and no correction.
/// @return The correction and the status.
[[nodiscard]] LensView resolveLensView(const Photo& photo, const LensDatabase* database);

/// @brief Gives negative settings with the match recorded when a correction is on and nothing is.
///
/// ::arraw::recordLensMatch for settings the photograph does not carry yet, so that the step that
/// turns a correction on records the lens with it (ADR 050, section 6).
/// @param photo Document the settings are for.
/// @param negative Settings to record the match in.
/// @param database Database to match in; null leaves the settings as they are.
/// @return The settings, with the lens recorded when that changed anything.
[[nodiscard]] NegativeSettings withMatchRecorded(const Photo& photo, NegativeSettings negative,
                                                 const LensDatabase* database);

/// @brief Gives negative settings with the recorded lens cleared and, if a correction is on,
/// matched again.
/// @param photo Document the settings are for.
/// @param database Database to match in; null only clears.
/// @return The settings.
[[nodiscard]] NegativeSettings withLensReset(const Photo& photo, const LensDatabase* database);

} // namespace arraw::app
