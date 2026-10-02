#pragma once

#include <SettingDescriptors.h>

#include <QTimer>
#include <QWidget>

#include <string_view>

class QDoubleSpinBox;
class QEvent;
class QLabel;
class QObject;
class QSlider;

namespace arraw::app {

/// @brief Row editing one numeric setting with a slider and a spin box.
///
/// Reports edits in the shape of the edit protocol (ADR 022). A drag is one
/// edit. Keyboard, wheel and spin-box changes that follow each other closely
/// are one edit too, which ends once they pause, focus leaves the row, or
/// finishPendingEdit() is called; a reset is an edit of its own.
class SettingSlider : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SettingSlider)
public:
    /// @brief Builds the row for a setting.
    /// @param key Key of a ranged float or double setting in the descriptor table.
    /// @param parent Owning widget.
    /// @throws std::invalid_argument if @p key names no such setting.
    /// @throws std::out_of_range if no presentation exists for @p key.
    explicit SettingSlider(std::string_view key, QWidget* parent = nullptr);

    /// @brief Gives the key of the setting.
    [[nodiscard]] std::string_view key() const noexcept {
        return key_;
    }

    /// @brief Shows a value without emitting any signal.
    /// @param value Value in the setting's units.
    void setValue(double value);

    /// @brief Ends an edit of keyboard, wheel or spin-box changes that is still waiting to end.
    ///
    /// Emits editFinished() when such an edit is open; does nothing otherwise.
    void finishPendingEdit();

signals:
    /// @brief Announces that an edit begins.
    void editStarted();

    /// @brief Announces the value an edit has reached.
    /// @param value Value in the setting's units.
    void valueEdited(double value);

    /// @brief Announces that the edit is over.
    void editFinished();

protected:
    /// @brief Resets on a double-click of the label; ends a pending edit when focus leaves.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Reports one change of a keyboard, wheel or spin-box edit, opening it if needed.
    /// @param value Value in the setting's units.
    void nudge(double value);

    /// Key of the setting, pointing into the descriptor table.
    std::string_view key_;

    /// Range of the setting.
    SettingRange range_;

    /// Distance between two slider positions, in the setting's units.
    double step_;

    /// Value that a reset restores.
    double default_;

    /// Single-shot timer that ends a pending edit once its changes pause.
    QTimer pendingTimer_;

    /// Whether an edit of keyboard, wheel or spin-box changes is open.
    bool pending_ = false;

    QLabel* label_ = nullptr;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* spinBox_ = nullptr;
};

} // namespace arraw::app
