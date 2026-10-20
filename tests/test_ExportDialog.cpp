#include "support/TempDir.h"
#include "ui/ExportDialog.h"

#include <ExportPresets.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::app;
namespace fs = std::filesystem;

namespace {

constexpr ImageSize frame{6000, 4000};

/// @brief Finds the size box: the one whose first item is the original size.
QComboBox* sizeBoxOf(ExportDialog& dialog) {
    for (auto* box : dialog.findChildren<QComboBox*>()) {
        if (box->itemText(0) == QObject::tr("Original size")) {
            return box;
        }
    }
    return nullptr;
}

/// @brief Finds the allow-enlarging check.
QCheckBox* enlargeOf(ExportDialog& dialog) {
    for (auto* check : dialog.findChildren<QCheckBox*>()) {
        if (check->text() == QObject::tr("Allow enlarging")) {
            return check;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("The export dialog offers the original size and the long edges",
          "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    REQUIRE(box != nullptr);
    REQUIRE(box->count() == 6);
    CHECK(box->currentIndex() == 0);
    CHECK_FALSE(dialog.settings().resize);
    CHECK_FALSE(enlargeOf(dialog)->isEnabled());
}

TEST_CASE("Each long edge gives a square box of that edge", "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    const std::uint32_t expected[]{1080, 1600, 2048, 3840};
    for (int i = 0; i < 4; ++i) {
        box->setCurrentIndex(i + 1);
        const ExportSettings settings = dialog.settings();
        CHECK(settings.resize);
        CHECK(settings.width == expected[i]);
        CHECK(settings.height == expected[i]);
        CHECK(enlargeOf(dialog)->isEnabled());
    }
    box->setCurrentIndex(0);
    CHECK_FALSE(dialog.settings().resize);
    CHECK_FALSE(enlargeOf(dialog)->isEnabled());
}

TEST_CASE("The custom size starts at the frame and takes the boxes", "[app][dialog][export]") {
    ExportDialog dialog(ExportSettings{}, frame);
    auto* box = sizeBoxOf(dialog);
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    for (auto* spin : dialog.findChildren<QSpinBox*>()) {
        if (spin->suffix() == QObject::tr(" px")) {
            (width == nullptr ? width : height) = spin;
        }
    }
    REQUIRE(height != nullptr);
    CHECK_FALSE(width->isEnabled());
    box->setCurrentIndex(5);
    CHECK(width->isEnabled());
    CHECK(height->isEnabled());
    CHECK(enlargeOf(dialog)->isEnabled());
    ExportSettings settings = dialog.settings();
    CHECK(settings.resize);
    CHECK(settings.width == 6000);
    CHECK(settings.height == 4000);
    width->setValue(800);
    height->setValue(600);
    settings = dialog.settings();
    CHECK(settings.width == 800);
    CHECK(settings.height == 600);
}

TEST_CASE("The dialog starts at the stored size choice", "[app][dialog][export]") {
    {
        ExportDialog dialog(ExportSettings{.resize = true, .width = 1600, .height = 1600}, frame);
        CHECK(sizeBoxOf(dialog)->currentIndex() == 2);
        CHECK(dialog.settings().width == 1600);
    }
    {
        ExportDialog dialog(ExportSettings{.resize = true, .width = 700, .height = 500}, frame);
        CHECK(sizeBoxOf(dialog)->currentIndex() == 5);
        CHECK(dialog.settings().width == 700);
        CHECK(dialog.settings().height == 500);
    }
}

namespace {

/// A presets file in a temporary directory, with a dialog's questions answered by the fields.
struct Presets {
    test::TempDir directory;
    ExportPresetStore store{directory.path()};

    std::optional<QString> typedName;  ///< What the name prompt gives; nothing cancels.
    bool confirmed = true;             ///< What the confirmation gives.
    std::vector<QString> asked;        ///< Texts of the confirmations, in order.
    std::vector<QString> warnings;     ///< Texts of the problems reported.
    std::vector<QString> nameInitials; ///< Initial texts the name prompt was shown with.

    [[nodiscard]] ExportDialog::Prompts prompts() {
        return {.name =
                    [this](const QString&, const QString&, const QString& initial) {
                        nameInitials.push_back(initial);
                        return typedName;
                    },
                .confirm =
                    [this](const QString&, const QString& text) {
                        asked.push_back(text);
                        return confirmed;
                    },
                .warn = [this](const QString&, const QString& text) { warnings.push_back(text); }};
    }

    void write(const std::string& text) const {
        std::ofstream(store.file(), std::ios::binary) << text;
    }

    void save(const std::string& name, const ExportSettings& settings) {
        store.save({.name = name, .settings = settings});
    }
};

QComboBox* presetBoxOf(ExportDialog& dialog) {
    return dialog.findChild<QComboBox*>("presetBox");
}

QAction* actionOf(ExportDialog& dialog, const char* name) {
    return dialog.findChild<QAction*>(name);
}

QPushButton* saveAsOf(ExportDialog& dialog) {
    return dialog.findChild<QPushButton*>("presetSaveAs");
}

/// @brief Lists the texts of the picker's items, separators left out.
std::vector<QString> itemsOf(QComboBox* box) {
    std::vector<QString> texts;
    for (int i = 0; i < box->count(); ++i) {
        if (!box->itemText(i).isEmpty()) {
            texts.push_back(box->itemText(i));
        }
    }
    return texts;
}

bool enabledItem(QComboBox* box, int index) {
    return qobject_cast<QStandardItemModel*>(box->model())->item(index)->isEnabled();
}

/// @brief Chooses an item by its text, as the user does.
void choose(QComboBox* box, const QString& text) {
    const int index = box->findText(text);
    REQUIRE(index >= 0);
    box->setCurrentIndex(index);
}

/// @brief Finds the spin box inside a group box.
QSpinBox* spinIn(ExportDialog& dialog, const QString& title) {
    for (auto* group : dialog.findChildren<QGroupBox*>()) {
        if (group->title() == title) {
            return group->findChild<QSpinBox*>();
        }
    }
    return nullptr;
}

const QString webPreset = QStringLiteral("Web 2048 px");

constexpr const char* newerFile = R"({"arraw": 99, "exportPresets": [{"name": "Future"}]})";

} // namespace

TEST_CASE("Without a store there is no picker", "[app][dialog][export][presets]") {
    ExportDialog dialog(ExportSettings{}, frame);
    CHECK(presetBoxOf(dialog) == nullptr);
    CHECK_FALSE(dialog.chosenPreset().has_value());
}

TEST_CASE("The picker lists presets in order, built-ins first, the unusable ones disabled",
          "[app][dialog][export][presets]") {
    Presets p;
    p.write(R"({"arraw": 1, "exportPresets": [{"name": "zeta"}, {"name": "Alpha"},
              {"name": "Future", "format": "avif"}]})");
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    auto* box = presetBoxOf(dialog);
    REQUIRE(box != nullptr);

    std::vector<QString> expected{QObject::tr("No preset")};
    for (const ExportPreset& builtIn : builtInExportPresets()) {
        expected.push_back(QString::fromStdString(builtIn.name));
    }
    for (const char* name : {"Alpha", "zeta", "Future"}) {
        expected.push_back(name);
    }
    CHECK(itemsOf(box) == expected);
    CHECK(box->currentIndex() == 0);
    CHECK_FALSE(dialog.chosenPreset().has_value());

    const int future = box->findText("Future");
    CHECK_FALSE(enabledItem(box, future));
    CHECK_FALSE(box->itemData(future, Qt::ToolTipRole).toString().isEmpty());
    CHECK(enabledItem(box, box->findText("Alpha")));
    CHECK(enabledItem(box, box->findText(webPreset)));
}

TEST_CASE("A built-in cannot be updated, renamed or deleted, but Save As copies it",
          "[app][dialog][export][presets]") {
    Presets p;
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    auto* box = presetBoxOf(dialog);
    choose(box, webPreset);
    CHECK(dialog.chosenPreset() == "Web 2048 px");
    CHECK_FALSE(actionOf(dialog, "presetUpdate")->isEnabled());
    CHECK_FALSE(actionOf(dialog, "presetRename")->isEnabled());
    CHECK_FALSE(actionOf(dialog, "presetDelete")->isEnabled());

    spinIn(dialog, "JPEG Quality")->setValue(60);
    CHECK(box->currentText() == webPreset + " (modified)");
    CHECK_FALSE(actionOf(dialog, "presetUpdate")->isEnabled());

    CHECK(saveAsOf(dialog)->isEnabled());
    p.typedName = QStringLiteral("My web");
    saveAsOf(dialog)->click();
    CHECK(p.warnings.empty());
    CHECK(p.nameInitials == std::vector<QString>{webPreset});
    CHECK(dialog.chosenPreset() == "My web");
    CHECK(box->currentText() == "My web");
    const auto saved = p.store.load("My web");
    REQUIRE(saved.has_value());
    CHECK(saved->settings.quality == 60);
    CHECK(saved->settings.width == 2048);
    // The built-in is as it was.
    CHECK(p.store.load("Web 2048 px")->settings.quality == 85);
    CHECK(actionOf(dialog, "presetDelete")->isEnabled());
}

TEST_CASE("Choosing a preset sets every control", "[app][dialog][export][presets]") {
    Presets p;
    const ExportSettings all{.format = ImageFileFormat::Tiff,
                             .encoding = NamedEncoding::DisplayP3,
                             .sixteenBit = true,
                             .resize = true,
                             .width = 700,
                             .height = 500,
                             .allowEnlarging = true,
                             .quality = 70,
                             .sharpening = 40,
                             .captureInfo = false,
                             .location = true,
                             .descriptive = false};
    p.save("Everything", all);
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    choose(presetBoxOf(dialog), "Everything");
    CHECK(canonical(dialog.settings()) == canonical(all));
    CHECK(presetBoxOf(dialog)->currentText() == "Everything");

    // Then a JPEG one: 16 bits and the box fall away, and nothing is marked.
    choose(presetBoxOf(dialog), "Full size JPEG");
    CHECK(canonical(dialog.settings()) == canonical(builtInExportPresets()[2].settings));
    CHECK(presetBoxOf(dialog)->currentText() == "Full size JPEG");
}

TEST_CASE("Changing a control marks the preset modified; changing it back clears the mark",
          "[app][dialog][export][presets]") {
    Presets p;
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    auto* box = presetBoxOf(dialog);
    choose(box, webPreset);
    CHECK(box->currentText() == webPreset);
    auto* quality = spinIn(dialog, "JPEG Quality");
    quality->setValue(50);
    CHECK(box->currentText() == webPreset + " (modified)");
    CHECK(dialog.chosenPreset() == "Web 2048 px");
    quality->setValue(85);
    CHECK(box->currentText() == webPreset);
    // Nothing switches to another preset by itself.
    CHECK(dialog.chosenPreset() == "Web 2048 px");
}

TEST_CASE("The dialog opens on the remembered preset, modified when the last settings differ",
          "[app][dialog][export][presets]") {
    Presets p;
    const ExportSettings mine{.quality = 33, .sharpening = 10};
    p.save("Mine", mine);
    {
        ExportDialog dialog(mine, frame, &p.store, std::string("mine"));
        CHECK(dialog.chosenPreset() == "Mine");
        CHECK(presetBoxOf(dialog)->currentText() == "Mine");
        CHECK(dialog.settings() == mine);
    }
    {
        ExportSettings other = mine;
        other.quality = 77;
        ExportDialog dialog(other, frame, &p.store, std::string("Mine"));
        CHECK(presetBoxOf(dialog)->currentText() == "Mine (modified)");
        CHECK(dialog.settings() == other);
        CHECK(actionOf(dialog, "presetUpdate")->isEnabled());
    }
}

TEST_CASE("A remembered preset that is gone shows No preset", "[app][dialog][export][presets]") {
    Presets p;
    ExportDialog dialog(ExportSettings{}, frame, &p.store, std::string("Vanished"));
    CHECK(presetBoxOf(dialog)->currentIndex() == 0);
    CHECK(presetBoxOf(dialog)->currentText() == "No preset");
    CHECK_FALSE(dialog.chosenPreset().has_value());
}

TEST_CASE("Save As writes the file, and a name that folds alike asks before replacing",
          "[app][dialog][export][presets]") {
    Presets p;
    ExportDialog dialog(ExportSettings{.quality = 41}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    CHECK_FALSE(fs::exists(p.store.file()));

    p.typedName = QStringLiteral("  Client  ");
    saveAsOf(dialog)->click();
    CHECK(fs::exists(p.store.file()));
    CHECK(p.store.load("Client")->settings.quality == 41);
    CHECK(dialog.chosenPreset() == "Client");
    CHECK(p.asked.empty());

    spinIn(dialog, "JPEG Quality")->setValue(42);
    p.typedName = QStringLiteral("CLIENT");
    p.confirmed = false;
    saveAsOf(dialog)->click();
    REQUIRE(p.asked.size() == 1);
    CHECK(p.asked[0].contains("Client"));
    CHECK(p.store.load("Client")->settings.quality == 41);
    CHECK(p.store.load("Client")->name == "Client");

    p.confirmed = true;
    saveAsOf(dialog)->click();
    CHECK(p.asked.size() == 2);
    CHECK(p.store.load("client")->settings.quality == 42);
    CHECK(p.store.load("client")->name == "CLIENT");
    CHECK(dialog.chosenPreset() == "CLIENT");

    // A name that is not valid, or a cancelled prompt, changes nothing.
    p.typedName = QStringLiteral("   ");
    saveAsOf(dialog)->click();
    CHECK(p.warnings.size() == 1);
    p.typedName.reset();
    saveAsOf(dialog)->click();
    CHECK(p.warnings.size() == 1);
    CHECK(p.store.read().presets.size() == builtInExportPresets().size() + 1);
}

TEST_CASE("Update, Rename and Delete act on the file at once", "[app][dialog][export][presets]") {
    Presets p;
    p.save("Mine", {.quality = 30});
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    auto* box = presetBoxOf(dialog);
    choose(box, "Mine");
    CHECK_FALSE(actionOf(dialog, "presetUpdate")->isEnabled()); // Not modified yet.

    spinIn(dialog, "JPEG Quality")->setValue(55);
    REQUIRE(actionOf(dialog, "presetUpdate")->isEnabled());
    CHECK(actionOf(dialog, "presetUpdate")->text() == "Update \"Mine\"");
    actionOf(dialog, "presetUpdate")->trigger();
    CHECK(p.store.load("Mine")->settings.quality == 55);
    CHECK(box->currentText() == "Mine");
    CHECK_FALSE(actionOf(dialog, "presetUpdate")->isEnabled());

    p.typedName = QStringLiteral("Theirs");
    actionOf(dialog, "presetRename")->trigger();
    CHECK(p.nameInitials.back() == "Mine");
    CHECK_FALSE(p.store.load("Mine").has_value());
    CHECK(p.store.load("Theirs")->settings.quality == 55);
    CHECK(dialog.chosenPreset() == "Theirs");
    CHECK(box->currentText() == "Theirs");

    p.confirmed = false;
    actionOf(dialog, "presetDelete")->trigger();
    CHECK(p.store.load("Theirs").has_value());
    p.confirmed = true;
    actionOf(dialog, "presetDelete")->trigger();
    CHECK_FALSE(p.store.load("Theirs").has_value());
    CHECK_FALSE(dialog.chosenPreset().has_value());
    CHECK(box->currentIndex() == 0);
    CHECK(dialog.settings().quality == 55); // The controls stay as they were.
    CHECK(p.warnings.empty());
}

TEST_CASE("A change that fails is reported and leaves the dialog as it was",
          "[app][dialog][export][presets]") {
    Presets p;
    p.save("Mine", {.quality = 30});
    p.save("Other", {.quality = 31});
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    choose(presetBoxOf(dialog), "Mine");
    p.typedName = QStringLiteral("other");
    actionOf(dialog, "presetRename")->trigger();
    REQUIRE(p.warnings.size() == 1);
    CHECK(dialog.chosenPreset() == "Mine");
    CHECK(p.store.load("Mine").has_value());

    // Save As over a built-in is refused by the store.
    p.typedName = QStringLiteral("web 2048 PX");
    saveAsOf(dialog)->click();
    CHECK(p.warnings.size() == 2);
    CHECK(p.store.load("Web 2048 px")->settings.quality == 85);
}

TEST_CASE("A newer file disables every write, with the reason", "[app][dialog][export][presets]") {
    Presets p;
    p.write(newerFile);
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    auto* box = presetBoxOf(dialog);
    CHECK_FALSE(saveAsOf(dialog)->isEnabled());
    CHECK(saveAsOf(dialog)->toolTip().contains("newer"));
    // A preset of the newer file is used as far as it is understood; writes stay off.
    choose(box, "Future");
    for (const char* name : {"presetUpdate", "presetRename", "presetDelete"}) {
        CHECK_FALSE(actionOf(dialog, name)->isEnabled());
        CHECK(actionOf(dialog, name)->toolTip().contains("newer"));
    }
    CHECK_FALSE(actionOf(dialog, "presetSetAside")->isVisible());
}

TEST_CASE("An unreadable file offers Set Aside, which moves it after confirmation",
          "[app][dialog][export][presets]") {
    Presets p;
    p.write("this is not json");
    ExportDialog dialog(ExportSettings{}, frame, &p.store);
    dialog.setPrompts(p.prompts());
    CHECK_FALSE(saveAsOf(dialog)->isEnabled());
    CHECK_FALSE(actionOf(dialog, "presetUpdate")->isEnabled());
    auto* aside = actionOf(dialog, "presetSetAside");
    REQUIRE(aside->isVisible());

    p.confirmed = false;
    aside->trigger();
    REQUIRE(p.asked.size() == 1);
    CHECK(p.asked[0].contains("export-presets.json"));
    CHECK(fs::exists(p.store.file()));

    p.confirmed = true;
    aside->trigger();
    CHECK_FALSE(fs::exists(p.store.file()));
    int moved = 0;
    for (const auto& entry : fs::directory_iterator(p.directory.path())) {
        moved += entry.path().filename().string().starts_with("export-presets.unreadable-");
    }
    CHECK(moved == 1);
    CHECK(saveAsOf(dialog)->isEnabled());
    CHECK_FALSE(aside->isVisible());
    CHECK(p.warnings.empty());
}
