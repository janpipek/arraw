#include "EditSession.h"

#include <LensResolution.h>
#include <LocalAdjustmentEdits.h>
#include <Sidecar.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

using namespace arraw;

void EditSession::begin() {
    if (editing()) {
        commit();
    }
    baseline_ = Baseline{photo_.state(), photo_.negative()};
}

void EditSession::update(DevelopState state) {
    if (!editing()) {
        throw std::logic_error("An edit must be begun before it is updated");
    }
    photo_ = photo_.with(std::move(state));
}

void EditSession::updateNegative(NegativeSettings negative) {
    if (!editing()) {
        throw std::logic_error("An edit must be begun before it is updated");
    }
    photo_ = photo_.with(std::move(negative));
}

Photo EditSession::withLensRecorded(const LensSettings& before, const Photo& after) const {
    if (lensDatabase_ == nullptr || before == after.negative().lens) {
        return after;
    }
    return recordLensMatch(after, *lensDatabase_);
}

void EditSession::commitPhoto(Photo next, EditOrigin origin, std::string detail) {
    Baseline before = std::move(*baseline_);
    baseline_.reset();
    photo_ = std::move(next);
    if (before.state != photo_.state() || before.negative != photo_.negative()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(position_) + 1,
                       history_.end());
        history_.push_back({photo_.state(), origin, std::move(detail), photo_.negative()});
        ++position_;
    }
}

void EditSession::commit(EditOrigin origin, std::string detail) {
    if (!editing()) {
        throw std::logic_error("There is no open edit to commit");
    }
    // Matched before anything changes, so a throw leaves the edit open.
    Photo next = withLensRecorded(baseline_->negative.lens, photo_);
    commitPhoto(std::move(next), origin, std::move(detail));
}

void EditSession::cancel() {
    if (!editing()) {
        throw std::logic_error("There is no open edit to cancel");
    }
    // The baseline was valid when it was taken, so this cannot throw on its contents.
    photo_ = photo_.with(std::move(baseline_->negative)).with(std::move(baseline_->state));
    baseline_.reset();
}

void EditSession::setState(DevelopState state, EditOrigin origin, std::string detail) {
    // Checked before anything opens, so an invalid state leaves no edit behind.
    Photo next = photo_.with(std::move(state));
    begin();
    photo_ = std::move(next);
    commit(origin, std::move(detail));
}

void EditSession::setNegative(NegativeSettings negative, EditOrigin origin, std::string detail) {
    setState(negative, photo_.state(), origin, std::move(detail));
}

void EditSession::setState(NegativeSettings negative, DevelopState state, EditOrigin origin,
                           std::string detail) {
    // Both are checked before anything changes, so an invalid one leaves no edit behind.
    Photo next = photo_.with(negative).with(std::move(state));
    // Both records are made before anything changes, so a throw leaves the session as it was.
    // The open edit's own record comes first: the new negative then keeps it when it turns every
    // correction off.
    std::optional<Photo> committed;
    if (editing()) {
        committed = withLensRecorded(baseline_->negative.lens, photo_);
    }
    const Photo& current = committed ? *committed : photo_;
    if (committed && !photo_.negative().lens.lens && committed->negative().lens.lens &&
        !next.negative().lens.lens) {
        // The caller built it before the commit recorded a lens, so it cannot know the record.
        NegativeSettings negative = next.negative();
        negative.lens.lens = committed->negative().lens.lens;
        next = next.with(std::move(negative));
    }
    next = withLensRecorded(current.negative().lens, next);
    if (committed) {
        commitPhoto(std::move(*committed), EditOrigin::Edit, {});
    }
    if (next.state() == photo_.state() && next.negative() == photo_.negative()) {
        return;
    }
    photo_ = std::move(next);
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(position_) + 1, history_.end());
    history_.push_back({photo_.state(), origin, std::move(detail), photo_.negative()});
    ++position_;
}

bool EditSession::baselineIsCurrent() const noexcept {
    return baseline_->state == photo_.state() && baseline_->negative == photo_.negative();
}

bool EditSession::canUndo() const noexcept {
    return position_ > 0 || (editing() && !baselineIsCurrent());
}

bool EditSession::canRedo() const noexcept {
    // Redoing commits an open edit first, and one that changed something clears redo.
    return position_ + 1 < history_.size() && (!editing() || baselineIsCurrent());
}

void EditSession::goTo(std::size_t index) {
    if (editing()) {
        commit();
    }
    if (index >= history_.size()) {
        throw std::out_of_range("There is no history step " + std::to_string(index));
    }
    photo_ = photo_.with(history_[index].negative).with(history_[index].state);
    position_ = index;
}

void EditSession::undo() {
    if (editing()) {
        commit();
    }
    if (position_ == 0) {
        throw std::logic_error("There is nothing to undo");
    }
    goTo(position_ - 1);
}

void EditSession::redo() {
    if (editing()) {
        commit();
    }
    if (position_ + 1 >= history_.size()) {
        throw std::logic_error("There is nothing to redo");
    }
    goTo(position_ + 1);
}

void EditSession::save() {
    // The photograph as the commit will leave it, matched before anything is written or changed.
    Photo next = editing() ? withLensRecorded(baseline_->negative.lens, photo_) : photo_;
    // Written first, so a failure leaves even an open edit open.
    writeSidecar(next);
    saved_ = next;
    if (editing()) {
        commitPhoto(std::move(next), EditOrigin::Edit, {});
    }
}

void EditSession::setMarks(PhotoMarks marks) {
    // Both are checked and written before anything changes.
    Photo saved = saved_.with(marks);
    Photo current = photo_.with(marks);
    writeSidecar(saved);
    saved_ = std::move(saved);
    photo_ = std::move(current);
}

void EditSession::discardChanges() {
    photo_ = saved_;
    baseline_.reset();
    history_ = {{saved_.state(), EditOrigin::Opened, {}, saved_.negative()}};
    position_ = 0;
}

namespace {

/// @brief Lists how the local adjustments of two states differ.
LocalChange describeLocalChange(const DevelopState& before, const DevelopState& after) {
    LocalChange change;
    const auto holds = [](const DevelopState& state, LocalAdjustmentId id) {
        return std::ranges::find(state.localAdjustments, id, &LocalAdjustment::id) !=
               state.localAdjustments.end();
    };
    for (const LocalAdjustment& adjustment : before.localAdjustments) {
        if (!holds(after, adjustment.id)) {
            change.removed.push_back(adjustment.id);
        }
    }
    std::vector<LocalAdjustmentId> orderAfter;
    for (const LocalAdjustment& adjustment : after.localAdjustments) {
        if (holds(before, adjustment.id)) {
            orderAfter.push_back(adjustment.id);
        } else {
            change.added.push_back(adjustment.id);
        }
    }
    std::vector<LocalAdjustmentId> orderBefore;
    for (const LocalAdjustment& adjustment : before.localAdjustments) {
        if (holds(after, adjustment.id)) {
            orderBefore.push_back(adjustment.id);
        }
    }
    change.reordered = orderBefore != orderAfter;
    for (const LocalAdjustment& now : after.localAdjustments) {
        const LocalAdjustment* was = findLocalAdjustment(before, now.id);
        if (was == nullptr || *was == now) {
            continue;
        }
        MaskChange mask;
        mask.id = now.id;
        mask.name = was->name != now.name;
        mask.enabled = was->enabled != now.enabled;
        mask.opacity = was->opacity != now.opacity;
        mask.invert = was->invert != now.invert;
        const auto* wasBrush = std::get_if<BrushMask>(&was->shape);
        const auto* nowBrush = std::get_if<BrushMask>(&now.shape);
        if (wasBrush != nullptr && nowBrush != nullptr) {
            mask.strokes = *wasBrush != *nowBrush;
        } else {
            mask.shape = was->shape != now.shape;
        }
        for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
            if (was->deltas.*descriptor.member != now.deltas.*descriptor.member) {
                mask.deltas.push_back(descriptor.key);
            }
        }
        change.changed.push_back(std::move(mask));
    }
    return change;
}

} // namespace

ChangeDescription arraw::describeChange(const DevelopState& before, const DevelopState& after) {
    ChangeDescription change;
    bool first = true;
    bool oneGroup = true;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        const bool same = visitField(descriptor, before.settings, [&](const auto& was) {
            return visitField(descriptor, after.settings, [&](const auto& is) {
                // The row is the same, so the types are; the other pairings are never visited.
                if constexpr (std::is_same_v<decltype(was), decltype(is)>) {
                    return was == is;
                } else {
                    return false;
                }
            });
        });
        if (same) {
            continue;
        }
        change.keys.push_back(descriptor.key);
        if (first) {
            change.group = descriptor.group;
            first = false;
        } else if (change.group != descriptor.group) {
            oneGroup = false;
        }
    }
    if (!oneGroup) {
        change.group.reset();
    }
    change.local = describeLocalChange(before, after);
    return change;
}

ChangeDescription arraw::describeChange(const HistoryStep& before, const HistoryStep& after) {
    ChangeDescription change = describeChange(before.state, after.state);
    const NegativeSettings& was = before.negative;
    const NegativeSettings& now = after.negative;
    if (was.demosaic != now.demosaic) {
        change.negative.push_back("demosaic");
    }
    if (was.lens.distortion != now.lens.distortion) {
        change.negative.push_back("lensDistortion");
    }
    if (was.lens.vignettingAmount != now.lens.vignettingAmount) {
        change.negative.push_back("lensVignetting");
    }
    if (was.lens.lateralCa != now.lens.lateralCa) {
        change.negative.push_back("lensLateralCa");
    }
    if (was.lens.lens != now.lens.lens) {
        change.negative.push_back("lens");
    }
    return change;
}
