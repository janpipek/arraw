#pragma once

#include <Diagnostics.h>
#include <ImageBuffer.h>
#include <LensCorrection.h>
#include <LensDatabase.h>
#include <LensFacts.h>
#include <LensMatch.h>
#include <LensProblem.h>
#include <NegativeSettings.h>
#include <Photo.h>

#include <filesystem>
#include <optional>

/// @file
/// @brief Resolving a photograph's lens settings into the correction the render applies (ADR 050,
/// section 6).

namespace arraw {

/// @brief Gives the corrections the settings ask for, as the profile sampling takes them.
/// @param settings Lens settings.
/// @return Which corrections are asked for (a vignetting amount above 0 asks for vignetting).
[[nodiscard]] constexpr LensKinds kindsOf(const LensSettings& settings) noexcept {
    return {settings.distortion, settings.vignettingAmount > 0.0F, settings.lateralCa};
}

/// @brief Gives the notice that tells a photographer of a lens problem.
/// @param problem Why a lens could not be resolved.
/// @return The notice of the same name.
[[nodiscard]] Notice noticeFor(LensProblem problem) noexcept;

/// @brief Finds the lens a photograph's corrections are for, or says why there is none.
///
/// The recorded lens when there is one, looked up by its exact name (a lens that has left the
/// database gives ::arraw::Notice::LensProfileMissing, a database of another stamp the
/// informational ::arraw::Notice::LensDatabaseChanged); else the match of the facts
/// (::arraw::matchLens), with ::arraw::Notice::CameraNotInDatabase when the camera is not listed.
/// A build without lensfun and a database that did not load are told too. Nothing is sampled, and
/// which corrections are on is not looked at, so this serves a display of the lens as well as a
/// render.
/// @param settings Corrections and the recorded lens, if any.
/// @param facts What the photograph says about its camera and lens.
/// @param database Database to look in.
/// @param log Where the notices go.
/// @param subject Photograph the notices are about, or none.
/// @return The lens with the crop factor and focal length to resolve it at, or nothing.
[[nodiscard]] std::optional<LensMatch>
resolveLensMatch(const LensSettings& settings, const LensFacts& facts, const LensDatabase& database,
                 DiagnosticLog& log = discardedDiagnostics(),
                 const std::optional<std::filesystem::path>& subject = std::nullopt);

/// @brief Resolves lens settings into the correction a render applies, or says why it cannot.
///
/// With no correction on, nothing is looked at and the result is empty. A recorded lens is looked
/// up by its exact lensfun name and used with its recorded crop factor; no fuzzy matching runs, so
/// a database update cannot move an edit to another lens. If the recorded lens is no longer in the
/// database the corrections are skipped with ::arraw::Notice::LensProfileMissing, never matched
/// again; a database of another stamp gives the informational
/// ::arraw::Notice::LensDatabaseChanged and goes on. With no recorded lens the facts are matched
/// (::arraw::matchLens). When a correction cannot be resolved the render goes on without it, and
/// one notice names the reason (the notices of ::arraw::LensProblem, plus
/// ::arraw::Notice::LensNotCalibrated when the lens is found but has no calibration for a
/// correction that is on, in which case the others are still applied). The vignetting gain is
/// scaled by the settings' amount (::arraw::withVignettingAmount), so an amount of 100 is the
/// profile as it is.
/// @param settings Corrections and the recorded lens, if any.
/// @param facts What the photograph says about its camera and lens.
/// @param database Database to look in.
/// @param size Size of the photograph, which fixes the aspect the tables are sampled at.
/// @param log Where the notices go.
/// @param subject Photograph the notices are about, or none.
/// @return The correction, its fill scale set; empty when no correction is on or nothing resolved.
[[nodiscard]] LensCorrection
resolveLens(const LensSettings& settings, const LensFacts& facts, const LensDatabase& database,
            ImageSize size, DiagnosticLog& log = discardedDiagnostics(),
            const std::optional<std::filesystem::path>& subject = std::nullopt);

/// @brief Resolves a photograph's lens corrections: the rendering twin of
/// ::arraw::decodeOptionsFor.
///
/// Whoever holds a ::arraw::Photo and renders it passes this to the render (ADR 050, section 4).
/// Corrections apply to a RAW only; for any other photograph the result is empty and nothing is
/// said.
/// @param photo Document to render.
/// @param database Database to look in.
/// @param log Where the notices go; the photograph is their subject.
/// @return The correction, as ::arraw::resolveLens.
[[nodiscard]] LensCorrection lensCorrectionFor(const Photo& photo, const LensDatabase& database,
                                               DiagnosticLog& log = discardedDiagnostics());

/// @brief Resolves a photograph's lens corrections against the process's database.
///
/// As the overload with a database, which it takes from ::arraw::LensDatabase::system only when
/// the photograph asks for a correction, so a photograph without one never loads it. The first
/// load is slow: not for the interface thread.
/// @param photo Document to render.
/// @param log Where the notices go.
/// @return The correction.
[[nodiscard]] LensCorrection lensCorrectionFor(const Photo& photo,
                                               DiagnosticLog& log = discardedDiagnostics());

/// @brief Records the lens of a photograph that has a correction on and no lens recorded.
///
/// Matches the photograph's facts (::arraw::matchLens) and records the lens found with its crop
/// factor, origin ::arraw::LensOrigin::Matched, and the database's stamp, so that a database
/// update cannot move the edit to another lens (ADR 050, section 6). Nothing is recorded, and the
/// photograph is returned as it was, when no correction is on, a lens is recorded already, the
/// photograph is not a RAW, or no lens matches (resolution then says why, at render).
/// @param photo Document to record for.
/// @param database Database to match in.
/// @return The photograph, with its lens recorded when this changed anything.
[[nodiscard]] Photo recordLensMatch(const Photo& photo, const LensDatabase& database);

/// @brief Records a lens the photographer chose, in place of the one matched or recorded.
///
/// The lens is looked up by its exact name. Its crop factor is the camera's when the camera is in
/// the database, else the 35 mm focal length over the focal length, else the lens's own; the
/// origin is ::arraw::LensOrigin::Chosen and the stamp the database's. The corrections stay as they
/// are, and no focal range or aperture filter applies: the choice is the photographer's.
/// @param photo Document to record for; a RAW.
/// @param lens Lens to record, as lensfun names it.
/// @param database Database the lens must be listed in.
/// @return The photograph with the lens recorded, or nothing when the database does not list the
/// lens or the photograph is not a RAW.
[[nodiscard]] std::optional<Photo> withChosenLens(const Photo& photo, const LensIdentity& lens,
                                                  const LensDatabase& database);

} // namespace arraw
