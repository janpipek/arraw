#pragma once

#include <DevelopState.h>

#include <QWidget>

#include <vector>

namespace arraw::app {

class SettingSlider;

/// @brief Panel of the develop controls, showing a state and reporting edits to it.
///
/// Holds no authoritative state: whoever owns the state shows it with
/// showState() and receives each edit as a new state.
class DevelopPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DevelopPanel)
public:
    /// @brief Builds the panel with every row at its default.
    /// @param parent Owning widget.
    explicit DevelopPanel(QWidget* parent = nullptr);

    /// @brief Shows a state without emitting any signal.
    /// @param state State to show; also the base of the next edit.
    /// @param raw Whether the photograph is a RAW file, which raw-only rows need.
    void showState(const DevelopState& state, bool raw);

    /// @brief Ends any row's edit that is still waiting for its changes to pause.
    ///
    /// For callers about to act on the history, such as undo, which must see
    /// that edit committed rather than open.
    void finishPendingEdit();

signals:
    /// @brief Announces that an edit begins.
    void editStarted();

    /// @brief Announces the state an edit has reached.
    /// @param state Last shown state with the edited value replaced.
    void stateEdited(const arraw::DevelopState& state);

    /// @brief Announces that the edit is over.
    void editFinished();

private:
    /// @brief Writes an edited value into the last shown state and reports it.
    void applyEdit(const SettingSlider& row, double value);

    /// Last state shown, kept only to build the next one.
    DevelopState shown_;

    std::vector<SettingSlider*> rows_;
};

} // namespace arraw::app
