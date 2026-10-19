#include "LensfunSource.h"

#include <ColorEncoding.h>
#include <LensMatch.h>
#include <LensResolution.h>

#include <cmath>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;

namespace {

/// @brief Checks whether a photograph's lens corrections apply: a RAW's do, nothing else's.
bool appliesTo(const ImageMetadata& metadata) {
    return !std::holds_alternative<NamedEncoding>(metadata.encoding);
}

/// @brief Hands a notice to a log.
void note(DiagnosticLog& log, const std::optional<std::filesystem::path>& subject, Notice notice,
          Severity severity, std::vector<DiagnosticValue> values = {}) {
    log.record({.notice = notice, .severity = severity, .subject = subject, .values = values});
}

/// @brief Names the lenses of a list, joined by a bar.
std::string namesOf(const std::vector<LensIdentity>& lenses) {
    std::string names;
    for (const LensIdentity& lens : lenses) {
        names += (names.empty() ? "" : " | ") + lens.model;
    }
    return names;
}

/// @brief Reports why matching found no lens.
void reportNoMatch(const LensNoMatch& none, const LensFacts& facts, DiagnosticLog& log,
                   const std::optional<std::filesystem::path>& subject) {
    std::vector<DiagnosticValue> values;
    switch (none.problem) {
    case LensProblem::LensAmbiguous:
        values = {namesOf(none.candidates)};
        break;
    case LensProblem::LensFocalUnknown:
        values = {none.candidates.empty() ? std::string{} : none.candidates.front().model};
        break;
    case LensProblem::LensNotInDatabase:
        values = {facts.lensNames.empty() ? std::string{} : facts.lensNames.front()};
        break;
    default:
        break;
    }
    note(log, subject, noticeFor(none.problem), Severity::Warning, std::move(values));
}

/// @brief Gives the match a recorded lens stands for, or reports why there is none.
std::optional<LensMatch> matchOfRecorded(const RecordedLens& recorded, const LensFacts& facts,
                                         const LensDatabase& database, DiagnosticLog& log,
                                         const std::optional<std::filesystem::path>& subject) {
    const auto entry = database.lens(recorded.maker, recorded.model, recorded.cropFactor);
    if (!entry) {
        note(log, subject, Notice::LensProfileMissing, Severity::Warning, {recorded.model});
        return std::nullopt;
    }
    if (recorded.database != database.stamp()) {
        note(log, subject, Notice::LensDatabaseChanged, Severity::Info,
             {recorded.database, database.stamp()});
    }
    LensMatch match;
    match.lens = {entry->maker, entry->model};
    match.name = entry->model;
    match.exactName = true;
    match.cameraListed = database.findCamera(facts.cameraMaker, facts.cameraModel).has_value();
    match.cropFactor = recorded.cropFactor;
    match.cropSource = CropSource::Camera;
    if (facts.focalLength) {
        match.focalLength = *facts.focalLength;
        match.focalLengthRead = true;
    } else if (entry->minFocal > 0.0 && entry->minFocal == entry->maxFocal) {
        match.focalLength = entry->minFocal;
    } else {
        note(log, subject, Notice::LensFocalUnknown, Severity::Warning, {entry->model});
        return std::nullopt;
    }
    return match;
}

} // namespace

Notice arraw::noticeFor(LensProblem problem) noexcept {
    switch (problem) {
    case LensProblem::LensUnidentified:
        return Notice::LensUnidentified;
    case LensProblem::LensFocalUnknown:
        return Notice::LensFocalUnknown;
    case LensProblem::CameraNotInDatabase:
        return Notice::CameraNotInDatabase;
    case LensProblem::LensNotInDatabase:
        return Notice::LensNotInDatabase;
    case LensProblem::LensAmbiguous:
        return Notice::LensAmbiguous;
    case LensProblem::LensProfileMissing:
        return Notice::LensProfileMissing;
    case LensProblem::LensDatabaseChanged:
        return Notice::LensDatabaseChanged;
    case LensProblem::LensDatabaseMissing:
        return Notice::LensDatabaseMissing;
    case LensProblem::LensfunUnavailable:
        return Notice::LensfunUnavailable;
    }
    return Notice::LensNotInDatabase;
}

std::optional<LensMatch>
arraw::resolveLensMatch(const LensSettings& settings, const LensFacts& facts,
                        const LensDatabase& database, DiagnosticLog& log,
                        const std::optional<std::filesystem::path>& subject) {
    if (!lensfunAvailable()) {
        note(log, subject, Notice::LensfunUnavailable, Severity::Warning);
        return std::nullopt;
    }
    if (const auto problem = database.problem()) {
        note(log, subject, noticeFor(*problem), Severity::Warning);
        return std::nullopt;
    }
    if (settings.lens) {
        return matchOfRecorded(*settings.lens, facts, database, log, subject);
    }
    const LensMatchResult found = matchLens(facts, database);
    if (const auto* none = std::get_if<LensNoMatch>(&found)) {
        reportNoMatch(*none, facts, log, subject);
        return std::nullopt;
    }
    LensMatch match = std::get<LensMatch>(found);
    if (!match.cameraListed) {
        note(log, subject, Notice::CameraNotInDatabase, Severity::Info,
             {facts.cameraMaker, facts.cameraModel});
    }
    return match;
}

LensCorrection arraw::resolveLens(const LensSettings& settings, const LensFacts& facts,
                                  const LensDatabase& database, ImageSize size, DiagnosticLog& log,
                                  const std::optional<std::filesystem::path>& subject) {
    const LensKinds kinds = kindsOf(settings);
    if (!kinds.any()) {
        return {};
    }
    const std::optional<LensMatch> match =
        resolveLensMatch(settings, facts, database, log, subject);
    if (!match) {
        return {};
    }

    std::optional<LensCorrection> sampled = sampleLensfun(database, *match, facts, size, kinds);
    if (!sampled) {
        note(log, subject, Notice::LensProfileMissing, Severity::Warning, {match->lens.model});
        return {};
    }
    std::string missing;
    const auto lacks = [&](bool wanted, bool present, const char* name) {
        if (wanted && !present) {
            missing += (missing.empty() ? "" : ", ") + std::string(name);
        }
    };
    lacks(kinds.distortion, !sampled->distortion.empty(), "distortion");
    lacks(kinds.vignetting, !sampled->vignetting.empty(), "vignetting");
    lacks(kinds.lateralCa, sampled->hasLateralCa(), "chromatic aberration");
    if (!missing.empty()) {
        note(log, subject, Notice::LensNotCalibrated, Severity::Warning,
             {match->lens.model, missing});
    }
    return withVignettingAmount(std::move(*sampled), settings.vignettingAmount);
}

LensCorrection arraw::lensCorrectionFor(const Photo& photo, const LensDatabase& database,
                                        DiagnosticLog& log) {
    if (!appliesTo(photo.metadata())) {
        return {};
    }
    return resolveLens(photo.negative().lens, photo.metadata().lens, database,
                       photo.metadata().size, log, photo.path());
}

LensCorrection arraw::lensCorrectionFor(const Photo& photo, DiagnosticLog& log) {
    if (!appliesTo(photo.metadata()) || !photo.negative().lens.any()) {
        return {};
    }
    return lensCorrectionFor(photo, LensDatabase::system(), log);
}

std::optional<Photo> arraw::withChosenLens(const Photo& photo, const LensIdentity& lens,
                                           const LensDatabase& database) {
    if (!appliesTo(photo.metadata())) {
        return std::nullopt;
    }
    const auto entry = database.lens(lens.maker, lens.model);
    if (!entry) {
        return std::nullopt;
    }
    const LensFacts& facts = photo.metadata().lens;
    double cropFactor = entry->cropFactor;
    if (const auto camera = database.findCamera(facts.cameraMaker, facts.cameraModel)) {
        cropFactor = camera->cropFactor;
    } else if (facts.focalLength && facts.focalLength35 && *facts.focalLength > 0.0) {
        // Rounded to the hundredths a camera's crop factor is given in, as matching does.
        cropFactor = std::round(*facts.focalLength35 / *facts.focalLength * 100.0) / 100.0;
    }
    NegativeSettings negative = photo.negative();
    negative.lens.lens = RecordedLens{.maker = entry->maker,
                                      .model = entry->model,
                                      .cropFactor = cropFactor,
                                      .origin = LensOrigin::Chosen,
                                      .database = database.stamp()};
    return photo.with(std::move(negative));
}

Photo arraw::recordLensMatch(const Photo& photo, const LensDatabase& database) {
    const LensSettings& settings = photo.negative().lens;
    if (!settings.any() || settings.lens || !appliesTo(photo.metadata())) {
        return photo;
    }
    const LensMatchResult found = matchLens(photo.metadata().lens, database);
    const auto* match = std::get_if<LensMatch>(&found);
    if (match == nullptr) {
        return photo;
    }
    NegativeSettings negative = photo.negative();
    negative.lens.lens = RecordedLens{.maker = match->lens.maker,
                                      .model = match->lens.model,
                                      .cropFactor = match->cropFactor,
                                      .origin = LensOrigin::Matched,
                                      .database = database.stamp()};
    return photo.with(std::move(negative));
}
