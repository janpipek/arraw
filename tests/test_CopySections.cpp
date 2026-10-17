#include "CopySections.h"
#include "support/TempDir.h"

#include <QSettings>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace arraw;
using namespace arraw::app;

namespace {

QSettings scratch(const test::TempDir& directory) {
    return QSettings(QString::fromStdU16String(directory.file("copy.ini").u16string()),
                     QSettings::IniFormat);
}

const std::vector<CopySection> defaults(defaultCopySections.begin(), defaultCopySections.end());

} // namespace

TEST_CASE("Copy sections come back as saved", "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    const std::vector<CopySection> chosen{CopySection::Exposure, CopySection::Grain};
    saveCopySections(chosen, store);
    store.sync();
    QSettings again(store.fileName(), QSettings::IniFormat);
    CHECK(restoreCopySections(again) == chosen);
}

TEST_CASE("Missing copy sections mean the default, an empty list means none",
          "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    CHECK(restoreCopySections(store) == defaults);
    saveCopySections({}, store);
    store.sync();
    QSettings again(store.fileName(), QSettings::IniFormat);
    CHECK(restoreCopySections(again).empty());
}

TEST_CASE("Unknown section names are dropped, the geometry is kept", "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    store.setValue("copySettings/sections", QStringList{"tone", "nonsense", "crop", "rotateAndFlip",
                                                        "whiteBalance", "tone"});
    // A selection from before the demosaic existed also gets it, once.
    CHECK(restoreCopySections(store) ==
          std::vector<CopySection>{CopySection::WhiteBalance, CopySection::Tone,
                                   CopySection::RotateAndFlip, CopySection::Crop,
                                   CopySection::Demosaic});
}

TEST_CASE("Every copy section has a label", "[app][copy]") {
    for (const CopySection section : copyableSections) {
        CHECK_FALSE(copySectionLabel(section).isEmpty());
    }
}

TEST_CASE("Skipped sections are worded, and nothing skipped is silent", "[app][copy]") {
    CHECK(skippedMessage({}).isEmpty());
    const std::vector<CopySection> white{CopySection::WhiteBalance};
    CHECK(skippedMessage(white).contains("White balance"));
}

TEST_CASE("A remembered selection from before Demosaic gets it, once", "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    store.setValue("copySettings/sections", QStringList{"tone", "exposure"});
    CHECK(restoreCopySections(store) == std::vector<CopySection>{CopySection::Exposure,
                                                                 CopySection::Tone,
                                                                 CopySection::Demosaic});

    // Unchecked afterwards, it stays unchecked.
    saveCopySections(std::vector<CopySection>{CopySection::Tone}, store);
    CHECK(restoreCopySections(store) == std::vector<CopySection>{CopySection::Tone});
    store.sync();
    QSettings again(store.fileName(), QSettings::IniFormat);
    CHECK(restoreCopySections(again) == std::vector<CopySection>{CopySection::Tone});
}

TEST_CASE("A selection that none was stored for is the default, and the migration is spent",
          "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    CHECK(restoreCopySections(store) == defaults);
    CHECK(std::ranges::find(defaults, CopySection::Demosaic) != defaults.end());
    // An empty selection that was saved means none, migration or not.
    saveCopySections({}, store);
    CHECK(restoreCopySections(store).empty());
}
