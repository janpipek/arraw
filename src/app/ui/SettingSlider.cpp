#include "SettingSlider.h"

#include "SettingPresentation.h"

#include <SettingDescriptors.h>

#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace arraw::app {

namespace {

const FieldDescriptor& numericDescriptor(std::string_view key) {
    const FieldDescriptor* descriptor = findDescriptor(key);
    if (descriptor == nullptr || !descriptor->range) {
        throw std::invalid_argument("'" + std::string(key) + "' is not a ranged setting");
    }
    return *descriptor;
}

/// @brief Checks whether a setting's leaf is an optional number.
bool isOptional(const FieldDescriptor& descriptor) {
    return visitField(descriptor, DevelopSettings{}, [](const auto& field) {
        return std::is_same_v<std::remove_cvref_t<decltype(field)>, std::optional<float>>;
    });
}

} // namespace

SettingSlider::SettingSlider(std::string_view key, QWidget* parent)
    : QWidget(parent), key_(numericDescriptor(key).key), range_(*numericDescriptor(key).range),
      step_(presentationOf(key).step), scale_(presentationOf(key).scale), default_(0.0),
      optional_(isOptional(numericDescriptor(key))) {
    const SettingPresentation& presentation = presentationOf(key);
    // An optional setting has no default; the panel shows its fallback at once.
    default_ = optional_ ? (range_.minimum + range_.maximum) / 2.0
                         : defaultValueOf(numericDescriptor(key));

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
    slider_->setRange(0, tickCount(range_, step_, scale_));
    slider_->setToolTip(presentation.toolTip);
    // Arrow keys step between photographs, so a slider never takes them; the spin box is the
    // keyboard way to a value.
    slider_->setFocusPolicy(Qt::NoFocus);
    slider_->setValue(tickOf(default_, range_, step_, scale_));

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
        const double value = valueOfTick(tick, range_, step_, scale_);
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
            slider_->setValue(tickOf(value, range_, step_, scale_));
        }
        nudge(value);
    });
}

void SettingSlider::setValue(double value) {
    const QSignalBlocker sliderBlocker(slider_);
    const QSignalBlocker spinBlocker(spinBox_);
    slider_->setValue(tickOf(value, range_, step_, scale_));
    spinBox_->setValue(value);
}

bool SettingSlider::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_ && event->type() == QEvent::MouseButtonDblClick) {
        finishPendingEdit();
        emit editStarted();
        if (optional_) {
            emit valueCleared();
        } else {
            setValue(default_);
            emit valueEdited(default_);
        }
        emit editFinished();
        return true;
    }
    if ((watched == slider_ || watched == spinBox_) && event->type() == QEvent::FocusOut) {
        finishPendingEdit();
    }
    if (watched == spinBox_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // Commits the typed text (the spin box does not track the keyboard), then lets go.
            spinBox_->interpretText();
            emit focusReleased();
            return true;
        }
        if (key == Qt::Key_Escape) {
            // Drops what was typed: the text goes back to the value.
            if (auto* edit = spinBox_->findChild<QLineEdit*>()) {
                edit->setText(spinBox_->textFromValue(spinBox_->value()) + spinBox_->suffix());
            }
            emit focusReleased();
            return true;
        }
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
