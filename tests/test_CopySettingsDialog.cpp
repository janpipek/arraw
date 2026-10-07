#include "CopySections.h"
#include "ui/CopySettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace arraw;
using namespace arraw::app;

TEST_CASE("The copy dialog offers only what can be copied", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections);
    CHECK(dialog.findChildren<QCheckBox*>().size() ==
          static_cast<qsizetype>(copyableSections.size()));
    CHECK(dialog.findChild<QCheckBox*>("crop") == nullptr);
    CHECK(dialog.findChild<QCheckBox*>("rotateAndFlip") == nullptr);
    CHECK(dialog.sections() ==
          std::vector<CopySection>(copyableSections.begin(), copyableSections.end()));
}

TEST_CASE("The copy dialog shows an ampersand in a label, not a mnemonic", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections);
    CHECK(dialog.findChild<QCheckBox*>("blackAndWhite")->text().contains("&&"));
}

TEST_CASE("The copy dialog accepts only with a section checked", "[app][dialog][copy]") {
    CopySettingsDialog dialog(defaultCopySections);
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
        CopySettingsDialog first(restoreCopySections(store));
        first.findChild<QPushButton*>("checkNone")->click();
        first.findChild<QCheckBox*>("exposure")->setChecked(true);
        saveCopySections(first.sections(), store);
    }
    CopySettingsDialog second(restoreCopySections(store));
    CHECK(second.sections() == std::vector<CopySection>{CopySection::Exposure});
}
