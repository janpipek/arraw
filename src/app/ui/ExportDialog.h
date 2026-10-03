#pragma once

#include "ExportSettings.h"

#include <ImageBuffer.h>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QSlider;
class QSpinBox;
class QWidget;

namespace arraw::app {

/// @brief Dialog that gathers the settings of an export.
///
/// Only gathers: what the settings mean, and the file they go to, are decided
/// elsewhere (see ExportSettings).
class ExportDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ExportDialog)
public:
    /// @brief Builds the dialog showing settings.
    /// @param initial Settings to start from; its size is ignored.
    /// @param frame Size of the developed frame, which the width and height start at.
    /// @param parent Owning widget.
    ExportDialog(const ExportSettings& initial, ImageSize frame, QWidget* parent = nullptr);

    /// @brief Gives the settings as the dialog shows them now.
    [[nodiscard]] ExportSettings settings() const;

private:
    /// @brief Enables the controls that apply to the chosen format and size choice.
    void updateEnabled();

    QComboBox* formatBox_ = nullptr;
    QComboBox* profileBox_ = nullptr;
    QCheckBox* sixteenBitCheck_ = nullptr;
    QCheckBox* resizeCheck_ = nullptr;
    QSpinBox* widthSpin_ = nullptr;
    QSpinBox* heightSpin_ = nullptr;
    QCheckBox* enlargeCheck_ = nullptr;
    QGroupBox* qualityGroup_ = nullptr;
    QSlider* qualitySlider_ = nullptr;
    QSpinBox* qualitySpin_ = nullptr;
    QSlider* sharpenSlider_ = nullptr;
    QSpinBox* sharpenSpin_ = nullptr;
    QCheckBox* captureCheck_ = nullptr;
    QCheckBox* locationCheck_ = nullptr;
    QCheckBox* descriptiveCheck_ = nullptr;
};

} // namespace arraw::app
