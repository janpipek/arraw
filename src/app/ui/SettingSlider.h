#pragma once

#include "SettingPresentation.h"

#include <SettingDescriptors.h>

#include <QTimer>
#include <QWidget>

#include <string>
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
///
/// A row for an optional setting (temperature, tint) has no default of its
/// own: the panel shows a fallback with setValue() when the setting is absent,
/// and a reset reports valueCleared() instead of a value, for the panel to
/// apply as "absent again".
///
/// A setting whose presentation asks for a painted track (SliderTrack) gets
/// a groove showing what its values mean, such as the hues of a hue angle,
/// with the style's handle drawn over it.
class SettingSlider : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SettingSlider)
public:
    /// Shortest slider track a row keeps, in logical pixels: enough to place a value by eye.
    ///
    /// It is the slider's minimum width, so the minimum size of a panel of rows (and with it
    /// the develop dock's minimum width) grows with the label column and the value field.
    static constexpr int minimumSliderLength = 120;

    /// @brief Builds the row for a setting.
    /// @param key Key of a ranged float, double or optional float setting in the descriptor table.
    /// @param parent Owning widget.
    /// @throws std::invalid_argument if @p key names no such setting.
    /// @throws std::out_of_range if no presentation exists for @p key.
    explicit SettingSlider(std::string_view key, QWidget* parent = nullptr);

    /// @brief Builds a row that is not a row of the descriptor table.
    ///
    /// For controls that have their own range and presentation, such as the rows of a local
    /// adjustment (ADR 044), which share the label of a global control but not its range.
    /// The row is never optional: a reset restores @p defaultValue.
    /// @param id Name of the row, such as `local.exposure`; copied, and what key() gives.
    /// @param range Range of the values.
    /// @param defaultValue Value a reset restores.
    /// @param presentation Label, unit, decimals, step, tool tip, scale and track.
    /// @param parent Owning widget.
    SettingSlider(std::string_view id, const SettingRange& range, double defaultValue,
                  const SettingPresentation& presentation, QWidget* parent = nullptr);

    /// @brief Gives the key of the setting, or the id the row was given.
    [[nodiscard]] std::string_view key() const noexcept {
        return key_;
    }

    /// @brief Gives the width the label needs for its text.
    [[nodiscard]] int labelWidthHint() const;

    /// @brief Sets the width of the label column, so that the rows of a panel line up.
    /// @param width Width in pixels; at least ::arraw::app::SettingSlider::labelWidthHint
    /// for the text to fit.
    void setLabelWidth(int width);

    /// @brief Shows a value without emitting any signal.
    /// @param value Value in the setting's units.
    void setValue(double value);

    /// @brief Sets the value a reset (a double-click of the label) restores.
    ///
    /// The photograph's own default, which can depend on its kind (see
    /// ::arraw::defaultStateFor). Ignored for an optional setting, which a reset clears.
    /// @param value Value in the setting's units.
    void setDefaultValue(double value);

    /// @brief Gives the value a reset restores.
    [[nodiscard]] double defaultValue() const noexcept {
        return default_;
    }

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

    /// @brief Announces that a reset cleared an optional setting; follows editStarted().
    void valueCleared();

    /// @brief Announces that the edit is over.
    void editFinished();

    /// @brief Asks for the keyboard focus to go back to the photograph.
    ///
    /// Sent when Enter, Return or Esc ends the typing in the spin box, so that the
    /// arrow keys step between photographs again.
    void focusReleased();

protected:
    /// @brief Resets on a double-click of the label; ends a pending edit when focus leaves.
    ///
    /// Also hands the focus back on Enter, Return and Esc in the spin box: Enter keeps
    /// what was typed, Esc drops it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Builds the row; the other constructors delegate here.
    SettingSlider(std::string_view id, const SettingRange& range, double defaultValue,
                  const SettingPresentation& presentation, bool optional, QWidget* parent);

    /// @brief Reports one change of a keyboard, wheel or spin-box edit, opening it if needed.
    /// @param value Value in the setting's units.
    void nudge(double value);

    /// Key of the setting, or the id of a row of another table.
    std::string key_;

    /// Range of the setting.
    SettingRange range_;

    /// Distance between two slider positions, in the setting's units.
    double step_;

    /// How the slider spreads its positions.
    SliderScale scale_;

    /// Value that a reset restores; unused when the setting is optional.
    double default_;

    /// Whether the setting is optional, so that a reset clears it.
    bool optional_;

    /// Single-shot timer that ends a pending edit once its changes pause.
    QTimer pendingTimer_;

    /// Whether an edit of keyboard, wheel or spin-box changes is open.
    bool pending_ = false;

    QLabel* label_ = nullptr;
    QSlider* slider_ = nullptr;
    QDoubleSpinBox* spinBox_ = nullptr;
};

} // namespace arraw::app
