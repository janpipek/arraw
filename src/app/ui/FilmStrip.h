#pragma once

#include "FilmStripRules.h"
#include "FolderWatcher.h"
#include "ShotFilterModel.h"
#include "ShotModel.h"

#include <PhotoMarks.h>

#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>

class QComboBox;
class QEvent;
class QLabel;
class QListView;
class QToolButton;

namespace arraw::app {

class FilmStripDelegate;

/// @brief Strip of the shots of a folder, to cull and to pick the photograph to develop.
///
/// A view over a ::arraw::app::ShotModel and a ::arraw::app::ShotFilterModel that
/// also keeps the selection and the *active* shot, the one the develop view shows.
/// The strip never opens a photograph itself: a click or a step asks for one with
/// ::arraw::app::FilmStrip::activationRequested, and the window answers with
/// ::arraw::app::FilmStrip::setActive, or with nothing when the user cancels, so the
/// active shot and the selection stay as they were. Selecting with Ctrl or Shift
/// changes the selection and nothing else.
///
/// Marks are not written by the strip's own code path but through a writer the window
/// supplies (see ::arraw::app::FilmStrip::setMarksWriter), since only it knows which
/// photograph has a session. The filter is the session's and is not saved.
class FilmStrip : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(FilmStrip)
public:
    /// @brief Writes the marks of one shot, and throws what it cannot write.
    using MarksWriter = std::function<void(const std::filesystem::path&, const PhotoMarks&)>;

    /// @brief Builds an empty strip with its title bar.
    /// @param parent Owning widget.
    explicit FilmStrip(QWidget* parent = nullptr);

    /// @brief Stops watching the folder.
    ~FilmStrip() override;

    /// @brief Returns the title bar, to give to the dock that holds the strip.
    ///
    /// The folder button, the folder's name, and the filter controls. Owned by the strip
    /// until a dock takes it.
    /// @return The widget.
    [[nodiscard]] QWidget* titleBar() const noexcept {
        return titleBar_;
    }

    /// @brief Shows the shots of a folder, with none of them active.
    /// @param folder Folder to list.
    /// @throws std::runtime_error if the folder cannot be read; the strip is left as it was.
    void setFolder(const std::filesystem::path& folder);

    /// @brief Names the folder shown.
    /// @return The folder, empty before ::arraw::app::FilmStrip::setFolder.
    [[nodiscard]] const std::filesystem::path& folder() const noexcept {
        return model_.folder();
    }

    /// @brief Makes a shot the active one and the only one selected.
    ///
    /// Scrolls it into the middle of the strip. A shot the filter hides clears the
    /// filter first: the photograph is open, so it must be seen.
    /// @param primary Primary file of a shot of this folder; an unknown one is ignored.
    void setActive(const std::filesystem::path& primary);

    /// @brief Leaves no shot active, and none selected.
    void clearActive();

    /// @brief Names the active shot.
    /// @return Its primary file, or nothing before one is activated.
    [[nodiscard]] std::optional<std::filesystem::path> activePrimary() const;

    /// @brief Finds the shot a file belongs to.
    /// @param file A file of this folder, as the primary or as a companion.
    /// @return The shot's primary file, or nothing.
    [[nodiscard]] std::optional<std::filesystem::path>
    shotContaining(const std::filesystem::path& file) const;

    /// @brief Finds the first shot the filter shows.
    /// @return Its primary file, or nothing when the strip shows none.
    [[nodiscard]] std::optional<std::filesystem::path> firstVisible() const;

    /// @brief Returns the marks of the active shot.
    /// @return The marks the strip knows, defaults without an active shot.
    [[nodiscard]] PhotoMarks activeMarks() const;

    /// @brief Lists the shots that marks act on.
    /// @return The selected shots in strip order, or the active one if none is selected;
    /// empty without either.
    [[nodiscard]] std::vector<std::filesystem::path> targets() const;

    /// @brief Sets the function that writes marks.
    /// @param writer It must write the sidecar of the shot's primary file, and throw on failure.
    void setMarksWriter(MarksWriter writer);

    /// @brief Tells the watcher that the application wrote a photograph's sidecar.
    /// @param photo The photograph whose sidecar was just written.
    void noteOwnWrite(const std::filesystem::path& photo);

    /// @brief Sets the rating of the target shots.
    /// @param rating 0 to 5, or -1 for a reject.
    void rate(int rating);

    /// @brief Toggles a colour label on the target shots (see ::arraw::app::toggledLabel).
    /// @param label Label of the key.
    void toggleLabel(ColorLabel label);

    /// @brief Removes the colour label of the target shots.
    void clearLabel();

    /// @brief Asks for the shot a step from the active one among the shots shown.
    ///
    /// Emits ::arraw::app::FilmStrip::activationRequested; nothing happens at either end.
    /// @param delta Steps, -1 for the previous shot and 1 for the next.
    void navigate(int delta);

    /// @brief Sets the thumbnail of a shot; the seam for the thumbnail worker (plan, step 5).
    /// @param primary Primary file of the shot.
    /// @param thumbnail Image to show, or a null one for the placeholder.
    void setThumbnail(const std::filesystem::path& primary, QImage thumbnail);

    /// @brief Gives the filter that is applied.
    /// @return The filter; one that is not active shows everything.
    [[nodiscard]] const MarksFilter& filter() const noexcept {
        return proxy_.filter();
    }

    /// @brief Switches the filter off and resets its controls.
    void clearFilter();

    /// @brief Suggests a size for the dock.
    /// @return A strip as high as main's initial one.
    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// @brief Asks the window to make a shot the active one.
    /// @param primary Primary file of the shot.
    void activationRequested(const QString& primary);

    /// @brief Announces that the folder button was pressed.
    void folderRequested();

    /// @brief Asks for the active shot to be exported, from its context menu.
    void exportRequested();

    /// @brief Announces that the active shot changed.
    void activeChanged();

    /// @brief Announces that another program changed a shot's sidecar.
    /// @param primary Primary file of the shot; its marks are already refreshed.
    void sidecarChangedExternally(const QString& primary);

protected:
    /// @brief Handles clicks, context menus and the size of the cells.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Builds the title bar.
    void buildTitleBar();

    /// @brief Builds the filter's controls into the title bar.
    /// @param into The title bar.
    void buildFilterControls(QWidget* into);

    /// @brief Rebuilds the filter from the controls and applies it.
    void applyFilter();

    /// @brief Makes the cells as high as the strip allows.
    void updateCellSide();

    /// @brief Shows or hides the note that the filter leaves nothing.
    void updateEmptyHint();

    /// @brief Handles a press of the left button on a cell.
    /// @param index Cell under the pointer.
    /// @param modifiers Keyboard modifiers of the press.
    void clickCell(const QModelIndex& index, Qt::KeyboardModifiers modifiers);

    /// @brief Lists the shots a click on a cell would act on.
    /// @param index Cell that was clicked.
    /// @return The selection if it holds the cell, else the cell alone.
    [[nodiscard]] std::vector<std::filesystem::path> contextTargets(const QModelIndex& index) const;

    /// @brief Opens the context menu of a cell.
    /// @param position Position in the strip's viewport.
    void showContextMenu(const QPoint& position);

    /// @brief Sets one rating on shots.
    void rateShots(const std::vector<std::filesystem::path>& shots, int rating);

    /// @brief Toggles a label on shots.
    void toggleLabelOn(const std::vector<std::filesystem::path>& shots, ColorLabel label);

    /// @brief Sets or removes the label of shots.
    void setLabelOn(const std::vector<std::filesystem::path>& shots,
                    std::optional<ColorLabel> label);

    /// @brief Reads the marks of shots as they stand, the sidecar's where the model has not read
    /// it.
    [[nodiscard]] std::vector<PhotoMarks>
    currentMarks(const std::vector<std::filesystem::path>& shots) const;

    /// @brief Writes new marks for shots and shows them.
    ///
    /// Those that change are written; the ones that fail are reported together, once.
    /// @param shots Shots to change.
    /// @param edit Gives the marks a shot should have from the marks it has.
    void applyMarks(const std::vector<std::filesystem::path>& shots,
                    const std::function<PhotoMarks(const PhotoMarks&)>& edit);

    /// @brief Looks again, soon, for the active shot being hidden by the filter.
    void scheduleActiveCheck();

    /// @brief Asks for the nearest shown shot when the active one is filtered out.
    void ensureActiveShown();

    /// @brief Finds the shown index of a shot.
    [[nodiscard]] QModelIndex proxyIndexOf(const std::filesystem::path& primary) const;

    /// @brief Finds the primary file at a shown index.
    [[nodiscard]] static std::filesystem::path primaryAt(const QModelIndex& index);

    ShotModel model_;
    ShotFilterModel proxy_;
    FolderWatcher watcher_;
    QListView* list_ = nullptr;
    FilmStripDelegate* delegate_ = nullptr;
    QLabel* emptyHint_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QLabel* folderLabel_ = nullptr;
    QComboBox* starChoice_ = nullptr;
    std::vector<std::pair<QToolButton*, ColorLabel>> swatches_;
    QToolButton* clearButton_ = nullptr;
    MarksWriter writer_;
    std::filesystem::path active_;
    QTimer activeCheck_;
    bool updatingCells_ = false;
    /// Whether the left button went down on a cell, so that its release is ours too.
    bool pressHandled_ = false;
};

} // namespace arraw::app
