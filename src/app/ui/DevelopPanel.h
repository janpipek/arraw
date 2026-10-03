#pragma once

#include <DevelopState.h>
#include <WhiteBalance.h>

#include <QWidget>

#include <optional>
#include <vector>

class QButtonGroup;
class QComboBox;
class QPushButton;
class QStackedWidget;

namespace arraw::app {

class SettingSlider;

/// @brief What the panel needs to know about the photograph, besides its state.
struct PanelContext {
    /// @brief Whether the photograph is a RAW file, which white balance needs for now.
    bool raw = true;

    /// @brief The light the decode balanced for, shown where a setting is absent.
    std::optional<ColourTemperature> asShot = std::nullopt;
};

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
    /// @param context What the photograph is, for the rows that depend on it.
    void showState(const DevelopState& state, const PanelContext& context);

    /// @brief Shows whether the white balance picker is armed, without emitting any signal.
    /// @param picking Whether the Pick button is checked.
    void setPicking(bool picking);

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

    /// @brief Announces that the Pick button was checked or unchecked by the user.
    /// @param picking Whether the picker is now armed.
    void pickToggled(bool picking);

    /// @brief Asks for the keyboard focus to go back to the photograph.
    void focusReleased();

private:
    /// @brief Builds the Treatment row, Colour and B&W.
    QWidget* buildTreatmentRow();

    /// @brief Builds the White Balance group.
    QWidget* buildWhiteBalanceGroup();

    /// @brief Builds the Color group, Saturation and Vibrance.
    QWidget* buildColorGroup();

    /// @brief Builds the HSL box, a page of band rows for each of Hue, Saturation and Luminance.
    QWidget* buildHslGroup();

    /// @brief Builds the Black & White mix box.
    QWidget* buildBlackAndWhiteGroup();

    /// @brief Reports a Treatment button the user chose, as one complete edit.
    void applyTreatment(bool grayscale);

    /// @brief Builds a row of a group and wires its edits.
    SettingSlider* addRow(std::string_view key, QWidget* group);

    /// @brief Writes an edited value into the last shown state and reports it.
    void applyEdit(const SettingSlider& row, double value);

    /// @brief Writes a cleared optional value into the last shown state and reports it.
    void applyClear(const SettingSlider& row);

    /// @brief Reports a combo entry the user chose, as one complete edit.
    void applyChoice(int index);

    /// Last state shown, kept only to build the next one.
    DevelopState shown_;

    std::vector<SettingSlider*> rows_;

    QComboBox* presetCombo_ = nullptr;
    QPushButton* pickButton_ = nullptr;

    QButtonGroup* treatment_ = nullptr;
    QWidget* colorGroup_ = nullptr;
    QWidget* hslGroup_ = nullptr;
    QWidget* blackAndWhiteGroup_ = nullptr;
};

} // namespace arraw::app
