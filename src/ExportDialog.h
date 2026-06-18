#pragma once
#include "ColorManagement.h"
#include <QDialog>

class QComboBox;
class QSpinBox;
class QSlider;
class QCheckBox;
class QGroupBox;
class QLabel;

struct ExportOptions {
    enum class Format { JPEG, PNG, TIFF };
    Format format = Format::JPEG;
    int width = 0;
    int height = 0;
    int quality = 90;
    int sharpening = 0;
    OutputProfile profile = OutputProfile::SRgb;
    int bitDepth = 8; // 16 only for TIFF
};

/**
 * Modal dialog that gathers export settings for the current render.
 *
 * ExportDialog owns only the temporary UI state for one export operation:
 * format, dimensions, quality, sharpening, output profile, and TIFF bit depth.
 * Rendering and file writing stay with MainWindow/ImageViewport and the colour
 * pipeline.
 */
class ExportDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ExportDialog)
public:
    // srcW/srcH: natural pixel dimensions after crop (used as default output size)
    ExportDialog(int srcW, int srcH, QWidget* parent = nullptr);

    ExportOptions options() const;

private:
    void onFormatChanged(int idx);
    void onWidthChanged(int w);
    void onHeightChanged(int h);

    int srcW, srcH;
    bool syncing = false;

    QComboBox* formatBox;
    QComboBox* profileBox;
    QCheckBox* sixteenBitCheck;
    QSpinBox* widthSpin;
    QSpinBox* heightSpin;
    QCheckBox* constrainCheck;
    QGroupBox* qualityGroup;
    QSlider* qualitySlider;
    QSpinBox* qualitySpin;
    QSlider* sharpenSlider;
    QSpinBox* sharpenSpin;
};
