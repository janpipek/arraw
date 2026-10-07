#include "CopySections.h"
#include "support/TempDir.h"

#include <QSettings>
#include <QString>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Unknown and uncopyable section names are dropped", "[app][settings][copy]") {
    const test::TempDir directory;
    QSettings store = scratch(directory);
    store.setValue("copySettings/sections", QStringList{"tone", "nonsense", "crop", "rotateAndFlip",
                                                        "whiteBalance", "tone"});
    CHECK(restoreCopySections(store) ==
          std::vector<CopySection>{CopySection::WhiteBalance, CopySection::Tone});
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
