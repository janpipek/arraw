#include "DebugLog.h"
#include "PhotoLoader.h"
#include "support/TempDir.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"
#include "ui/StatusLine.h"

#include <ImageImport.h>
#include <Photo.h>
#include <Progress.h>
#include <Sidecar.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The Negative group and the re-decode it causes, in the window (ADR 049, plan step 7).

namespace {

/// Decodes as the window does, counting, and optionally holding each non-default decode until
/// released or cancelled.
struct CountingDecoder {
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<bool> hold{false};
    /// Whether a decode other than the default one fails.
    std::atomic<bool> fail{false};
    std::mutex mutex;
    std::vector<Demosaic> delivered;

    [[nodiscard]] PhotoLoader::Decoder decoder() {
        return [this, real = PhotoLoader::imageDecoder()](const std::filesystem::path& path,
                                                          const DecodeOptions& options,
                                                          ProgressChannel& channel) {
            ++started;
            while (hold && options.demosaic != Demosaic::Ahd) {
                if (channel.cancelled()) {
                    throw Cancelled();
                }
                QThread::msleep(1);
            }
            if (fail && options.demosaic != Demosaic::Ahd) {
                throw std::runtime_error("injected failure");
            }
            ImageBuffer buffer = real(path, options, channel);
            {
                const std::scoped_lock lock(mutex);
                delivered.push_back(options.demosaic);
            }
            ++finished;
            return buffer;
        };
    }

    [[nodiscard]] Demosaic last() {
        const std::scoped_lock lock(mutex);
        return delivered.back();
    }
};

/// A window over a folder of a photograph with Bayer detail, one without, and a PNG.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    CountingDecoder counter;
    MainWindow window{debugLog, nullptr, counter.decoder()};

    explicit Window(Demosaic first = Demosaic::Ahd, bool open = true) {
        QSettings().remove("copySettings/sections");
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        std::filesystem::copy_file(fixtures / "bayer-detail-32x24.dng", folder.file("a.dng"));
        std::filesystem::copy_file(fixtures / "bayer-32x24.dng", folder.file("b.dng"));
        std::filesystem::copy_file(fixtures / "testcard-61x41-srgb8.png", folder.file("c.png"));
        std::filesystem::copy_file(fixtures / "linear-32x24-neutral.dng", folder.file("d.dng"));
        const Photo bare = openPhoto(folder.file("a.dng"));
        writeSidecar(
            Photo(folder.file("a.dng"), bare.metadata(), NegativeSettings{first}, bare.state()));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        if (open) {
            this->open("a.dng");
        }
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] StatusLine& status() const {
        return *window.findChild<StatusLine*>();
    }

    [[nodiscard]] QComboBox& combo() const {
        return *window.findChild<QComboBox*>("demosaicCombo");
    }

    [[nodiscard]] QAction& action(const QString& name) const {
        for (QAction* candidate : window.findChildren<QAction*>()) {
            if (candidate->objectName() == name) {
                return *candidate;
            }
        }
        FAIL("no action " << name.toStdString());
        throw;
    }

    [[nodiscard]] DevelopPanel& panel() const {
        return *window.findChild<DevelopPanel*>();
    }

    /// Opens a shot and waits for its first render.
    void open(const char* name) const {
        emit strip().activationRequested(QString::fromStdU16String(folder.file(name).u16string()));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    /// Waits until the decodes asked for have all finished and the window is not waiting.
    [[nodiscard]] bool settled(int finished) const {
        return QTest::qWaitFor(
            [&] { return counter.finished == finished && status().message().isEmpty(); }, 20000);
    }

    void choose(Demosaic demosaic) const {
        const int index = combo().findData(static_cast<int>(demosaic));
        REQUIRE(index >= 0);
        emit combo().activated(index);
    }

    [[nodiscard]] Demosaic shown() const {
        return static_cast<Demosaic>(combo().currentData().toInt());
    }

    [[nodiscard]] QString pathOf(const char* name) const {
        return QString::fromStdU16String(folder.file(name).u16string());
    }

    /// Clicks a row of the history list (the newest is row 0).
    void clickHistory(int row) const {
        auto* list = window.findChild<QListView*>("historyList");
        REQUIRE(list != nullptr);
        emit list->clicked(list->model()->index(row, 0));
    }

    [[nodiscard]] int historyRows() const {
        return window.findChild<QListView*>("historyList")->model()->rowCount();
    }

    void setExposure(double value) const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                row->findChild<QDoubleSpinBox*>()->setValue(value);
                panel().finishPendingEdit();
                return;
            }
        }
        FAIL("no exposure row");
    }

    /// Triggers Copy and accepts its dialog.
    void copy() const {
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                dialog->accept();
            }
        });
        action("copySettingsAction").trigger();
    }
};

} // namespace

TEST_CASE("Opening decodes once, and no develop edit decodes again", "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    CHECK(w.counter.started == 1);
    CHECK(w.shown() == Demosaic::Ahd);

    w.setExposure(0.5);
    w.setExposure(-0.5);
    w.action("undoAction").trigger();
    w.action("undoAction").trigger();
    w.action("redoAction").trigger();
    QTest::qWait(100);
    CHECK(w.counter.started == 1);
}

TEST_CASE("Choosing a demosaic decodes again, and undo decodes back", "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));

    w.choose(Demosaic::Vng);
    REQUIRE(w.settled(2));
    CHECK(w.counter.last() == Demosaic::Vng);
    CHECK(w.shown() == Demosaic::Vng);
    CHECK_FALSE(w.view().wholeFrameImage().isNull());

    // A develop edit after it decodes nothing.
    w.setExposure(0.25);
    QTest::qWait(100);
    CHECK(w.counter.started == 2);

    w.action("undoAction").trigger(); // The exposure.
    QTest::qWait(100);
    CHECK(w.counter.started == 2);
    w.action("undoAction").trigger(); // The demosaic.
    REQUIRE(w.settled(3));
    CHECK(w.counter.last() == Demosaic::Ahd);
    CHECK(w.shown() == Demosaic::Ahd);
}

TEST_CASE("Two quick choices deliver one decode, and the old picture stays up",
          "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    w.counter.hold = true;

    w.choose(Demosaic::Vng);
    REQUIRE(QTest::qWaitFor([&] { return w.counter.started == 2; }, 20000));
    CHECK(w.status().message().contains("VNG"));
    CHECK_FALSE(w.view().wholeFrameImage().isNull());
    w.choose(Demosaic::Ppg);
    CHECK(w.status().message().contains("PPG"));
    // Edits go on against the old pixels, and decode nothing.
    w.setExposure(0.5);
    w.counter.hold = false;

    REQUIRE(w.settled(2));
    CHECK(w.counter.started == 3);
    CHECK(w.counter.last() == Demosaic::Ppg);
    CHECK(w.shown() == Demosaic::Ppg);
}

TEST_CASE("Going back before the decode lands cancels it", "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    w.counter.hold = true;
    w.choose(Demosaic::Vng);
    REQUIRE(QTest::qWaitFor([&] { return w.counter.started == 2; }, 20000));
    w.action("undoAction").trigger();
    REQUIRE(QTest::qWaitFor([&] { return w.status().message().isEmpty(); }, 20000));
    QTest::qWait(100);
    CHECK(w.counter.finished == 1);
    CHECK(w.counter.started == 2);
    CHECK(w.shown() == Demosaic::Ahd);
}

TEST_CASE("A paste carries the demosaic in one step, and one without it does not",
          "[app][window][negative][copy]") {
    Window w(Demosaic::Vng);
    REQUIRE(w.settled(1));
    CHECK(w.counter.last() == Demosaic::Vng);
    w.copy();

    w.open("b.dng");
    REQUIRE(w.settled(2));
    CHECK(w.shown() == Demosaic::Ahd);
    w.action("pasteSettingsAction").trigger();
    REQUIRE(w.settled(3));
    CHECK(w.counter.last() == Demosaic::Vng);
    CHECK(w.shown() == Demosaic::Vng);
    // One step: a single undo returns both.
    w.action("undoAction").trigger();
    REQUIRE(w.settled(4));
    CHECK(w.shown() == Demosaic::Ahd);
    CHECK_FALSE(w.action("undoAction").isEnabled());

    // The same, without the Demosaic section.
    QSettings().setValue("copySettings/sections", QStringList{"exposure"});
    QSettings().setValue("copySettings/demosaicMigrated", true);
    w.action("redoAction").trigger(); // Back to the pasted VNG, which decodes.
    REQUIRE(w.settled(5));
    w.action("undoAction").trigger();
    REQUIRE(w.settled(6));
    const int before = w.counter.started;
    w.open("a.dng");
    REQUIRE(w.settled(before + 1));
    w.copy();
    w.open("b.dng");
    REQUIRE(w.settled(before + 2));
    w.action("pasteSettingsAction").trigger();
    QTest::qWait(150);
    CHECK(w.counter.started == before + 2);
    CHECK(w.shown() == Demosaic::Ahd);
}

TEST_CASE("A paste of the lens switches decodes nothing, and undo and redo decode nothing",
          "[app][window][negative][copy][lens]") {
    Window w(Demosaic::Ahd, false);
    {
        // The photograph being copied from has a correction on.
        const Photo bare = openPhoto(w.folder.file("a.dng"));
        NegativeSettings negative = bare.negative();
        negative.lens.distortion = true;
        writeSidecar(bare.with(negative));
    }
    w.open("a.dng");
    REQUIRE(w.settled(1));
    w.copy();

    w.open("b.dng");
    REQUIRE(w.settled(2));
    const int before = w.counter.started;
    const int rows = w.historyRows();
    w.action("pasteSettingsAction").trigger();
    QTest::qWait(150);
    // One step, and a lens edit is not a decode (ADR 050).
    CHECK(w.historyRows() == rows + 1);
    CHECK(w.counter.started == before);
    CHECK(w.status().message().isEmpty());

    w.action("undoAction").trigger();
    QTest::qWait(100);
    CHECK(w.historyRows() == rows + 1);
    CHECK(w.counter.started == before);
    w.action("redoAction").trigger();
    QTest::qWait(100);
    CHECK(w.counter.started == before);
}

TEST_CASE("The Negative group is off for a file with nothing to demosaic",
          "[app][window][negative]") {
    Window w(Demosaic::Ahd, false);
    w.open("c.png");
    CHECK_FALSE(w.combo().isEnabled());
    CHECK_FALSE(w.combo().toolTip().isEmpty());
    auto* note = w.window.findChild<QLabel*>("negativeNote");
    REQUIRE(note != nullptr);
    CHECK_FALSE(note->isHidden());

    w.open("a.dng");
    CHECK(w.combo().isEnabled());
    CHECK(note->isHidden());
    // Not in the crop mode, which is one edit of its own.
    w.action("cropAction").trigger();
    REQUIRE(w.view().isCropMode());
    CHECK_FALSE(w.combo().isEnabled());
}

TEST_CASE("A sidecar asking for a demosaic of a file that has none decodes once",
          "[app][window][negative]") {
    Window w(Demosaic::Ahd, false);
    const Photo bare = openPhoto(w.folder.file("c.png"));
    writeSidecar(Photo(w.folder.file("c.png"), bare.metadata(),
                       NegativeSettings{.demosaic = Demosaic::Vng}, bare.state()));
    w.open("c.png");
    REQUIRE(w.settled(1));
    // A decode that cannot differ is not asked for again, however long the window waits.
    QTest::qWait(300);
    CHECK(w.counter.started == 1);
    CHECK(w.counter.finished == 1);
    CHECK(w.status().message().isEmpty());
}

TEST_CASE("A paste of the demosaic onto a file that has none decodes nothing",
          "[app][window][negative][copy]") {
    Window w(Demosaic::Vng);
    REQUIRE(w.settled(1));
    w.copy();

    w.open("d.dng"); // A linear DNG: already demosaiced.
    REQUIRE(w.settled(2));
    w.action("pasteSettingsAction").trigger();
    QTest::qWait(300);
    CHECK(w.counter.started == 2);
    CHECK(w.counter.finished == 2);
}

TEST_CASE("A failed re-decode is tried again when the choice comes back",
          "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    w.counter.fail = true;

    w.choose(Demosaic::Vng);
    REQUIRE(QTest::qWaitFor([&] { return w.status().message().contains("Cannot decode"); }, 20000));
    CHECK(w.counter.started == 2);
    CHECK(w.counter.finished == 1);

    // Back to the pixels that are shown: nothing to decode.
    w.action("undoAction").trigger();
    QTest::qWait(100);
    CHECK(w.shown() == Demosaic::Ahd);
    CHECK(w.counter.started == 2);

    // The same choice again is asked for again, and now it works.
    w.counter.fail = false;
    w.action("redoAction").trigger();
    REQUIRE(w.settled(2));
    CHECK(w.counter.started == 3);
    CHECK(w.counter.last() == Demosaic::Vng);
    CHECK(w.shown() == Demosaic::Vng);
}

TEST_CASE("A changed demosaic changes the picture shown", "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    const QImage before = w.view().wholeFrameImage();
    w.choose(Demosaic::Vng);
    REQUIRE(w.settled(2));
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != before; }, 20000));
}

TEST_CASE("Going to a step across a demosaic decodes once, and shows that step",
          "[app][window][negative][history]") {
    Window w;
    REQUIRE(w.settled(1));
    const QImage ahd = w.view().wholeFrameImage();

    w.choose(Demosaic::Vng);
    REQUIRE(w.settled(2));
    w.setExposure(0.5);
    REQUIRE(w.historyRows() == 3); // Exposure, Demosaic, Opened.
    QTest::qWait(100);
    CHECK(w.counter.started == 2);

    w.clickHistory(2); // Opened: AHD again, with the exposure edit still ahead.
    REQUIRE(w.settled(3));
    CHECK(w.counter.started == 3);
    CHECK(w.counter.last() == Demosaic::Ahd);
    CHECK(w.shown() == Demosaic::Ahd);
    CHECK(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == ahd; }, 20000));

    w.clickHistory(0); // The newest: VNG and the exposure, in one jump.
    REQUIRE(w.settled(4));
    CHECK(w.counter.started == 4);
    CHECK(w.counter.last() == Demosaic::Vng);
    CHECK(w.shown() == Demosaic::Vng);

    w.clickHistory(1); // Same pixels, no exposure: no decode.
    QTest::qWait(150);
    CHECK(w.counter.started == 4);
    CHECK(w.shown() == Demosaic::Vng);
}

TEST_CASE("Discarding changes brings back the saved negative and its pixels",
          "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    const QImage ahd = w.view().wholeFrameImage();
    w.choose(Demosaic::Vng);
    REQUIRE(w.settled(2));
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != ahd; }, 20000));

    QTimer::singleShot(0, [] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            box->button(QMessageBox::Discard)->click();
        }
    });
    w.window.close(); // Asks about the unsaved change, and discards it.
    REQUIRE(w.settled(3));
    CHECK(w.counter.last() == Demosaic::Ahd);
    CHECK(w.shown() == Demosaic::Ahd);
    CHECK(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == ahd; }, 20000));
}

TEST_CASE("A sidecar changed on disk to another demosaic decodes with it",
          "[app][window][negative]") {
    Window w;
    REQUIRE(w.settled(1));
    const QImage ahd = w.view().wholeFrameImage();

    const Photo bare = openPhoto(w.folder.file("a.dng"));
    writeSidecar(Photo(w.folder.file("a.dng"), bare.metadata(),
                       NegativeSettings{.demosaic = Demosaic::Vng}, bare.state()));
    emit w.strip().sidecarChangedExternally(w.pathOf("a.dng"));
    REQUIRE(w.settled(2));
    CHECK(w.counter.last() == Demosaic::Vng);
    CHECK(w.shown() == Demosaic::Vng);
    CHECK(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != ahd; }, 20000));
    QTest::qWait(150);
    CHECK(w.counter.started == 2);
}

TEST_CASE("An export queued during a re-decode renders the chosen demosaic",
          "[app][window][negative][export]") {
    Window w;
    REQUIRE(w.settled(1));

    // Exports through the real dialogs, which are modal: accepts the options, names the file,
    // and waits for the written picture (the format's own suffix replaces the typed one).
    const auto exportAs = [&](const char* stem) {
        const std::filesystem::path typed = w.folder.file(std::string(stem) + ".png");
        int stage = 0;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&] {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* files = qobject_cast<QFileDialog*>(modal)) {
                files->findChild<QLineEdit*>("fileNameEdit")
                    ->setText(QString::fromStdU16String(typed.u16string()));
                static_cast<QDialog*>(files)->accept();
                stage = 2;
            } else if (auto* dialog = qobject_cast<QDialog*>(modal); dialog && stage == 0) {
                dialog->accept();
                stage = 1;
            }
        });
        poll.start(10);
        w.action("exportAction").trigger();
        poll.stop();
        REQUIRE(stage == 2);
        return [&w, stem] {
            QImage image;
            REQUIRE(QTest::qWaitFor(
                [&] {
                    for (const auto& entry : std::filesystem::directory_iterator(w.folder.path())) {
                        if (entry.path().stem() == stem && entry.path().extension() != ".xmp" &&
                            image.load(QString::fromStdU16String(entry.path().u16string()))) {
                            return true;
                        }
                    }
                    return false;
                },
                20000));
            return image;
        };
    };

    const QImage ahd = exportAs("ahd")();

    w.counter.hold = true;
    w.choose(Demosaic::Vng);
    REQUIRE(QTest::qWaitFor([&] { return w.counter.started == 2; }, 20000));
    // The pixels shown are still AHD, so the export thread decodes for itself.
    const auto during = exportAs("during");
    w.counter.hold = false;
    const QImage duringImage = during();
    REQUIRE(w.settled(2));

    const QImage after = exportAs("after")();
    CHECK(duringImage != ahd);
    CHECK(duringImage == after);
}
