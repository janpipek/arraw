#include "DebugLog.h"
#include "SidecarLimits.h"
#include "SnapshotModel.h"
#include "support/GeneratedPhoto.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/MaskOverlay.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"
#include "ui/StatusLine.h"

#include <DevelopState.h>
#include <Photo.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QInputDialog>
#include <QListView>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSplitter>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The snapshot panel in the History dock: Create, preview, Restore, Rename, Delete, damaged
/// lists and the handling of the app's own sidecar writes (ADR 056, Gate 4).

namespace {

namespace fs = std::filesystem;

/// Adds a snapshot to the sidecar on disk, before or beside the window.
SnapshotUpdate seed(const fs::path& photo, std::uint64_t revision, std::string name, float exposure,
                    std::optional<SnapshotId> parent = std::nullopt) {
    Snapshot snapshot;
    snapshot.parent = parent;
    snapshot.name = std::move(name);
    snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000}};
    snapshot.state.settings.tone.exposure = exposure;
    return addSnapshot(photo, revision, snapshot);
}

/// Answers the next modal dialog from a timer, once one shows.
struct Answer {
    QTimer timer;
    bool answered = false;
    QString title;
    QString informative;

    explicit Answer(std::function<void(QWidget*)> act) {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this, act = std::move(act)] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                timer.stop();
                answered = true;
                title = modal->windowTitle();
                if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                    informative = box->informativeText();
                }
                act(modal);
            }
        });
        timer.start();
    }
};

/// Accepts a QInputDialog, with a name if one is given.
std::function<void(QWidget*)> typing(std::optional<QString> name = std::nullopt) {
    return [name](QWidget* modal) {
        auto* dialog = qobject_cast<QInputDialog*>(modal);
        REQUIRE(dialog != nullptr);
        if (name) {
            dialog->setTextValue(*name);
        }
        dialog->accept();
    };
}

/// Clicks a button of a message box.
std::function<void(QWidget*)> clicking(const QString& text) {
    return [text](QWidget* modal) {
        auto* box = qobject_cast<QMessageBox*>(modal);
        REQUIRE(box != nullptr);
        for (QAbstractButton* button : box->buttons()) {
            if (button->text().remove('&') == text) {
                button->click();
                return;
            }
        }
        FAIL("no button " << text.toStdString());
    };
}

/// A window over one photograph.
struct Window {
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};
    fs::path photo;

    explicit Window(const std::function<void(const fs::path&)>& prepare = {},
                    bool generated = false) {
        const fs::path fixtures(ARRAW_TEST_DATA_DIR);
        if (generated) {
            photo = folder.file("a.png");
            test::writeGeneratedPhoto(photo, 640, 480);
        } else {
            photo = folder.file("a.dng");
            fs::copy_file(fixtures / "preview-32x24.dng", photo);
        }
        if (prepare) {
            prepare(photo);
        }
        window.openInitialPath(photo);
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
        REQUIRE(QTest::qWaitFor([this] { return !strip().isListing(); }, 10000));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] DevelopPanel& panel() const {
        return *window.findChild<DevelopPanel*>();
    }

    [[nodiscard]] MaskOverlay& overlay() const {
        return view().maskOverlay();
    }

    [[nodiscard]] QTreeView& tree() const {
        return *window.findChild<QTreeView*>("snapshotTree");
    }

    [[nodiscard]] SnapshotModel& model() const {
        return *window.findChild<SnapshotModel*>();
    }

    [[nodiscard]] QListView& history() const {
        return *window.findChild<QListView*>("historyList");
    }

    [[nodiscard]] QDockWidget& developDock() const {
        return *window.findChild<QDockWidget*>("DevelopDock");
    }

    [[nodiscard]] QWidget& child(const char* name) const {
        auto* found = window.findChild<QWidget*>(name);
        REQUIRE(found != nullptr);
        return *found;
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

    [[nodiscard]] QDoubleSpinBox& exposureBox() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                return *row->findChild<QDoubleSpinBox*>();
            }
        }
        FAIL("no exposure row");
        throw;
    }

    void setExposure(double value) const {
        exposureBox().setValue(value);
        panel().finishPendingEdit();
    }

    [[nodiscard]] int steps() const {
        return history().model()->rowCount();
    }

    [[nodiscard]] QString step(int row) const {
        return history().model()->index(row, 0).data(Qt::DisplayRole).toString();
    }

    [[nodiscard]] OpenDocument disk() const {
        return openDocument(photo);
    }

    [[nodiscard]] QModelIndex rowNamed(const QString& name) const {
        const QModelIndexList found = model().match(model().index(0, 0), Qt::DisplayRole, name, 1,
                                                    Qt::MatchExactly | Qt::MatchRecursive);
        return found.isEmpty() ? QModelIndex() : found.first();
    }

    /// Makes the window the active one again, which a dialog's closing may have undone.
    void activate() {
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
    }

    /// Selects a row as a click does, which previews it.
    void select(const QString& name) const {
        const QModelIndex index = rowNamed(name);
        REQUIRE(index.isValid());
        tree().selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                            QItemSelectionModel::Rows);
    }

    [[nodiscard]] bool previewing() const {
        return child("snapshotPreviewBanner").isVisibleTo(&window);
    }

    /// Creates a snapshot through the action, with a name, and waits until it is listed.
    void create(const QString& name) {
        const std::size_t before = disk().snapshots.entries.size();
        Answer answer(typing(name));
        action("createSnapshotAction").trigger();
        REQUIRE(answer.answered);
        REQUIRE(
            QTest::qWaitFor([&] { return disk().snapshots.entries.size() == before + 1; }, 10000));
        REQUIRE(QTest::qWaitFor([&] { return rowNamed(name).isValid(); }, 10000));
        activate();
    }

    [[nodiscard]] bool waitForThumbnails() const {
        return QTest::qWaitFor(
            [this] {
                const SnapshotCollection collection = disk().snapshots;
                if (collection.entries.empty()) {
                    return false;
                }
                for (const Snapshot& snapshot : collection.entries) {
                    if (!snapshot.thumbnail) {
                        return false;
                    }
                }
                return true;
            },
            30000);
    }

    [[nodiscard]] bool waitForRenderOtherThan(const QImage& before) const {
        return QTest::qWaitFor([&] { return view().wholeFrameImage() != before; }, 20000);
    }

    void keyOnWindow(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const {
        QWidget* focus = QApplication::focusWidget();
        QTest::keyClick(focus != nullptr ? focus : const_cast<MainWindow*>(&window), key,
                        modifiers);
    }

    [[nodiscard]] QPointF at(double u, double v) const {
        return overlay().mapping()->widgetFrom(CorrectedPosition{u, v});
    }

    void mouse(QEvent::Type type, QPointF position, Qt::MouseButtons held = Qt::LeftButton) const {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, position, overlay().mapToGlobal(position), button, held,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&overlay(), &event);
    }

    [[nodiscard]] QString status() const {
        return window.findChild<StatusLine*>()->message();
    }
};

} // namespace

TEST_CASE("The snapshots sit above the history in the same dock", "[app][window][snapshots]") {
    Window w;
    auto* dock = w.window.findChild<QDockWidget*>("HistoryDock");
    REQUIRE(dock != nullptr);
    auto* splitter = w.window.findChild<QSplitter*>("historySplitter");
    REQUIRE(splitter != nullptr);
    CHECK(splitter->orientation() == Qt::Vertical);
    CHECK(dock->widget() == splitter);
    CHECK(splitter->indexOf(&w.child("snapshotPanel")) < splitter->indexOf(&w.history()));
    CHECK(w.child("snapshotPanel").isAncestorOf(&w.tree()));
    CHECK(w.child("snapshotPanel").isEnabled());
    CHECK(w.history().isEnabled());
    CHECK(w.action("presetsAndHistoryAction").text().contains("Snapshots"));
}

TEST_CASE("Ctrl+N creates a snapshot of the working look, off the UI thread",
          "[app][window][snapshots]") {
    Window w;
    w.setExposure(0.75);
    REQUIRE(w.steps() == 2);
    const bool modifiedBefore = w.window.isWindowModified();
    REQUIRE(modifiedBefore);
    REQUIRE(w.disk().snapshots.entries.empty());

    CHECK(w.action("createSnapshotAction").shortcut() == QKeySequence(Qt::CTRL | Qt::Key_N));
    Answer answer(typing()); // The default name.
    w.keyOnWindow(Qt::Key_N, Qt::ControlModifier);
    REQUIRE(answer.answered);
    CHECK(answer.title == "New Snapshot");

    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));
    const OpenDocument document = w.disk();
    REQUIRE(document.snapshots.entries.size() == 1);
    const Snapshot& created = document.snapshots.entries.front();
    CHECK_FALSE(created.name.empty());
    CHECK(created.state.settings.tone.exposure == 0.75F);
    // The row is the working base and the session is dirty on that account; no step was added.
    const QModelIndex row = w.model().index(0, 0);
    CHECK(w.model().data(row, SnapshotModel::BaseRole).toBool());
    CHECK(w.window.isWindowModified());
    CHECK(w.steps() == 2);
    CHECK(w.status().contains("Created snapshot"));
    // Nothing else was saved: the sidecar holds neither the exposure nor a saved base.
    CHECK(document.photo.state().settings.tone.exposure == 0.0F);
    CHECK_FALSE(document.savedBase);
}

TEST_CASE("A Create with no edits makes the Save prompt say only the base changed",
          "[app][window][snapshots]") {
    Window w;
    w.create("First");
    REQUIRE(w.window.isWindowModified());

    Answer answer(clicking("Cancel"));
    CHECK_FALSE(w.window.close());
    REQUIRE(answer.answered);
    CHECK(answer.informative.contains("already saved"));
    CHECK(answer.informative.contains("kept either way"));
    CHECK(answer.informative.contains("First"));

    SECTION("with an edit as well the ordinary text says snapshots are kept") {
        w.setExposure(0.5);
        Answer again(clicking("Cancel"));
        CHECK_FALSE(w.window.close());
        REQUIRE(again.answered);
        CHECK(again.informative.contains("will be lost"));
        CHECK(again.informative.contains("Snapshots are already stored"));
    }
    SECTION("Ctrl+S saves the base") {
        w.window.findChild<QAction*>("saveAction")->trigger();
        CHECK_FALSE(w.window.isWindowModified());
        CHECK(w.disk().savedBase == SnapshotId{1});
    }
}

TEST_CASE("Selecting a row previews the snapshot and Esc ends it", "[app][window][snapshots]") {
    Window w([](const fs::path& photo) { seed(photo, 0, "Bright", 2.0F); });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));
    const QImage before = w.view().wholeFrameImage();
    const int steps = w.steps();
    const bool modified = w.window.isWindowModified();
    REQUIRE(w.exposureBox().value() == 0.0);
    REQUIRE(w.developDock().isEnabled());

    w.select("Bright");
    CHECK(w.previewing());
    CHECK_FALSE(w.developDock().isEnabled());
    CHECK_FALSE(w.history().isEnabled());
    CHECK_FALSE(w.action("undoAction").isEnabled());
    CHECK_FALSE(w.action("saveAction").isEnabled());
    CHECK_FALSE(w.action("cropAction").isEnabled());
    CHECK_FALSE(w.action("copySettingsAction").isEnabled());
    CHECK_FALSE(w.action("createSnapshotAction").isEnabled());
    CHECK(w.action("restoreSnapshotAction").isEnabled());
    // An export would write the working look, not the one on screen.
    CHECK_FALSE(w.action("exportAction").isEnabled());
    CHECK_FALSE(w.window.findChild<QMenu*>("exportPresetMenu")->menuAction()->isEnabled());
    CHECK(w.child("snapshotPanel").isEnabled());
    // The panel shows the snapshot's values; the session is untouched.
    CHECK(w.exposureBox().value() == 2.0);
    CHECK(w.steps() == steps);
    CHECK(w.window.isWindowModified() == modified);
    REQUIRE(w.waitForRenderOtherThan(before));
    const QImage previewed = w.view().wholeFrameImage();

    w.keyOnWindow(Qt::Key_Escape);
    CHECK_FALSE(w.previewing());
    CHECK(w.action("exportAction").isEnabled());
    CHECK(w.developDock().isEnabled());
    CHECK(w.history().isEnabled());
    CHECK(w.exposureBox().value() == 0.0);
    CHECK_FALSE(w.tree().selectionModel()->hasSelection());
    CHECK(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != previewed; }, 20000));
    CHECK(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == before; }, 20000));
    CHECK(w.steps() == steps);
    CHECK(w.window.isWindowModified() == modified);
}

TEST_CASE("A click on a row previews it and only a double-click restores",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) { seed(photo, 0, "Bright", 2.0F); });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));
    const int steps = w.steps();
    const QPoint where = w.tree().visualRect(w.rowNamed("Bright")).center();

    QTest::mouseClick(w.tree().viewport(), Qt::LeftButton, Qt::NoModifier, where);
    CHECK(w.previewing());
    CHECK(w.steps() == steps);
    CHECK_FALSE(w.window.isWindowModified());

    QTest::mouseDClick(w.tree().viewport(), Qt::LeftButton, Qt::NoModifier, where);
    CHECK_FALSE(w.previewing());
    CHECK(w.steps() == steps + 1);
    CHECK(w.window.isWindowModified());
}

TEST_CASE("Another program deleting the previewed snapshot ends the preview",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) {
        seed(photo, 0, "Bright", 2.0F);
        seed(photo, 1, "Dark", -1.0F);
    });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 2; }, 10000));
    w.select("Bright");
    REQUIRE(w.previewing());

    const OpenDocument before = w.disk();
    const SnapshotId gone = before.snapshots.entries.front().id;
    deleteSnapshot(w.photo, before.snapshots.revision, gone);
    emit w.strip().sidecarChangedExternally(QString::fromStdU16String(w.photo.u16string()));

    CHECK_FALSE(w.previewing());
    CHECK(w.developDock().isEnabled());
    CHECK(w.history().isEnabled());
    CHECK(w.model().rowCount() == 1);
    CHECK_FALSE(w.rowNamed("Bright").isValid());
    CHECK(w.rowNamed("Dark").isValid());
}

TEST_CASE("The banner's Close and clearing the selection end a preview",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) { seed(photo, 0, "Bright", 2.0F); });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));

    w.select("Bright");
    REQUIRE(w.previewing());
    QTest::mouseClick(&static_cast<QAbstractButton&>(w.child("previewCloseButton")),
                      Qt::LeftButton);
    CHECK_FALSE(w.previewing());
    CHECK(w.developDock().isEnabled());

    w.select("Bright");
    REQUIRE(w.previewing());
    w.tree().selectionModel()->clearSelection();
    CHECK_FALSE(w.previewing());
}

TEST_CASE("Restore makes the snapshot's look the working look as one step",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) { seed(photo, 0, "Bright", 2.0F); });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));
    REQUIRE(w.steps() == 1);
    REQUIRE_FALSE(w.window.isWindowModified());

    SECTION("the banner's Restore button") {
        w.select("Bright");
        REQUIRE(w.previewing());
        QTest::mouseClick(&static_cast<QAbstractButton&>(w.child("previewRestoreButton")),
                          Qt::LeftButton);
    }
    SECTION("Enter on the row") {
        w.select("Bright");
        w.activate();
        w.tree().setFocus();
        QTest::keyClick(&w.tree(), Qt::Key_Return);
    }
    CHECK_FALSE(w.previewing());
    CHECK(w.steps() == 2);
    CHECK(w.step(0) == "Restore snapshot: Bright");
    CHECK(w.exposureBox().value() == 2.0);
    CHECK(w.window.isWindowModified());
    CHECK(w.developDock().isEnabled());
    CHECK(w.model().data(w.rowNamed("Bright"), SnapshotModel::BaseRole).toBool());
    // The sidecar still holds the saved look.
    CHECK_FALSE(w.disk().savedBase);
}

TEST_CASE("Rename and Delete change the sidecar and the tree", "[app][window][snapshots]") {
    Window w;
    w.create("One");
    w.create("Two"); // below One, the working base

    SECTION("rename, with F2 on the tree") {
        w.select("One");
        Answer answer(typing("Uno"));
        w.activate();
        w.tree().setFocus();
        QTest::keyClick(&w.tree(), Qt::Key_F2);
        REQUIRE(answer.answered);
        CHECK(answer.title == "Rename Snapshot");
        REQUIRE(QTest::qWaitFor([&] { return w.rowNamed("Uno").isValid(); }, 10000));
        CHECK_FALSE(w.rowNamed("One").isValid());
        CHECK(w.disk().snapshots.entries.front().name == "Uno");
        // The preview stays on the renamed snapshot.
        CHECK(w.previewing());
    }
    SECTION("deleting the working base moves the marker to its parent") {
        REQUIRE(w.model().data(w.rowNamed("Two"), SnapshotModel::BaseRole).toBool());
        w.select("Two");
        Answer answer(clicking("Delete"));
        w.action("deleteSnapshotAction").trigger();
        REQUIRE(answer.answered);
        CHECK(answer.title == "Delete Snapshot");
        CHECK(answer.informative.contains("based on its parent"));
        CHECK(answer.informative.contains("cannot be undone"));
        REQUIRE(QTest::qWaitFor([&] { return !w.rowNamed("Two").isValid(); }, 10000));
        CHECK(w.disk().snapshots.entries.size() == 1);
        CHECK(w.model().data(w.rowNamed("One"), SnapshotModel::BaseRole).toBool());
        // The previewed snapshot is gone, so the preview ended.
        CHECK_FALSE(w.previewing());
        CHECK(w.developDock().isEnabled());
    }
    SECTION("Cancel deletes nothing") {
        w.select("One");
        Answer answer(clicking("Cancel"));
        w.action("deleteSnapshotAction").trigger();
        REQUIRE(answer.answered);
        CHECK(w.disk().snapshots.entries.size() == 2);
        CHECK(w.rowNamed("One").isValid());
    }
    SECTION("children are named in the confirmation") {
        w.select("One");
        Answer answer(clicking("Delete"));
        w.action("deleteSnapshotAction").trigger();
        REQUIRE(answer.answered);
        CHECK(answer.informative.contains("move to its parent"));
        REQUIRE(QTest::qWaitFor([&] { return !w.rowNamed("One").isValid(); }, 10000));
        CHECK(w.model().data(w.rowNamed("Two"), SnapshotModel::ParentIdRole).toULongLong() == 0);
    }
}

TEST_CASE("The thumbnails of snapshots are stored, and the app's own write ends no mode",
          "[app][window][snapshots][slow]") {
    Window w([](const fs::path& photo) {
        seed(photo, 0, "Bright", 2.0F);
        seed(photo, 1, "Dark", -1.0F);
    });
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 2; }, 10000));
    w.action("cropAction").trigger();
    REQUIRE(w.view().isCropMode());
    const int steps = w.steps();

    // The worker renders the thumbnails and the writer stores them, while the mode is on.
    REQUIRE(w.waitForThumbnails());
    REQUIRE(QTest::qWaitFor(
        [&] {
            const QModelIndex row = w.rowNamed("Bright");
            const QImage picture = w.model().data(row, Qt::DecorationRole).value<QImage>();
            return picture.pixelColor(1, 1) != QColor(0x50, 0x50, 0x50);
        },
        10000));
    QTest::qWait(100);

    // The watcher's event for the write, delivered as the system would.
    emit w.strip().sidecarChangedExternally(QString::fromStdU16String(w.photo.u16string()));
    CHECK(w.view().isCropMode());
    CHECK_FALSE(w.status().contains("changed on disk"));
    CHECK_FALSE(w.status().contains("Reloaded"));
    CHECK(w.steps() == steps);
    CHECK(w.model().rowCount() == 2);
}

TEST_CASE("An external change of the snapshots alone ends no gesture and keeps the history",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) { seed(photo, 0, "Bright", 2.0F); }, true);
    REQUIRE(QTest::qWaitFor([&] { return w.model().rowCount() == 1; }, 10000));

    const bool unsaved = GENERATE(false, true);
    if (unsaved) {
        w.setExposure(0.5);
    }
    // A radial mask gesture left open.
    w.keyOnWindow(Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    QTest::mouseClick(w.window.findChild<QAbstractButton*>("maskRadial"), Qt::LeftButton);
    w.mouse(QEvent::MouseButtonPress, w.at(0.5, 0.5));
    w.mouse(QEvent::MouseMove, w.at(0.5, 0.5) + QPointF(40.0, 0.0));
    REQUIRE(w.overlay().isDragging());
    const int steps = w.steps();
    const bool modified = w.window.isWindowModified(); // The open gesture counts.
    REQUIRE(w.disk().snapshots.revision == 1);

    // Another program adds a snapshot.
    seed(w.photo, 1, "Added elsewhere", 0.25F);
    emit w.strip().sidecarChangedExternally(QString::fromStdU16String(w.photo.u16string()));

    CHECK(w.overlay().isDragging());
    CHECK(w.view().isMaskMode());
    CHECK(w.steps() == steps);
    CHECK(w.rowNamed("Added elsewhere").isValid());
    CHECK(w.model().rowCount() == 2);
    CHECK(w.window.isWindowModified() == modified);
    CHECK_FALSE(w.status().contains("changed on disk"));
}

TEST_CASE("Unreadable snapshot entries are shown and removed after a confirmation",
          "[app][window][snapshots]") {
    Window w([](const fs::path& photo) {
        seed(photo, 0, "Fine", 1.0F);
        // A node the reader cannot use, beside the good one.
        QFile file(QString::fromStdU16String(sidecarPath(photo).u16string()));
        REQUIRE(file.open(QIODevice::ReadWrite));
        QByteArray text = file.readAll();
        const qsizetype list = text.indexOf("</arraw:snapshots>");
        REQUIRE(list > 0);
        const qsizetype seq = text.lastIndexOf("</rdf:Seq>", list);
        REQUIRE(seq > 0);
        text.insert(
            seq, "<rdf:li rdf:parseType=\"Resource\"><arraw:id>9</arraw:id>"
                 "<arraw:name>Bad</arraw:name><arraw:created>2026-10-23T12:00:00Z</arraw:created>"
                 "<arraw:state>{ not json</arraw:state><arraw:negative>{}</arraw:negative>"
                 "</rdf:li>");
        file.seek(0);
        file.resize(0);
        file.write(text);
    });
    REQUIRE(w.disk().unreadableSnapshots == 1);
    CHECK(w.child("snapshotDamageNotice").isVisibleTo(&w.window));
    CHECK(w.action("discardUnreadableAction").isEnabled());
    CHECK(w.model().rowCount() == 1);

    SECTION("Cancel keeps them") {
        Answer answer(clicking("Cancel"));
        QTest::mouseClick(&static_cast<QAbstractButton&>(w.child("removeUnreadableButton")),
                          Qt::LeftButton);
        REQUIRE(answer.answered);
        CHECK(answer.informative.contains("cannot be undone"));
        CHECK(w.disk().unreadableSnapshots == 1);
        CHECK(w.child("snapshotDamageNotice").isVisibleTo(&w.window));
    }
    SECTION("Remove clears the sidecar and the notice") {
        Answer answer(clicking("Remove"));
        QTest::mouseClick(&static_cast<QAbstractButton&>(w.child("removeUnreadableButton")),
                          Qt::LeftButton);
        REQUIRE(answer.answered);
        REQUIRE(QTest::qWaitFor([&] { return w.disk().unreadableSnapshots == 0; }, 10000));
        REQUIRE(QTest::qWaitFor(
            [&] { return !w.child("snapshotDamageNotice").isVisibleTo(&w.window); }, 10000));
        CHECK(w.disk().snapshots.entries.size() == 1);
        CHECK(w.model().rowCount() == 1);
        CHECK_FALSE(w.action("discardUnreadableAction").isEnabled());
    }
}

TEST_CASE("A Create while the sidecar is locked keeps the window alive and then warns",
          "[app][window][snapshots][slow]") {
    Window w;
    const int timeoutBefore = detail::sidecarLockTimeout;
    detail::sidecarLockTimeout = 1500;
    struct Restore {
        int value;
        ~Restore() {
            detail::sidecarLockTimeout = value;
        }
    } restore{timeoutBefore};

    QLockFile lock(QString::fromStdU16String(sidecarPath(w.photo).u16string()) + ".lock");
    REQUIRE(lock.tryLock(1000));

    Answer name(typing("Locked out"));
    w.action("createSnapshotAction").trigger();
    REQUIRE(name.answered);

    // The event loop runs while the write waits for the lock.
    bool ticked = false;
    QTimer::singleShot(100, [&] { ticked = true; });
    Answer warning(clicking("OK"));
    REQUIRE(QTest::qWaitFor([&] { return ticked; }, 2000));
    // Actions rest while a structural write is in flight.
    CHECK_FALSE(w.action("createSnapshotAction").isEnabled());
    REQUIRE(QTest::qWaitFor([&] { return warning.answered; }, 10000));
    CHECK(warning.title == "Cannot Create Snapshot");

    // The failure has landed: the actions are back.
    CHECK(w.action("createSnapshotAction").isEnabled());
    lock.unlock();
    CHECK(w.model().rowCount() == 0);
    CHECK(w.disk().snapshots.entries.empty());
    CHECK_FALSE(w.window.isWindowModified());
    CHECK(w.steps() == 1);
}
