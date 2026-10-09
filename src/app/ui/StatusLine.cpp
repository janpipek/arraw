#include "StatusLine.h"

#include "RenderProgressPie.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>

namespace arraw::app {

StatusLine::StatusLine(QToolButton* zoomButton, QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    pie_ = new RenderProgressPie(this);
    message_ = new QLabel(this);
    // Ignored, so that a long message is elided instead of widening the window.
    message_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message_->setMinimumWidth(0);
    message_->installEventFilter(this);
    device_ = new QLabel(this);
    row->addWidget(pie_);
    row->addWidget(message_, 1);
    row->addWidget(device_);
    row->addWidget(zoomButton);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &StatusLine::clearMessage);
}

void StatusLine::showMessage(const QString& text, int timeoutMs, const QString& tip) {
    timer_.stop();
    text_ = text;
    tip_ = tip;
    elide();
    if (timeoutMs > 0 && !text.isEmpty()) {
        timer_.start(timeoutMs);
    }
}

void StatusLine::clearMessage() {
    timer_.stop();
    text_.clear();
    tip_.clear();
    elide();
}

bool StatusLine::eventFilter(QObject* watched, QEvent* event) {
    if (watched == message_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) {
        elide();
    }
    return QWidget::eventFilter(watched, event);
}

void StatusLine::elide() {
    const QString fitted =
        QFontMetrics(message_->font()).elidedText(text_, Qt::ElideRight, message_->width());
    if (message_->text() != fitted) {
        message_->setText(fitted);
    }
    message_->setToolTip(!tip_.isEmpty() ? tip_ : (fitted != text_ ? text_ : QString()));
}

} // namespace arraw::app
