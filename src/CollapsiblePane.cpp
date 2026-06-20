#include "CollapsiblePane.h"
#include <QWidget>

CollapsiblePane::CollapsiblePane(QWidget* expanded, QWidget* strip)
    : expanded(expanded),
      strip(strip) {
    apply();
}

void CollapsiblePane::collapse() {
    collapsed = true;
    apply();
}

void CollapsiblePane::expand() {
    collapsed = false;
    apply();
}

void CollapsiblePane::toggle() {
    collapsed = !collapsed;
    apply();
}

bool CollapsiblePane::isCollapsed() const {
    return collapsed;
}

void CollapsiblePane::hide() {
    hidden = true;
    apply();
}

void CollapsiblePane::show() {
    hidden = false;
    apply();
}

bool CollapsiblePane::isHidden() const {
    return hidden;
}

// The only place that decides the two widgets' visibility: while hidden, neither
// shows (lights-out, ADR 0025); otherwise exactly one shows, opposite the other
// (ADR 0012).
void CollapsiblePane::apply() {
    expanded->setVisible(!hidden && !collapsed);
    strip->setVisible(!hidden && collapsed);
}
