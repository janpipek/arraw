#include "CollapsiblePane.h"
#include <QWidget>

CollapsiblePane::CollapsiblePane(QWidget* expanded, QWidget* strip)
    : expanded(expanded), strip(strip) {
    setCollapsed(collapsed);
}

void CollapsiblePane::collapse() { setCollapsed(true); }
void CollapsiblePane::expand()   { setCollapsed(false); }
void CollapsiblePane::toggle()   { setCollapsed(!collapsed); }

bool CollapsiblePane::isCollapsed() const { return collapsed; }

// Exactly one of the two widgets is visible; this is the only place that decides
// which (ADR 0012).
void CollapsiblePane::setCollapsed(bool c) {
    collapsed = c;
    expanded->setVisible(!c);
    strip->setVisible(c);
}
