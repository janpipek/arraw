#pragma once

#include "CurveEditing.h"

#include <CurveHistogram.h>
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

class CurveEditor;
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

    /// @brief Shows the curve-input histogram behind the tone curves.
    /// @param histogram Counts for the state being edited (ADR 035).
    void showCurveHistogram(const CurveHistogram& histogram);

    /// @brief Clears the histogram behind the tone curves, as for another photograph.
    void clearCurveHistogram();

    /// @brief Tells whether the curve editor is on screen, so its histogram worth counting.
    ///
    /// False while the panel or the Tone Curve group is hidden, and while the
    /// editor is scrolled wholly out of the scroll area the panel sits in.
    [[nodiscard]] bool curveHistogramWanted() const;

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

    /// @brief Announces that curveHistogramWanted() changed.
    /// @param wanted Whether the curve editor is now on screen.
    void curveHistogramWantedChanged(bool wanted);

protected:
    /// @brief Follows moves, resizes, showing and hiding, which change what of the editor shows.
    bool event(QEvent* event) override;

    /// @brief Follows the editor's showing, hiding and painting, and the viewport's resizes.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// @brief Builds the Treatment row, Colour and B&W.
    QWidget* buildTreatmentRow();

    /// @brief Builds the White Balance group.
    QWidget* buildWhiteBalanceGroup();

    /// @brief Builds the Tone Curve group: the channel switch, a reset and the curve editor.
    QWidget* buildToneCurveGroup();

    /// @brief Builds the Color group, Saturation and Vibrance.
    QWidget* buildColorGroup();

    /// @brief Builds the Colour Grading group: a hue and saturation per tonal zone, Balance,
    /// Blending.
    QWidget* buildColorGradingGroup();

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

    /// @brief Writes an edited curve into the last shown state and reports it.
    void applyCurveEdit(CurveChannel channel, const ToneCurve& curve);

    /// @brief Ends every pending edit but the one of a row or the curve editor.
    /// @param keep Row or editor whose edit begins; nullptr ends them all.
    void finishOtherEdits(const QObject* keep);

    /// @brief Reports a combo entry the user chose, as one complete edit.
    void applyChoice(int index);

    /// @brief Works out curveHistogramWanted() again, announcing a change.
    void updateCurveHistogramWanted();

    /// @brief Watches the parent's resizes: the viewport, when the panel is in a scroll area.
    void watchViewport();

    /// Last state shown, kept only to build the next one.
    DevelopState shown_;

    std::vector<SettingSlider*> rows_;

    QComboBox* presetCombo_ = nullptr;
    QPushButton* pickButton_ = nullptr;

    QButtonGroup* treatment_ = nullptr;
    CurveEditor* curveEditor_ = nullptr;
    QWidget* colorGroup_ = nullptr;
    QWidget* hslGroup_ = nullptr;
    QWidget* blackAndWhiteGroup_ = nullptr;

    /// Parent watched for resizes, which change how much of the panel shows.
    QWidget* viewport_ = nullptr;

    /// Whether the curve editor was on screen when last worked out.
    bool curveHistogramWanted_ = false;
};

} // namespace arraw::app
