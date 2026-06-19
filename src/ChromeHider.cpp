#include "ChromeHider.h"
#include <QWidget>
#include <cstddef>
#include <utility>

ChromeHider::ChromeHider(std::vector<QWidget*> widgets) : widgets_(std::move(widgets)) {}

void ChromeHider::hide() {
    if (hidden_)
        return;

    wasHidden_.clear();
    wasHidden_.reserve(widgets_.size());
    for (QWidget* widget : widgets_) {
        wasHidden_.push_back(widget ? widget->isHidden() : true);
        if (widget)
            widget->hide();
    }
    hidden_ = true;
}

void ChromeHider::restore() {
    if (!hidden_)
        return;

    for (std::size_t i = 0; i < widgets_.size(); ++i)
        if (QWidget* widget = widgets_[i])
            widget->setVisible(!wasHidden_[i]);
    hidden_ = false;
}
