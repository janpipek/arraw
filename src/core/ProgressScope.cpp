#include "ProgressScope.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <numeric>

using namespace arraw;
using namespace arraw::detail;

namespace {

/// @brief Smallest growth of the fraction worth a report: about a fifth of a percent.
constexpr double reportStep = 1.0 / 512.0;

thread_local ProgressSpan* current = nullptr;

} // namespace

Cancelled::Cancelled() : std::runtime_error("Cancelled") {}

ProgressSpan* detail::currentProgress() noexcept {
    return current;
}

HiddenProgress::HiddenProgress() noexcept : hidden_(current) {
    current = nullptr;
}

HiddenProgress::~HiddenProgress() {
    current = hidden_;
}

void detail::throwIfCancelled() {
    if (current != nullptr && current->cancelled()) {
        throw Cancelled();
    }
}

void detail::completeUnit() {
    if (current != nullptr) {
        static_cast<void>(current->beginUnit());
        current->endUnit();
    }
}

ProgressSpan::ProgressSpan(ProgressRoot* root) noexcept
    : root_(root), slot_{0.0, 1.0}, exceptions_(std::uncaught_exceptions()) {}

ProgressSpan::ProgressSpan(ProgressStep step, std::uint32_t units) {
    ProgressSpan* parent = current;
    if (parent == nullptr) {
        return;
    }
    root_ = parent->root_;
    slot_ = root_->slotOf(step);
    step_ = step;
    units_ = units;
    totalWeight_ = units;
    enter();
}

ProgressSpan::ProgressSpan(ProgressStep step, std::span<const double> unitWeights)
    : ProgressSpan(step, 0) {
    if (root_ != nullptr) {
        weights_ = unitWeights;
        units_ = static_cast<std::uint32_t>(unitWeights.size());
        totalWeight_ = std::accumulate(unitWeights.begin(), unitWeights.end(), 0.0);
    }
}

ProgressSpan::ProgressSpan(std::span<const double> unitWeights) {
    ProgressSpan* parent = current;
    if (parent == nullptr) {
        return;
    }
    slot_ = parent->beginUnit();
    root_ = parent->root_;
    step_ = parent->step_;
    weights_ = unitWeights;
    units_ = static_cast<std::uint32_t>(unitWeights.size());
    totalWeight_ = std::accumulate(unitWeights.begin(), unitWeights.end(), 0.0);
    isUnit_ = true;
    enter();
}

void ProgressSpan::enter() {
    if (root_->channel_->cancelled()) {
        throw Cancelled();
    }
    parent_ = current;
    exceptions_ = std::uncaught_exceptions();
    // Reported before the span becomes current: a callback that throws leaves
    // a constructor that throws, whose destructor never runs to restore it.
    root_->publish(slot_.start, step_);
    current = this;
}

ProgressSpan::~ProgressSpan() {
    if (root_ == nullptr || parent_ == nullptr) {
        // Inactive, or the top span, which its root closes.
        return;
    }
    current = parent_;
    if (std::uncaught_exceptions() > exceptions_) {
        return;
    }
    // A pass that declares its loops must run exactly those: one that drifted
    // from its list would report its shares out of step.
    assert((units_ == 0 || done_ == units_) && "a progress span ran other units than it declared");
    try {
        if (isUnit_) {
            parent_->endUnit();
        } else {
            root_->publish(slot_.end(), step_);
        }
    } catch (...) {
        // A callback that throws here has nowhere to go: the work is done.
    }
}

ProgressSlot ProgressSpan::beginUnit() {
    if (root_ == nullptr) {
        return {};
    }
    if (cancelled()) {
        throw Cancelled();
    }
    if (parent_ == nullptr) {
        // The top span: work outside every step, such as a conversion before
        // the first, is in no step's share, so it adds nothing.
        return {root_->reported_, 0.0};
    }
    if (units_ == 0 || totalWeight_ <= 0.0) {
        return slot_;
    }
    if (done_ >= units_) {
        // More units than the span declared: they add nothing more.
        return {slot_.end(), 0.0};
    }
    double before = 0.0;
    double weight = 1.0;
    if (weights_.empty()) {
        before = done_;
    } else {
        before = std::accumulate(weights_.begin(), weights_.begin() + done_, 0.0);
        weight = weights_[done_];
    }
    const double start = slot_.start + slot_.width * before / totalWeight_;
    if (done_ + 1 == units_) {
        // The last unit ends exactly where the span does, so that what follows (the next
        // step, or a render resumed from here) starts where it ended, not an ulp away.
        return {start, slot_.end() - start};
    }
    return {start, slot_.width * weight / totalWeight_};
}

void ProgressSpan::endUnit() {
    if (root_ == nullptr) {
        return;
    }
    const ProgressSlot slot = beginUnit();
    ++done_;
    root_->publish(slot.end(), step_);
}

void ProgressSpan::report(const ProgressSlot& slot, double fraction) const {
    if (root_ != nullptr) {
        root_->publish(slot.start + slot.width * std::clamp(fraction, 0.0, 1.0), step_);
    }
}

bool ProgressSpan::cancelled() const noexcept {
    return root_ != nullptr && root_->channel_->cancelled();
}

ProgressRoot::ProgressRoot(ProgressChannel* channel, const StepWeights& weights, ProgressStep first)
    : channel_(channel), outer_(current), top_(channel != nullptr ? this : nullptr) {
    if (channel_ == nullptr) {
        current = nullptr;
        return;
    }
    if (channel_->cancelled()) {
        throw Cancelled();
    }
    const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
    double start = 0.0;
    for (std::size_t step = 0; step < progressStepCount; ++step) {
        const double width = total > 0.0 ? std::max(weights[step], 0.0) / total : 0.0;
        slots_[step] = {start, width};
        start += width;
    }
    top_.step_ = first;
    // As in ProgressSpan::enter: reported before anything is installed.
    publish(slotOf(first).start, first, true);
    current = &top_;
}

ProgressRoot::~ProgressRoot() {
    current = outer_;
}

void ProgressRoot::finish(ProgressStep last) {
    if (channel_ != nullptr) {
        publish(slotOf(last).end(), last, true);
    }
}

ProgressSlot ProgressRoot::slotOf(ProgressStep step) const noexcept {
    return slots_[static_cast<std::size_t>(step)];
}

void ProgressRoot::publish(double fraction, ProgressStep step, bool force) {
    // Rounding in the slots must not leave a finished operation short of one.
    fraction = std::clamp(fraction, reported_, 1.0);
    if (fraction > 1.0 - 1e-9) {
        fraction = 1.0;
    }
    if (!force && reportedStep_ == step && fraction < reported_ + reportStep &&
        !(fraction == 1.0 && reported_ < 1.0)) {
        return;
    }
    reported_ = fraction;
    reportedStep_ = step;
    channel_->report({.fraction = fraction, .step = step});
}
