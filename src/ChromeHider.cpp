#include "ChromeHider.h"
#include <QWidget>

ChromeHider::ChromeHider(const std::vector<QWidget*>& widgets) {
    entries_.reserve(widgets.size());
    for (QWidget* widget : widgets)
        entries_.push_back({widget});
}

void ChromeHider::hide() {
    if (hidden_)
        return;

    for (Entry& entry : entries_) {
        entry.wasHidden = entry.widget ? entry.widget->isHidden() : true;
        if (entry.widget)
            entry.widget->hide();
    }
    hidden_ = true;
}

void ChromeHider::restore() {
    if (!hidden_)
        return;

    for (const Entry& entry : entries_)
        if (entry.widget)
            entry.widget->setVisible(!entry.wasHidden);
    hidden_ = false;
}
