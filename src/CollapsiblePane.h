#pragma once

class QWidget;

// Owns the collapse semantics for a pair of widgets that are always opposite:
// the expanded content and the thin reveal strip shown in its place. The single
// place that knows "exactly one of the two is visible" (ADR 0012).
class CollapsiblePane {
public:
    CollapsiblePane(QWidget* expanded, QWidget* strip);

    void collapse();   // hide expanded content, show the strip
    void expand();     // show expanded content, hide the strip
    void toggle();
    bool isCollapsed() const;

private:
    void setCollapsed(bool);   // the one place the invariant is applied

    QWidget* expanded;
    QWidget* strip;
    bool collapsed = false;
};
