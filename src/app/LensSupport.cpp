#include "LensSupport.h"

#include <ColorEncoding.h>
#include <Diagnostics.h>
#include <LensResolution.h>

#include <QCoreApplication>

#include <algorithm>
#include <string>
#include <variant>

namespace arraw::app {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("arraw::app::LensSupport", text);
}

/// @brief Capitalises the first letter of a sentence the core worded in lower case.
QString sentence(const Diagnostic& diagnostic) {
    QString text = QString::fromStdString(describe(diagnostic));
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
        if (!text.endsWith(QLatin1Char('.'))) {
            text += QLatin1Char('.');
        }
    }
    return text;
}

/// @brief Finds the lens a resolved correction came from.
std::optional<LensIdentity> lensOf(const LensCorrection& correction) {
    for (const auto* from :
         {&correction.distortionFrom, &correction.vignettingFrom, &correction.lateralCaFrom}) {
        if (*from) {
            return (*from)->lens;
        }
    }
    return std::nullopt;
}

/// @brief Tells how the lens came to be the photograph's.
QString originText(const LensSettings& settings) {
    if (!settings.lens) {
        return tr("found automatically");
    }
    return settings.lens->origin == LensOrigin::Chosen ? tr("chosen") : tr("recorded");
}

} // namespace

LensDatabaseSource systemLensDatabase() {
    return []() -> const LensDatabase& { return LensDatabase::system(); };
}

QString lensDisplayName(const LensIdentity& lens) {
    if (lens.maker.empty() || lens.model.starts_with(lens.maker)) {
        return QString::fromStdString(lens.model);
    }
    return QString::fromStdString(lens.maker + " " + lens.model);
}

LensView resolveLensView(const Photo& photo, const LensDatabase* database) {
    LensView view;
    LensStatus& status = view.status;
    if (std::holds_alternative<NamedEncoding>(photo.metadata().encoding)) {
        status.text = tr("Lens corrections apply only to a RAW file.");
        return view;
    }
    status.applicable = true;
    if (database == nullptr) {
        status.loading = true;
        status.text = tr("Loading the lens database…");
        return view;
    }
    const LensSettings& settings = photo.negative().lens;
    status.canChoose = database->loaded();
    status.canReset = settings.lens.has_value();

    CollectedDiagnostics log;
    std::optional<LensIdentity> lens;
    if (settings.any()) {
        view.correction = lensCorrectionFor(photo, *database, log);
        lens = lensOf(view.correction);
    } else if (const auto match = resolveLensMatch(settings, photo.metadata().lens, *database, log,
                                                   photo.path())) {
        lens = match->lens;
    }

    if (!lens && settings.any()) {
        // A correction can come back inactive for want of a calibration; the lens was still found.
        CollectedDiagnostics discarded;
        if (const auto match = resolveLensMatch(settings, photo.metadata().lens, *database,
                                                discarded, photo.path())) {
            lens = match->lens;
        }
    }

    QString problem;
    for (const Diagnostic& entry : log.entries()) {
        switch (entry.notice) {
        case Notice::LensAmbiguous:
            if (const auto found = matchLens(photo.metadata().lens, *database);
                std::holds_alternative<LensNoMatch>(found)) {
                status.candidates = std::get<LensNoMatch>(found).candidates;
            }
            [[fallthrough]];
        case Notice::LensUnidentified:
        case Notice::LensFocalUnknown:
        case Notice::LensNotInDatabase:
        case Notice::LensProfileMissing:
        case Notice::LensDatabaseMissing:
        case Notice::LensfunUnavailable:
            if (problem.isEmpty()) {
                problem = sentence(entry);
            }
            break;
        case Notice::LensDatabaseChanged:
        case Notice::LensNotCalibrated:
            status.note +=
                (status.note.isEmpty() ? QString() : QStringLiteral("\n")) + sentence(entry);
            break;
        default:
            break;
        }
    }
    if (lens && problem.isEmpty()) {
        status.found = true;
        status.text = tr("lensfun: %1 (%2)").arg(lensDisplayName(*lens), originText(settings));
    } else {
        if (problem.isEmpty()) {
            problem = tr("No lens was found.");
        }
        status.text = tr("No profile: %1").arg(problem);
    }
    return view;
}

NegativeSettings withMatchRecorded(const Photo& photo, NegativeSettings negative,
                                   const LensDatabase* database) {
    if (database == nullptr) {
        return negative;
    }
    return recordLensMatch(photo.with(std::move(negative)), *database).negative();
}

NegativeSettings withLensReset(const Photo& photo, const LensDatabase* database) {
    NegativeSettings negative = photo.negative();
    negative.lens.lens.reset();
    return withMatchRecorded(photo, std::move(negative), database);
}

} // namespace arraw::app
