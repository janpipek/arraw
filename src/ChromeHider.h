#pragma once
#include <vector>

class QWidget;

// Hides a fixed set of chrome widgets together and restores each to exactly the
// visibility it had before. This is the snapshot/restore mechanism behind
// lights-out mode (docs/adr/0025), kept independent of CollapsiblePane so the
// dock/strip state machine remains untouched.
class ChromeHider {
public:
    explicit ChromeHider(std::vector<QWidget*> widgets);

    void hide();
    void restore();

    bool hidden() const { return hidden_; }

private:
    std::vector<QWidget*> widgets_;
    std::vector<bool> wasHidden_;
    bool hidden_ = false;
};
