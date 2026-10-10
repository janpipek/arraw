#include "RenderActivity.h"

#include <QCoreApplication>

#include <algorithm>

namespace arraw::app {

void RenderActivity::begin(Clock::time_point now) {
    advance(now);
    if (busy_) {
        // A newer render of the same busy period: what it replaces stays until it reports.
        return;
    }
    busy_ = true;
    busySince_ = now;
    completedAt_.reset();
    fraction_.reset();
    step_ = ProgressStep::Pointwise;
}

void RenderActivity::report(Clock::time_point now, double fraction, ProgressStep step) {
    advance(now);
    if (!busy_) {
        return;
    }
    fraction_ = std::clamp(fraction, 0.0, 1.0);
    step_ = step;
}

void RenderActivity::finish(Clock::time_point now, bool completed) {
    advance(now);
    if (!busy_) {
        return;
    }
    busy_ = false;
    if (completed && shown_) {
        fraction_ = 1.0;
        completedAt_ = now;
    }
}

RenderActivity::Clock::time_point RenderActivity::hiddenAt() const {
    const Clock::time_point held = shownSince_ + renderActivityHold;
    return completedAt_ ? std::max(held, *completedAt_ + renderActivityFilled) : held;
}

void RenderActivity::advance(Clock::time_point now) {
    if (busy_ && !shown_ && now - busySince_ >= renderActivityDelay) {
        // Shown from when the delay ran out, however late this was asked.
        shown_ = true;
        shownSince_ = busySince_ + renderActivityDelay;
    }
    if (!busy_ && shown_ && now >= hiddenAt()) {
        shown_ = false;
        completedAt_.reset();
        fraction_.reset();
    }
}

RenderActivity::Display RenderActivity::poll(Clock::time_point now) {
    advance(now);
    Display display;
    display.visible = shown_;
    if (!shown_) {
        return display;
    }
    display.fraction = fraction_;
    display.step = step_;
    return display;
}

std::optional<RenderActivity::Clock::time_point> RenderActivity::nextChange() const {
    if (busy_ && !shown_) {
        return busySince_ + renderActivityDelay;
    }
    if (!busy_ && shown_) {
        return hiddenAt();
    }
    return std::nullopt;
}

QString renderStepText(ProgressStep step) {
    const auto text = [](const char* source) {
        return QCoreApplication::translate("RenderActivity", source);
    };
    switch (step) {
    case ProgressStep::Decode:
        return text("Decoding…");
    case ProgressStep::Denoise:
        return text("Reducing noise…");
    case ProgressStep::Context:
        return text("Analysing local contrast…");
    case ProgressStep::Coverage:
        return text("Painting brush masks…");
    case ProgressStep::Pointwise:
        return text("Developing…");
    case ProgressStep::Geometry:
        return text("Rotating and cropping…");
    case ProgressStep::Resize:
        return text("Resizing…");
    case ProgressStep::Effects:
        return text("Adding effects…");
    }
    return text("Developing…");
}

} // namespace arraw::app
