#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"
#include "ui/StatusLine.h"

#include <ImageExport.h>
#include <Photo.h>
#include <Sidecar.h>

#include <QDoubleSpinBox>
#include <QImage>
#include <QSettings>
#include <QStringList>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>

using namespace arraw;
using namespace arraw::app;

/// Exporting shots from the film strip, with the dialogs answered by the test.

namespace {

namespace fs = std::filesystem;

/// A window over three photographs, two of them in the folder shown and one in a subfolder,
/// with the export's questions answered by the fields below.
struct Window {
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
