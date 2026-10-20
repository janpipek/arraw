#include "AppSettings.h"
#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/DevelopPanel.h"
#include "ui/ExportDialog.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"
#include "ui/StatusLine.h"

#include <ConfigDirectory.h>
#include <ExportPresets.h>
#include <ImageExport.h>
#include <Photo.h>
#include <Sidecar.h>

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QItemSelectionModel>
#include <QListView>
#include <QMenu>
#include <QSettings>
#include <QStringList>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

using namespace arraw;
using namespace arraw::app;

/// Exporting shots from the film strip, with the dialogs answered by the test.

namespace {

namespace fs = std::filesystem;

/// Points the configuration directory at a directory for as long as it lives.
class ConfigScope {
public:
    explicit ConfigScope(const fs::path& directory)
        : previous_(qgetenv(configDirectoryVariable)),
          wasSet_(qEnvironmentVariableIsSet(configDirectoryVariable)) {
        qputenv(configDirectoryVariable, QString::fromStdU16String(directory.u16string()).toUtf8());
    }
    ~ConfigScope() {
        if (wasSet_) {
            qputenv(configDirectoryVariable, previous_);
        } else {
            qunsetenv(configDirectoryVariable);
        }
    }
    ConfigScope(const ConfigScope&) = delete;
    ConfigScope& operator=(const ConfigScope&) = delete;

private:
    QByteArray previous_;
    bool wasSet_;
};

/// A window over three photographs, two of them in the folder shown and one in a subfolder,
/// with the export's questions answered by the fields below.
struct Window {
    test::TempDir config; ///< Where the presets are, instead of the user's.
    ConfigScope scope{config.path()};
    ExportPresetStore presets{config.path()};
    test::TempDir folder;
    test::TempDir out;
    DebugLog debugLog;
    MainWindow window{debugLog};

    ExportSettings answer;          ///< Settings the dialog "gives".
    bool settingsCancelled = false; ///< Whether the settings dialog is cancelled.
    fs::path chosenFolder;          ///< Folder the picker "gives"; empty cancels.
    MainWindow::ExistingChoice choice = MainWindow::ExistingChoice::Cancel;
    std::optional<std::size_t> askedExisting; ///< Count the question was asked with.
    std::optional<fs::path> startedAt;        ///< Where the picker started.
    int folderAsked = 0;                      ///< Times the picker was asked.

    Window() {
        QSettings().remove("export");
        const fs::path fixtures(ARRAW_TEST_DATA_DIR);
        addShot(fixtures / "preview-32x24.dng", "a.dng", 0.5F);
        addShot(fixtures / "bayer-32x24.dng", "b.dng", 1.0F);
        fs::create_directories(folder.file("sub"));
        addShot(fixtures / "bayer-32x24.dng", "sub/a.dng", 1.5F);
        answer.format = ImageFileFormat::Png;
        chosenFolder = out.path();
        window.setExportPrompts(
            {.settings = [this](const ExportSettings&, ImageSize) -> std::optional<ExportSettings> {
                 if (settingsCancelled) {
                     return std::nullopt;
                 }
                 return answer;
             },
             .folder =
                 [this](const fs::path& start) {
                     ++folderAsked;
                     startedAt = start;
                     return chosenFolder;
                 },
             .existing =
                 [this](std::size_t count) {
                     askedExisting = count;
                     return choice;
                 }});
        window.openInitialPath(folder.file("a.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    void addShot(const fs::path& from, const char* name, float exposure) const {
        const fs::path path = folder.file(name);
        fs::copy_file(from, path);
        const Photo bare = openPhoto(path);
        DevelopState state = bare.state();
        state.settings.tone.exposure = exposure;
        writeSidecar(Photo(path, bare.metadata(), state));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] QString message() const {
        return window.findChild<StatusLine*>()->message();
    }

    [[nodiscard]] QString path(const char* name) const {
        return QString::fromStdU16String(folder.file(name).u16string());
    }

    /// Exports and waits for the summary.
    void exportShots(const QStringList& shots) {
        window.findChild<FilmStrip*>()->exportRequested(shots);
        REQUIRE(QTest::qWaitFor(
            [this] { return message().startsWith("Exported") || message().startsWith("Nothing"); },
            20000));
    }
};

void touch(const fs::path& path) {
    std::ofstream(path) << "x";
}

} // namespace

TEST_CASE("Exporting several shots writes one file each into the chosen folder",
          "[app][window][export]") {
    Window w;
    // Same stem, different folders: the second is numbered.
    w.exportShots({w.path("a.dng"), w.path("b.dng"), w.path("sub/a.dng")});
    CHECK(w.message() == "Exported 3 of 3 files");
    CHECK(fs::exists(w.out.file("a.png")));
    CHECK(fs::exists(w.out.file("b.png")));
    CHECK(fs::exists(w.out.file("a_2.png")));
    CHECK_FALSE(w.askedExisting.has_value());
    CHECK(QSettings().value("export/lastFolder").toString() ==
          QString::fromStdU16String(w.out.path().u16string()));
}

TEST_CASE("The folder picker starts at the last export folder", "[app][window][export]") {
    Window w;
    w.exportShots({w.path("b.dng")});
    REQUIRE(w.startedAt.has_value());
    CHECK(*w.startedAt == w.folder.path()); // First time: beside the shot.
    w.startedAt.reset();
    w.exportShots({w.path("b.dng")});
    CHECK(*w.startedAt == w.out.path());
}

TEST_CASE("Exports use the saved state, not the session's unsaved edits", "[app][window][export]") {
    Window w;
    w.exportShots({w.path("a.dng")});
    const QImage saved(QString::fromStdU16String(w.out.file("a.png").u16string()));
    REQUIRE_FALSE(saved.isNull());
    CHECK_FALSE(w.message().contains("unsaved"));

    // The open photograph, a.dng, gets an edit that is not saved.
    for (auto* row : w.window.findChildren<SettingSlider*>()) {
        if (row->key() == "exposure") {
            row->findChild<QDoubleSpinBox*>()->setValue(3.0);
        }
    }
    w.window.findChild<DevelopPanel*>()->finishPendingEdit();
    w.choice = MainWindow::ExistingChoice::ReplaceAll;
    w.exportShots({w.path("a.dng")});
    CHECK(w.message().contains("a.dng"));
    CHECK(w.message().contains("unsaved edits are not included"));
    CHECK(QImage(QString::fromStdU16String(w.out.file("a.png").u16string())) == saved);
}

TEST_CASE("Existing files: skip them, replace them, or cancel everything",
          "[app][window][export]") {
    Window w;
    touch(w.out.file("a.png"));
    const auto size = [&] { return fs::file_size(w.out.file("a.png")); };

    w.choice = MainWindow::ExistingChoice::Cancel;
    w.window.findChild<FilmStrip*>()->exportRequested({w.path("a.dng"), w.path("b.dng")});
    QTest::qWait(300);
    CHECK(w.askedExisting == 1);
    CHECK(size() == 1);
    CHECK_FALSE(fs::exists(w.out.file("b.png")));

    w.choice = MainWindow::ExistingChoice::SkipExisting;
    w.exportShots({w.path("a.dng"), w.path("b.dng")});
    CHECK(w.message() == "Exported 1 of 1 files, 1 skipped");
    CHECK(size() == 1);
    CHECK(fs::exists(w.out.file("b.png")));

    w.choice = MainWindow::ExistingChoice::ReplaceAll;
    w.exportShots({w.path("a.dng"), w.path("b.dng")});
    CHECK(w.askedExisting == 2);
    CHECK(w.message() == "Exported 2 of 2 files");
    CHECK(size() > 1);
}

TEST_CASE("Cancelling a dialog exports nothing", "[app][window][export]") {
    Window w;
    w.settingsCancelled = true;
    w.window.findChild<FilmStrip*>()->exportRequested({w.path("b.dng")});
    QTest::qWait(200);
    CHECK(w.folderAsked == 0);
    w.settingsCancelled = false;
    w.chosenFolder.clear();
    w.window.findChild<FilmStrip*>()->exportRequested({w.path("b.dng")});
    QTest::qWait(200);
    CHECK(w.folderAsked == 1);
    CHECK(fs::is_empty(w.out.path()));
}

TEST_CASE("A photograph is never overwritten by its own export", "[app][window][export]") {
    Window w;
    w.answer.format = ImageFileFormat::Jpeg;
    // Export a JPEG shot into its own folder under the same stem.
    const fs::path jpeg = w.folder.file("c.jpg");
    fs::copy_file(fs::path(ARRAW_TEST_DATA_DIR) / "testcard-61x41-srgb8.png", jpeg);
    w.chosenFolder = w.folder.path();
    w.exportShots({QString::fromStdU16String(jpeg.u16string())});
    CHECK(w.message() == "Exported 1 of 1 files");
    CHECK(fs::exists(w.folder.file("c_2.jpg")));
}

TEST_CASE("A failing file is counted and the others are still written", "[app][window][export]") {
    Window w;
    touch(w.folder.file("broken.dng"));
    w.exportShots({w.path("b.dng"), w.path("broken.dng")});
    CHECK(w.message() == "Exported 1 of 2 files, 1 failed");
    CHECK(fs::exists(w.out.file("b.png")));
    CHECK_FALSE(fs::exists(w.out.file("broken.png")));
}

namespace {

/// @brief Lets a function answer the next export dialog, which is modal.
///
/// Polls until an export dialog is the modal widget, then calls the function with it; the
/// function accepts or rejects it.
class DialogAnswer {
public:
    explicit DialogAnswer(std::function<void(ExportDialog&)> answer) : answer_(std::move(answer)) {
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
            if (auto* dialog = qobject_cast<ExportDialog*>(QApplication::activeModalWidget())) {
                timer_.stop();
                ++answered;
                answer_(*dialog);
            }
        });
        timer_.start(10);
    }

    int answered = 0; ///< Times the function was called.

private:
    std::function<void(ExportDialog&)> answer_;
    QTimer timer_;
};

/// @brief Opens the strip's context menu on a row and triggers an item of its preset submenu.
/// @return Whether the item was found and triggered.
bool exportWithPresetFromStrip(Window& w, int row, const QString& preset) {
    auto* list = w.window.findChild<FilmStrip*>()->findChild<QListView*>();
    bool chosen = false;
    QTimer pick;
    QObject::connect(&pick, &QTimer::timeout, &pick, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        for (QAction* action : menu->actions()) {
            if (action->text() == "Export with Preset" && action->menu() != nullptr) {
                // The submenu is read as it is about to show.
                emit action->menu()->aboutToShow();
                for (QAction* item : action->menu()->actions()) {
                    if (item->text() == preset && item->isEnabled()) {
                        chosen = true;
                        item->trigger();
                    }
                }
            }
        }
        menu->close();
    });
    pick.start(10);
    const QPoint where = list->visualRect(list->model()->index(row, 0)).center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, where, list->viewport()->mapToGlobal(where));
    QApplication::sendEvent(list->viewport(), &event);
    return chosen;
}

} // namespace

TEST_CASE("Export with Preset exports several shots without the settings dialog",
          "[app][window][export][presets]") {
    Window w;
    w.presets.save(
        {.name = "Tiny",
         .settings = {.format = ImageFileFormat::Png, .resize = true, .width = 16, .height = 16}});
    w.settingsCancelled = true; // The dialog, if asked, would export nothing.
    QSettings store;
    const ExportSettings before = restoreExportSettings(store);

    auto* list = w.window.findChild<FilmStrip*>()->findChild<QListView*>();
    REQUIRE(QTest::qWaitFor([&] { return list->model()->rowCount() >= 2; }, 10000));
    list->selectionModel()->select(list->model()->index(0, 0), QItemSelectionModel::Select);
    list->selectionModel()->select(list->model()->index(1, 0), QItemSelectionModel::Select);
    REQUIRE(exportWithPresetFromStrip(w, 0, "Tiny"));
    REQUIRE(QTest::qWaitFor([&] { return w.message().startsWith("Exported"); }, 20000));

    CHECK(w.message() == "Exported 2 of 2 files");
    CHECK(w.folderAsked == 1);
    const QImage written(QString::fromStdU16String(w.out.file("a.png").u16string()));
    REQUIRE_FALSE(written.isNull());
    CHECK(written.width() == 16);
    CHECK(fs::exists(w.out.file("b.png")));
    // The last used settings and the remembered preset are as they were.
    QSettings after;
    CHECK(restoreExportSettings(after) == before);
    CHECK_FALSE(after.contains("export/preset"));
}

TEST_CASE("Export with Preset lists unusable presets as disabled",
          "[app][window][export][presets]") {
    Window w;
    std::ofstream(w.presets.file(), std::ios::binary)
        << R"({"arraw": 1, "exportPresets": [{"name": "Tiny", "format": "png"},
                {"name": "Future", "format": "avif"}]})";
    auto* menu = w.window.findChild<QMenu*>("exportPresetMenu");
    REQUIRE(menu != nullptr);
    CHECK(menu->menuAction()->isEnabled());
    emit menu->aboutToShow();
    const auto find = [&](const QString& text) -> QAction* {
        for (QAction* action : menu->actions()) {
            if (action->text() == text) {
                return action;
            }
        }
        return nullptr;
    };
    REQUIRE(find("Tiny") != nullptr);
    CHECK(find("Tiny")->isEnabled());
    REQUIRE(find("Future") != nullptr);
    CHECK_FALSE(find("Future")->isEnabled());
    CHECK_FALSE(find("Future")->toolTip().isEmpty());
    CHECK(find("Web 2048 px")->isEnabled());
    CHECK(menu->actions().size() == builtInExportPresets().size() + 2);

    // Choosing one goes to the save prompt, not to the settings dialog.
    bool saveAsked = false;
    bool settingsAsked = false;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &watch, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (auto* dialog = qobject_cast<ExportDialog*>(modal)) {
            settingsAsked = true;
            dialog->reject();
        } else if (auto* file = qobject_cast<QFileDialog*>(modal)) {
            saveAsked = true;
            file->reject();
        }
    });
    watch.start(10);
    find("Tiny")->trigger();
    CHECK(saveAsked);
    CHECK_FALSE(settingsAsked);
}

TEST_CASE("The chosen preset's name is remembered for the next export",
          "[app][window][export][presets]") {
    Window w;
    w.presets.save(
        {.name = "Tiny",
         .settings = {.format = ImageFileFormat::Png, .resize = true, .width = 16, .height = 16}});
    // The real dialog, answered by the test.
    w.window.setExportPrompts(
        {.settings = {},
         .folder = [&](const fs::path&) { return w.out.path(); },
         .existing = [](std::size_t) { return MainWindow::ExistingChoice::ReplaceAll; }});
    {
        DialogAnswer answer([](ExportDialog& dialog) {
            auto* box = dialog.findChild<QComboBox*>("presetBox");
            box->setCurrentIndex(box->findText("Tiny"));
            dialog.accept();
        });
        w.window.findChild<FilmStrip*>()->exportRequested({w.path("b.dng")});
        REQUIRE(QTest::qWaitFor([&] { return w.message().startsWith("Exported"); }, 20000));
        CHECK(answer.answered == 1);
    }
    CHECK(QSettings().value("export/preset").toString() == "Tiny");

    std::optional<std::string> shown;
    std::optional<ExportSettings> shownSettings;
    {
        DialogAnswer answer([&](ExportDialog& dialog) {
            shown = dialog.chosenPreset();
            shownSettings = dialog.settings();
            dialog.reject();
        });
        w.window.findChild<FilmStrip*>()->exportRequested({w.path("b.dng")});
        REQUIRE(QTest::qWaitFor([&] { return answer.answered == 1; }, 10000));
    }
    CHECK(shown == "Tiny");
    REQUIRE(shownSettings.has_value());
    CHECK(shownSettings->format == ImageFileFormat::Png);

    // Choosing No preset forgets it.
    {
        DialogAnswer answer([](ExportDialog& dialog) {
            dialog.findChild<QComboBox*>("presetBox")->setCurrentIndex(0);
            dialog.accept();
        });
        w.window.findChild<FilmStrip*>()->exportRequested({w.path("b.dng")});
        REQUIRE(QTest::qWaitFor([&] { return answer.answered == 1; }, 10000));
        QTest::qWait(300);
    }
    CHECK_FALSE(QSettings().contains("export/preset"));
}
