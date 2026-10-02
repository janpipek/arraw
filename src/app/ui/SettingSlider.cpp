#include "SettingSlider.h"

#include "SettingPresentation.h"

#include <SettingDescriptors.h>

#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>

#include <stdexcept>
#include <string>

namespace arraw::app {

namespace {

const FieldDescriptor& numericDescriptor(std::string_view key) {
    const FieldDescriptor* descriptor = findDescriptor(key);
    if (descriptor == nullptr || !descriptor->range) {
        throw std::invalid_argument("'" + std::string(key) + "' is not a ranged setting");
    }
    return *descriptor;
}

} // namespace

SettingSlider::SettingSlider(std::string_view key, QWidget* parent)
    : QWidget(parent), key_(numericDescriptor(key).key), range_(*numericDescriptor(key).range),
      step_(1.0), default_(defaultValueOf(numericDescriptor(key))) {
    const SettingPresentation& presentation = presentationOf(key);
    step_ = presentation.step;

    label_ = new QLabel(presentation.label, this);
    label_->setToolTip(presentation.toolTip);
    label_->setMinimumWidth(110);
    label_->installEventFilter(this);

    // Long enough to bridge keyboard auto-repeat and a run of wheel notches,
    // short enough that a separate change soon after is its own step.
    pendingTimer_.setSingleShot(true);
    pendingTimer_.setInterval(500);
    connect(&pendingTimer_, &QTimer::timeout, this, &SettingSlider::finishPendingEdit);

    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, tickCount(range_, step_));
    slider_->setToolTip(presentation.toolTip);
    slider_->setValue(tickOf(default_, range_, step_));

    spinBox_ = new QDoubleSpinBox(this);
    spinBox_->setRange(range_.minimum, range_.maximum);
    spinBox_->setDecimals(presentation.decimals);
    spinBox_->setSingleStep(step_);
    spinBox_->setSuffix(presentation.unit);
    spinBox_->setKeyboardTracking(false);
    spinBox_->setValue(default_);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(label_);
    layout->addWidget(slider_, 1);
    layout->addWidget(spinBox_);

    slider_->installEventFilter(this);
    spinBox_->installEventFilter(this);

    connect(slider_, &QSlider::sliderPressed, this, [this] {
        finishPendingEdit();
        emit editStarted();
    });
    connect(slider_, &QSlider::sliderReleased, this, &SettingSlider::editFinished);
    connect(slider_, &QSlider::valueChanged, this, [this](int tick) {
        const double value = valueOfTick(tick, range_, step_);
        {
            const QSignalBlocker blocker(spinBox_);
            spinBox_->setValue(value);
        }
        if (slider_->isSliderDown()) {
            emit valueEdited(value);
        } else {
            nudge(value);
        }
    });
    connect(spinBox_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        {
            const QSignalBlocker blocker(slider_);
            slider_->setValue(tickOf(value, range_, step_));
        }
        nudge(value);
    });
}

void SettingSlider::setValue(double value) {
    const QSignalBlocker sliderBlocker(slider_);
    const QSignalBlocker spinBlocker(spinBox_);
    slider_->setValue(tickOf(value, range_, step_));
    spinBox_->setValue(value);
}

bool SettingSlider::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_ && event->type() == QEvent::MouseButtonDblClick) {
        finishPendingEdit();
        setValue(default_);
        emit editStarted();
        emit valueEdited(default_);
        emit editFinished();
        return true;
    }
    if ((watched == slider_ || watched == spinBox_) && event->type() == QEvent::FocusOut) {
        finishPendingEdit();
    }
    return QWidget::eventFilter(watched, event);
}

void SettingSlider::finishPendingEdit() {
    if (!pending_) {
        return;
    }
    pending_ = false;
    pendingTimer_.stop();
    emit editFinished();
}

void SettingSlider::nudge(double value) {
    if (!pending_) {
        pending_ = true;
        emit editStarted();
    }
    emit valueEdited(value);
    pendingTimer_.start();
}

} // namespace arraw::app
