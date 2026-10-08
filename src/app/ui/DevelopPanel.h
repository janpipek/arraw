#pragma once

#include "CurveEditing.h"

#include <CurveHistogram.h>
#include <DevelopState.h>
#include <EditSession.h>
#include <GeometrySettings.h>
#include <ImageImport.h>
#include <WhiteBalance.h>

#include <QWidget>

#include <optional>
#include <vector>

class QAbstractButton;
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

    /// @brief What a reset restores: the photograph's defaults (::arraw::defaultStateFor).
    DevelopSettings defaults{};

    /// @brief What the photograph declares about itself, for the edit rules of ::arraw::withValue.
    ImageMetadata photo{};
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

    /// @brief Shows whether the crop mode is on, without emitting any signal.
    ///
    /// In the mode every group but Crop is disabled: the mode is one edit of
    /// the geometry, which Esc throws away whole (ADR 040).
    /// @param cropping Whether the Crop button is checked.
    void setCropMode(bool cropping);

    /// @brief Gives the narrowest the develop dock can be, in logical pixels.
    ///
    /// Derived from the content: the panel's minimum width (label column, value
    /// field and a slider of at least ::arraw::app::SettingSlider::minimumSliderLength,
    /// plus margins) and a vertical scroll bar, so that nothing scrolls sideways.
    /// The dock's frame, if any, comes on top.
    [[nodiscard]] int minimumDockWidth() const;

    /// @brief Gives the width the develop dock opens at, in logical pixels.
    ///
    /// The minimum dock width and a few lines of the panel's font of room for the
    /// sliders to stretch into.
    [[nodiscard]] int defaultDockWidth() const;

    /// @brief Shows whether the straighten tool is armed, without emitting any signal.
    /// @param straightening Whether the Level button is checked.
    void setStraightening(bool straightening);

    /// @brief Shows whether the white balance picker is armed, without emitting any signal.
    /// @param picking Whether the Pick button is checked.
    void setPicking(bool picking);

    /// @brief Ends any row's edit that is still waiting for its changes to pause.
    ///
    /// For callers about to act on the history, such as undo, which must see
    /// that edit committed rather than open.
    void finishPendingEdit();

    /// @brief Tells what kind of step the edit being announced makes.
    ///
    /// Meaningful while ::arraw::app::DevelopPanel::editFinished is emitted: a Reset button's
    /// edit says ::arraw::EditOrigin::Reset, any other edit ::arraw::EditOrigin::Edit.
    [[nodiscard]] EditOrigin editOrigin() const noexcept {
        return editOrigin_;
    }

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

    /// @brief Announces that the Crop button was checked or unchecked by the user.
    /// @param cropping Whether the crop mode is asked for.
    void cropModeToggled(bool cropping);

    /// @brief Announces that the Level button was checked or unchecked by the user.
    /// @param straightening Whether the straighten tool is asked for.
    void straighteningToggled(bool straightening);

    /// @brief Announces an aspect the user chose from the menu.
    /// @param aspect Constraint chosen.
    /// @param matchOrientation Whether a ratio is the menu's landscape one, to be turned to
    /// suit the crop's orientation; false for a ratio taken as typed.
    void aspectChosen(const arraw::CropAspect& aspect, bool matchOrientation);

    /// @brief Announces that the lock button was checked or unchecked by the user.
    /// @param locked Whether the aspect is to be held at the crop's present ratio.
    void lockToggled(bool locked);

    /// @brief Announces that the Swap button was pressed.
    void orientationSwapped();

    /// @brief Announces a quarter-turn button.
    /// @param clockwise Whether the turn is clockwise.
    void turned(bool clockwise);

    /// @brief Announces a flip button.
    /// @param horizontal Whether left and right swap, rather than top and bottom.
    void flipped(bool horizontal);

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

    /// @brief Builds the Crop group: the mode, aspect, angle, turns, flips and Reset.
    QWidget* buildCropGroup();

    /// @brief Shows the geometry in the Crop group's controls, without emitting any signal.
    void showGeometry(const GeometrySettings& geometry);

    /// @brief Asks for the ratio of the Custom entry, then announces the choice.
    ///
    /// Cancelling leaves the settings as they were.
    void chooseCustomAspect();

    /// @brief Reports the geometry's return to its defaults, as one complete edit.
    void applyGeometryReset();

    /// @brief Origin of the edit in progress; see ::arraw::app::DevelopPanel::editOrigin.
    EditOrigin editOrigin_ = EditOrigin::Edit;

    /// @brief Builds the White Balance group.
    QWidget* buildWhiteBalanceGroup();

    /// @brief Builds the Tone Curve group: the channel switch, a reset and the curve editor.
    QWidget* buildToneCurveGroup();

    /// @brief Builds the Color group, Saturation and Vibrance.
    QWidget* buildColorGroup();

    /// @brief Builds the Presence group: Texture, Clarity and Dehaze.
    QWidget* buildPresenceGroup();

    /// @brief Builds the Colour Grading group: a hue and saturation per tonal zone, Balance,
    /// Blending.
    QWidget* buildColorGradingGroup();

    /// @brief Builds the Noise Reduction group: luminance and colour, each with its refinement.
    QWidget* buildNoiseReductionGroup();

    /// @brief Builds the Effects group: the post-crop vignette's rows and the grain's.
    QWidget* buildEffectsGroup();

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

    /// @brief Announces the end of an edit that a Reset button made.
    ///
    /// Sets the origin only around the one ::arraw::app::DevelopPanel::editFinished it emits.
    void finishResetEdit();

    /// @brief Reports a combo entry the user chose, as one complete edit.
    void applyChoice(int index);

    /// @brief Works out curveHistogramWanted() again, announcing a change.
    void updateCurveHistogramWanted();

    /// @brief Watches the parent's resizes: the viewport, when the panel is in a scroll area.
    void watchViewport();

    /// Last state shown, kept only to build the next one.
    DevelopState shown_;
    /// @brief State from before the row edit under way, the baseline of a run of straighten edits.
    DevelopState editStart_;

    /// @brief Photograph the shown state belongs to, for the edit rules.
    ImageMetadata photo_{};

    std::vector<SettingSlider*> rows_;

    QComboBox* presetCombo_ = nullptr;
    QPushButton* pickButton_ = nullptr;

    QPushButton* cropButton_ = nullptr;
    QPushButton* levelButton_ = nullptr;
    QAbstractButton* lockButton_ = nullptr;
    QAbstractButton* cropResetButton_ = nullptr;
    QComboBox* aspectCombo_ = nullptr;
    /// Treatment row and every group but Crop, disabled in the crop mode.
    std::vector<QWidget*> nonGeometryGroups_;

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
