#include "CopySections.h"
#include "ui/CopySettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace arraw;
using namespace arraw::app;

TEST_CASE("The copy dialog offers only what can be copied", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections, true);
    CHECK(dialog.findChildren<QCheckBox*>().size() ==
          static_cast<qsizetype>(copyableSections.size()));
    CHECK(dialog.findChild<QCheckBox*>("crop") == nullptr);
    CHECK(dialog.findChild<QCheckBox*>("rotateAndFlip") == nullptr);
    CHECK(dialog.sections() ==
          std::vector<CopySection>(copyableSections.begin(), copyableSections.end()));
}

TEST_CASE("The copy dialog shows an ampersand in a label, not a mnemonic", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections, true);
    CHECK(dialog.findChild<QCheckBox*>("blackAndWhite")->text().contains("&&"));
}

TEST_CASE("The copy dialog accepts only with a section checked", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections, true);
    auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    CHECK(ok->isEnabled());
    dialog.findChild<QPushButton*>("checkNone")->click();
    CHECK(dialog.sections().empty());
    CHECK_FALSE(ok->isEnabled());
    dialog.findChild<QCheckBox*>("tone")->setChecked(true);
    CHECK(ok->isEnabled());
    CHECK(dialog.sections() == std::vector<CopySection>{CopySection::Tone});
    dialog.findChild<QPushButton*>("checkAll")->click();
    CHECK(dialog.sections().size() == copyableSections.size());
}

TEST_CASE("The selection is remembered across two dialogs", "[app][dialog][copy]") {
    const QTemporaryDir directory;
    QSettings store(directory.filePath("copy.ini"), QSettings::IniFormat);
    {
        CopySettingsDialog first(restoreCopySections(store), true);
        first.findChild<QPushButton*>("checkNone")->click();
        first.findChild<QCheckBox*>("exposure")->setChecked(true);
        saveCopySections(first.remembered(), store);
    }
    CopySettingsDialog second(restoreCopySections(store), true);
    CHECK(second.sections() == std::vector<CopySection>{CopySection::Exposure});
}

TEST_CASE("From a photograph that is not a RAW, White Balance is unavailable but remembered",
          "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections, false);
    auto* whiteBalance = dialog.findChild<QCheckBox*>("whiteBalance");
    CHECK_FALSE(whiteBalance->isEnabled());
    CHECK_FALSE(whiteBalance->isChecked());
    CHECK_FALSE(whiteBalance->toolTip().isEmpty());
    dialog.findChild<QPushButton*>("checkAll")->click();
    CHECK_FALSE(whiteBalance->isChecked());

    const std::vector<CopySection> sections = dialog.sections();
    CHECK(std::ranges::find(sections, CopySection::WhiteBalance) == sections.end());
    CHECK(sections.size() == copyableSections.size() - 1);
    const std::vector<CopySection> remembered = dialog.remembered();
    CHECK(std::ranges::find(remembered, CopySection::WhiteBalance) != remembered.end());

    dialog.findChild<QPushButton*>("checkNone")->click();
    CHECK_FALSE(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
    // Unchecking what can be seen leaves the unavailable choice as it was.
    CHECK(dialog.remembered() == std::vector<CopySection>{CopySection::WhiteBalance});
}
