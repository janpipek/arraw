#pragma once
#include <vector>

class QWidget;

// Hides a fixed set of chrome widgets together and restores each to exactly the
// visibility it had before. This is the snapshot/restore mechanism behind
// lights-out mode (docs/adr/0027), kept independent of CollapsiblePane so the
// dock/strip state machine remains untouched.
class ChromeHider {
public:
    explicit ChromeHider(const std::vector<QWidget*>& widgets);

    void hide();
    void restore();

    bool hidden() const { return hidden_; }

private:
    // Each widget and the visibility captured by the last hide(), kept together
    // so the snapshot can never drift out of step with its widget.
    struct Entry {
        QWidget* widget;
        bool wasHidden = false; // valid only while hidden_
    };

    std::vector<Entry> entries_;
    bool hidden_ = false;
};
