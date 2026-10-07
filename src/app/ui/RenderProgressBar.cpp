#include "RenderProgressBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// Width of the bar, in average characters of the font.
constexpr int barWidthInCharacters = 16;

} // namespace

RenderProgressBar::RenderProgressBar(QWidget* parent)
    : QWidget(parent), step_(new QLabel(this)), bar_(new QProgressBar(this)) {
    step_->setObjectName("renderStepLabel");
    bar_->setObjectName("renderProgressBar");
    bar_->setRange(0, resolution);
    bar_->setFixedWidth(barWidthInCharacters * fontMetrics().averageCharWidth());
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(step_);
    layout->addWidget(bar_);
    hide();
}

void RenderProgressBar::setDisplay(const RenderActivity::Display& display) {
    display_ = display;
    setVisible(display.visible);
    if (!display.visible) {
        return;
    }
    step_->setText(renderStepText(display.step));
    if (display.fraction) {
        // Changed only when it changes, so that the style's busy animation is not restarted.
        if (bar_->maximum() != resolution) {
            bar_->setRange(0, resolution);
        }
        bar_->setValue(
            static_cast<int>(std::lround(std::clamp(*display.fraction, 0.0, 1.0) * resolution)));
    } else if (bar_->maximum() != 0) {
        // No fraction yet: the style's own busy animation.
        bar_->setRange(0, 0);
    }
}

} // namespace arraw::app
