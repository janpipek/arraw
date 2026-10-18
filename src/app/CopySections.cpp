#include "CopySections.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>

namespace arraw::app {

namespace {

const char* const settingsKey = "copySettings/sections";

/// Set once Demosaic has been added to a remembered selection that predates it, and by every
/// save, which writes a selection that has seen it.
const char* const demosaicMigratedKey = "copySettings/demosaicMigrated";

/// As ::demosaicMigratedKey, for Lens (ADR 050).
const char* const lensMigratedKey = "copySettings/lensMigrated";

QString tr(const char* text) {
    return QCoreApplication::translate("CopySections", text);
}

std::vector<CopySection> inOrder(const std::bitset<copySectionNames.size()>& chosen) {
    std::vector<CopySection> sections;
    for (const CopySection section : copyableSections) {
        if (chosen.test(static_cast<std::size_t>(section))) {
            sections.push_back(section);
        }
    }
    return sections;
}

/// Adds a section to a remembered selection that predates it, once.
void migrate(QSettings& store, const char* migratedKey, CopySection section) {
    if (store.contains(migratedKey)) {
        return;
    }
    if (store.contains(settingsKey)) {
        QStringList names = store.value(settingsKey).toStringList();
        const QString name =
            QString::fromUtf8(copySectionNames[static_cast<std::size_t>(section)].data());
        if (!names.contains(name)) {
            names.push_back(name);
            store.setValue(settingsKey, names);
        }
    }
    store.setValue(migratedKey, true);
}

} // namespace

QString copySectionLabel(CopySection section) {
    switch (section) {
    case CopySection::WhiteBalance:
        return tr("White Balance");
    case CopySection::Exposure:
        return tr("Exposure");
    case CopySection::Tone:
        return tr("Tone");
    case CopySection::Presence:
        return tr("Presence");
    case CopySection::Color:
        return tr("Colour");
    case CopySection::ToneCurve:
        return tr("Tone Curve");
    case CopySection::Hsl:
        return tr("HSL / Colour Mix");
    case CopySection::BlackAndWhite:
        return tr("Black & White");
    case CopySection::ColorGrading:
        return tr("Colour Grading");
    case CopySection::NoiseReduction:
        return tr("Noise Reduction");
    case CopySection::Vignette:
        return tr("Post-crop Vignette");
    case CopySection::Grain:
        return tr("Grain");
    case CopySection::RotateAndFlip:
        return tr("Rotate & Flip");
    case CopySection::Crop:
        return tr("Crop");
    case CopySection::Demosaic:
        return tr("Demosaic");
    case CopySection::Lens:
        return tr("Lens Corrections");
    }
    return {};
}

std::vector<CopySection> restoreCopySections(QSettings& store) {
    // A selection remembered before Demosaic or Lens existed would leave it unchecked for good,
    // while the default is to carry it: add it to such a selection, once (ADR 049, ADR 050).
    migrate(store, demosaicMigratedKey, CopySection::Demosaic);
    migrate(store, lensMigratedKey, CopySection::Lens);
    if (!store.contains(settingsKey)) {
        return {defaultCopySections.begin(), defaultCopySections.end()};
    }
    std::bitset<copySectionNames.size()> chosen;
    for (const QString& name : store.value(settingsKey).toStringList()) {
        const auto found = std::ranges::find_if(copySectionNames, [&](std::string_view known) {
            return name == QString::fromUtf8(known.data(), static_cast<qsizetype>(known.size()));
        });
        if (found != copySectionNames.end()) {
            chosen.set(static_cast<std::size_t>(found - copySectionNames.begin()));
        }
    }
    return inOrder(chosen);
}

void saveCopySections(std::span<const CopySection> sections, QSettings& store) {
    QStringList names;
    for (const CopySection section : sections) {
        names.push_back(QString::fromUtf8(copySectionNames[static_cast<std::size_t>(section)]));
    }
    // An empty list stays a stored value: it means none checked, not "use the default".
    store.setValue(settingsKey, names);
    store.setValue(demosaicMigratedKey, true);
    store.setValue(lensMigratedKey, true);
}

QString skippedMessage(std::span<const CopySection> skipped) {
    if (skipped.empty()) {
        return {};
    }
    QStringList labels;
    for (const CopySection section : skipped) {
        labels.push_back(copySectionLabel(section));
    }
    if (skipped.size() == 1 && skipped.front() == CopySection::WhiteBalance) {
        return tr("White balance was not pasted: it does not cross between a RAW and another "
                  "photograph.");
    }
    return tr("Not pasted, because it does not apply to this photograph: %1.")
        .arg(labels.join(", "));
}

} // namespace arraw::app
