#include "RenderIndicator.h"

#include <algorithm>
#include <chrono>
#include <optional>

namespace arraw::app {

namespace {

/// Time between polls while a sweep moves.
constexpr std::chrono::milliseconds sweepInterval{16};

} // namespace

RenderIndicator::RenderIndicator(QObject* parent)
    : RenderIndicator([] { return RenderActivity::Clock::now(); }, parent) {
    timed_ = true;
}

RenderIndicator::RenderIndicator(Clock clock, QObject* parent)
    : QObject(parent), clock_(std::move(clock)), timed_(false) {
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &RenderIndicator::poll);
}

void RenderIndicator::begin() {
    activity_.begin(clock_());
    refresh();
}

void RenderIndicator::report(double fraction, ProgressStep step) {
    activity_.report(clock_(), fraction, step);
    refresh();
}

void RenderIndicator::finish(bool completed) {
    activity_.finish(clock_(), completed);
    refresh();
}

void RenderIndicator::poll() {
    refresh();
}

void RenderIndicator::refresh() {
    const RenderActivity::Clock::time_point now = clock_();
    const RenderActivity::Display next = activity_.poll(now);
    if (!(next == display_)) {
        display_ = next;
        emit changed(display_);
    }
    if (!timed_) {
        return;
    }
    std::optional<std::chrono::milliseconds> wait;
    if (const auto due = activity_.nextChange()) {
        wait = std::max(std::chrono::milliseconds{1},
                        std::chrono::ceil<std::chrono::milliseconds>(*due - now));
    }
    if (display_.visible && !display_.fraction) {
        wait = std::min(wait.value_or(sweepInterval), sweepInterval);
    }
    if (wait) {
        timer_.start(*wait);
    } else {
        timer_.stop();
    }
}

} // namespace arraw::app
