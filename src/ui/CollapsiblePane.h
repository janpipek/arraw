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

    // Lights-out (docs/adr/0028): hide both widgets together, remembering the
    // collapsed state, and later restore exactly that state. While hidden,
    // collapse/expand/toggle update the remembered state without revealing
    // anything, so the dock/strip invariant survives a lights-out cycle.
    void hide();
    void show();
    bool isHidden() const;

private:
    void apply(); // the one place the dock/strip visibility invariant is applied

    QWidget* expanded;
    QWidget* strip;
    bool collapsed = false;
    bool hidden = false;
};
