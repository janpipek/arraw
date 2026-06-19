#pragma once

class QWidget;

/**
 * Small helper that owns the visibility invariant for a collapsible pane.
 *
 * CollapsiblePane keeps an expanded widget and its thin reveal strip mutually
 * exclusive: exactly one is visible at a time (ADR 0012). It is intentionally a
 * plain C++ helper, not a QObject/controller, because it only applies this local
 * widget invariant.
 */
class CollapsiblePane {
public:
    CollapsiblePane(QWidget* expanded, QWidget* strip);

    void collapse(); // hide expanded content, show the strip
    void expand();   // show expanded content, hide the strip
    void toggle();
    bool isCollapsed() const;

private:
    void setCollapsed(bool); // the one place the invariant is applied

    QWidget* expanded;
    QWidget* strip;
    bool collapsed = false;
};
