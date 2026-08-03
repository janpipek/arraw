#pragma once
#include "develop/FieldSpec.h"
#include "develop/GlobalAdjustment.h"
#include "ui/ToneCurveWidget.h"
#include <array>
#include <vector>
#include <QHash>
#include <QWidget>

class QSlider;
class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;
class QVBoxLayout;
class QStackedWidget;
class QGroupBox;
class QButtonGroup;
class QEvent;
class Histogram;
class AdjustmentSpinBox;

/**
 * Global develop editor shown in the Adjustments tab.
 *
 * AdjustmentPanel presents sliders, spin boxes, histograms, and tone-curve
 * controls for GlobalAdjustment. It keeps only the transient widget copy needed
 * to emit live changes and commit boundaries; DevelopSession owns the canonical
 * parameters for the active image.
 */
class AdjustmentPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AdjustmentPanel)
public:
    explicit AdjustmentPanel(QWidget* parent = nullptr);

    void setHistogramSamples(const QImage& finalSample, const QImage& curveInputSample);

    GlobalAdjustment params() const { return adjustments; }

    void setParams(const GlobalAdjustment& p);
    void resetAll();

    // The resolved lens-profile name for the active image, shown beside the Lens
    // Corrections toggles. Empty disables the toggles and shows "No lens profile".
    void setLensProfileName(const QString& name);

    // Enable/disable the Demosaic combo. Disabled (with an explanation) for
    // non-Bayer sensors, where the Bayer algorithms do not apply (docs/adr/0036).
    void setDemosaicAvailable(bool available);

    // Reflect the viewport's active tool without re-triggering wbPickerToggled.
    void setWbPickerChecked(bool checked);
    void setWbPickerEnabled(bool enabled);

signals:
    void paramsChanged(const GlobalAdjustment&);
    void adjustmentCommitted(const GlobalAdjustment& before, const GlobalAdjustment& after);
    void straightenActive(bool active);
    void wbPickerToggled(bool checked);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct SliderRow {
        QSlider* slider;
        AdjustmentSpinBox* spin;
        QLabel* nameLabel;
        FieldSpec spec;
    };

    SliderRow addSlider(
        QVBoxLayout* layout,
        const QString& name,
        const FieldSpec& spec,
        const QString& tooltip = {},
        QWidget* trailing = nullptr); // extra widget between the slider and its spinbox
    std::vector<SliderRow*> allRows();
    void connectRow(SliderRow& row);
    void connectCurve();
    void syncParams();
    // Show the Colour + HSL panels in Colour treatment, or the B&W Mix panel in
    // Black & White (docs/adr/0048), per adjustments.convertToGrayscale.
    void applyTreatmentVisibility();
    void commit();
    void resetRow(SliderRow& row);
    void updateCurveChannelIndicators();

    Histogram* histogram;
    QComboBox* wbPresets;
    QPushButton* wbPickerButton;
    ToneCurveWidget* toneCurve;
    std::array<QPushButton*, 4> curveChannelBtns; // indexed by ToneCurveWidget::Channel
    QStackedWidget* hslStack;

    SliderRow exposure;
    SliderRow contrast;
    SliderRow highlights;
    SliderRow shadows;
    SliderRow whites;
    SliderRow blacks;
    SliderRow filmicHighlights;
    SliderRow temperature;
    SliderRow tint;
    SliderRow saturation;
    SliderRow vibrance;
    QComboBox* demosaicCombo;
    SliderRow texture;
    SliderRow clarity;
    SliderRow dehaze;
    SliderRow sharpening;
    SliderRow luminanceNoiseReduction; // Amount (docs/adr/0046)
    SliderRow luminanceNoiseReductionDetail;
    SliderRow colorNoiseReduction; // Strength (issue #59)
    SliderRow colorNoiseReductionSmoothness;
    SliderRow rotation;
    QCheckBox* lensCorrectDistortionBox;
    QCheckBox* lensCorrectVignettingBox;
    QCheckBox* lensCorrectCABox;
    QLabel* lensProfileLabel;
    SliderRow postCropVignetteAmount;
    SliderRow postCropVignetteMidpoint;
    SliderRow postCropVignetteFeather;
    SliderRow grainAmount;
    SliderRow grainSize;
    SliderRow grainRoughness;

    // HSL: 8 ranges per channel (Hue page=0, Sat page=1, Lum page=2)
    std::array<SliderRow, 8> hslHue;
    std::array<SliderRow, 8> hslSat;
    std::array<SliderRow, 8> hslLum;

    // Black & White (docs/adr/0048): a Colour|B&W treatment switch and the 8-band
    // mixer. The treatment toggles which of colorBox/hslBox vs bwMixBox is shown.
    QButtonGroup* treatmentGroup; // id 0 = Colour, id 1 = Black & White
    QGroupBox* colorBox;
    QGroupBox* hslBox;
    QGroupBox* bwMixBox;
    std::array<SliderRow, 8> bwMix;

    // Colour Grading (docs/adr/0052): three zones × (hue, sat) plus two global
    // shaping sliders. Always visible — it grades both Colour and B&W results, so
    // unlike the Colour/HSL/B&W panels it is never hidden by the treatment switch.
    std::array<SliderRow, 3> colorGradeHue; // [Shadows, Midtones, Highlights]
    std::array<SliderRow, 3> colorGradeSat;
    SliderRow colorGradeBalance;
    SliderRow colorGradeBlending;
    // A small colour patch beside each Hue slider showing the tint direction, and
    // repainted as the slider moves. The Hue groove itself is a static hue spectrum
    // (set once in the constructor) so the handle sits on the colour it selects.
    std::array<QLabel*, 3> colorGradeHueSwatch = {};
    void updateHueSwatch(int zone, int hueDegrees);

    GlobalAdjustment adjustments;
    GlobalAdjustment committed; // last committed state — baseline for the next undo entry

    QHash<QObject*, SliderRow*> resetTargets; // slider/label -> row, for double-click reset
};
