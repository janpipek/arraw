#pragma once

#include "MaskEditing.h"

#include <DevelopState.h>
#include <LocalAdjustments.h>

#include <QGroupBox>

#include <optional>
#include <vector>

class QCheckBox;
class QLabel;
class QListView;
class QToolButton;

namespace arraw::app {

class MasksModel;
class SettingSlider;

/// @brief The Masks group of the develop panel: the list of masks and the controls of the
/// selected one (ADR 044, section 10).
///
/// Holds no authoritative state: whoever owns the state shows it with showState() and receives
/// each edit as a new state, in the develop panel's protocol (ADR 022). A slider drag or a run of
/// keyboard changes is one edit; every other action (rename, enable, invert, duplicate, delete,
/// move) is one complete edit. Selection, the armed tool and the overlay flag are view state of
/// the window: the panel reports what the user chose and shows what it is told, without emitting.
class MasksPanel : public QGroupBox {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MasksPanel)
public:
    /// @brief Builds the group, empty.
    /// @param parent Owning widget.
    explicit MasksPanel(QWidget* parent = nullptr);

    /// @brief Shows a state without emitting any signal.
    ///
    /// Keeps the selection while its mask is still in the list and clears it otherwise.
    /// @param state State to show; also the base of the next edit.
    void showState(const DevelopState& state);

    /// @brief Shows which mask is selected, without emitting any signal.
    /// @param id Mask to show the controls of; nothing for none.
    void setSelectedMask(std::optional<LocalAdjustmentId> id);

    /// @brief Gives the mask whose controls are shown.
    [[nodiscard]] std::optional<LocalAdjustmentId> selectedMask() const noexcept {
        return selected_;
    }

    /// @brief Shows which creation tool is armed, without emitting any signal.
    void setTool(MaskTool tool);

    /// @brief Shows whether the overlay is on, without emitting any signal.
    void setOverlayShown(bool shown);

    /// @brief Gives the width the labels of the rows need.
    [[nodiscard]] int labelWidthHint() const;

    /// @brief Sets the width of the label column, so that the rows line up with the panel's.
    void setLabelWidth(int width);

    /// @brief Ends any row's edit that is still waiting for its changes to pause.
    void finishPendingEdit();

signals:
    /// @brief Announces that an edit begins.
    void editStarted();

    /// @brief Announces the state an edit has reached.
    /// @param state Last shown state with the edit made.
    void stateEdited(const arraw::DevelopState& state);

    /// @brief Announces that the edit is over.
    void editFinished();

    /// @brief Announces a mask the user chose in the list, or none.
    void maskSelected(std::optional<arraw::LocalAdjustmentId> id);

    /// @brief Announces that the user armed or disarmed a creation tool.
    /// @param tool The tool now armed; None when the armed one was unchecked.
    void maskToolChosen(arraw::app::MaskTool tool);

    /// @brief Announces that the user toggled the overlay button.
    void overlayToggled(bool shown);

    /// @brief Announces that the user clicked a mask in the list, as opposed to moving in it by
    /// the keyboard.
    void maskClicked();

    /// @brief Asks for the keyboard focus to go back to the photograph.
    void focusReleased();

private:
    /// @brief Reports a state as one complete edit.
    void applyWhole(const DevelopState& next);

    /// @brief Shows the selected mask's controls, or the hint.
    void showSelected();

    /// @brief Enables the buttons as the list and the selection allow.
    void updateButtons();

    /// @brief Moves the selected mask by a number of places.
    void moveSelected(int places);

    /// @brief Builds a delta row or the opacity row and wires its edits.
    SettingSlider* addRow(const QString& id);

    /// @brief Ends every pending row edit but one row's.
    void finishOtherEdits(const SettingSlider* keep);

    DevelopState shown_;
    std::optional<LocalAdjustmentId> selected_;

    MasksModel* model_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* count_ = nullptr;
    QWidget* controls_ = nullptr;
    QCheckBox* invert_ = nullptr;
    QToolButton* linear_ = nullptr;
    QToolButton* radial_ = nullptr;
    QToolButton* duplicate_ = nullptr;
    QToolButton* remove_ = nullptr;
    QToolButton* up_ = nullptr;
    QToolButton* down_ = nullptr;
    QToolButton* overlay_ = nullptr;
    SettingSlider* opacity_ = nullptr;
    /// One row per local delta, in the table's order.
    std::vector<SettingSlider*> rows_;
    /// Whether the list's selection is being set by the panel, not the user.
    bool showing_ = false;
};

} // namespace arraw::app
