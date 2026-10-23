#include "EditSession.h"

#include <LensResolution.h>
#include <LocalAdjustmentEdits.h>
#include <Sidecar.h>

#include <algorithm>
#include <chrono>
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
    baseline_ = Baseline{photo_.state(), photo_.negative(), base_};
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

void EditSession::appendStep(HistoryStep step) {
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(position_) + 1, history_.end());
    history_.push_back(std::move(step));
    ++position_;
}

void EditSession::commitPhoto(Photo next, EditOrigin origin, std::string detail) {
    Baseline before = std::move(*baseline_);
    baseline_.reset();
    photo_ = std::move(next);
    if (before.state != photo_.state() || before.negative != photo_.negative() ||
        before.base != base_) {
        appendStep({photo_.state(), origin, std::move(detail), photo_.negative(), base_});
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
    base_ = baseline_->base;
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
    appendStep({photo_.state(), origin, std::move(detail), photo_.negative(), base_});
}

bool EditSession::baselineIsCurrent() const noexcept {
    return baseline_->state == photo_.state() && baseline_->negative == photo_.negative() &&
           baseline_->base == base_;
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
    base_ = history_[index].baseSnapshot;
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
    // Written first, so a failure leaves even an open edit open. No marks are named: setMarks()
    // writes them at once, so the session never holds a mark it has not written.
    const auto write = [&] {
        if (baseKnown_ || base_ != savedBase_) {
            writeSidecar(next, base_, MarkChanges{});
        } else {
            // The saved base is unknown, and so left as the file has it.
            writeSidecar(next);
        }
    };
    try {
        write();
    } catch (const SnapshotBaseMissing&) {
        // Another process deleted the base since the session read the list. Read it again,
        // remap to the nearest survivor, and try once more; the refresh stands if this fails.
        refreshSnapshots();
        write();
    }
    saved_ = next;
    baseKnown_ = baseKnown_ || base_ != savedBase_;
    savedBase_ = base_;
    if (editing()) {
        commitPhoto(std::move(next), EditOrigin::Edit, {});
    }
}

void EditSession::setMarks(PhotoMarks marks) {
    // Both are checked and written before anything changes.
    Photo saved = saved_.with(marks);
    Photo current = photo_.with(marks);
    // Only the marks that differ: a recipe or rating another process wrote is not put back.
    writeSidecarMarkChanges(saved_.path(), changedMarks(saved_.marks(), marks));
    saved_ = std::move(saved);
    photo_ = std::move(current);
}

void EditSession::discardChanges() {
    photo_ = saved_;
    base_ = savedBase_;
    baseline_.reset();
    history_ = {{saved_.state(), EditOrigin::Opened, {}, saved_.negative(), savedBase_}};
    position_ = 0;
}

std::optional<SnapshotId> EditSession::survivor(std::optional<SnapshotId> id,
                                                const SnapshotCollection& fresh) const {
    // The held collection has no cycle, and the walk is bounded all the same.
    for (std::size_t steps = 0; id && fresh.find(*id) == nullptr; ++steps) {
        const Snapshot* known = steps <= snapshots_.entries.size() ? snapshots_.find(*id) : nullptr;
        id = known != nullptr ? known->parent : std::nullopt;
    }
    return id;
}

void EditSession::adopt(SnapshotCollection fresh) {
    base_ = survivor(base_, fresh);
    savedBase_ = survivor(savedBase_, fresh);
    if (baseline_) {
        baseline_->base = survivor(baseline_->base, fresh);
    }
    for (HistoryStep& step : history_) {
        step.baseSnapshot = survivor(step.baseSnapshot, fresh);
    }
    snapshots_ = std::move(fresh);
}

void EditSession::refreshSnapshots() {
    const std::optional<SidecarContents> contents = readSidecar(photo_.path());
    adopt(contents ? contents->snapshots : SnapshotCollection{});
}

void EditSession::refreshAfterConflict() noexcept {
    try {
        refreshSnapshots();
    } catch (...) {
        // The conflict is the error to report; the collection stays as it was.
    }
}

void EditSession::replaceBase(SnapshotId gone, std::optional<SnapshotId> instead) {
    const auto swap = [&](std::optional<SnapshotId>& base) {
        if (base == gone) {
            base = instead;
        }
    };
    swap(base_);
    swap(savedBase_);
    if (baseline_) {
        swap(baseline_->base);
    }
    for (HistoryStep& step : history_) {
        swap(step.baseSnapshot);
    }
}

SnapshotId EditSession::createSnapshot(std::string name) {
    // The photograph as settling a gesture will leave it; matched before anything is written.
    Photo next = editing() ? withLensRecorded(baseline_->negative.lens, photo_) : photo_;
    Snapshot draft;
    draft.parent = base_;
    draft.name = std::move(name);
    draft.created = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    draft.state = next.state();
    draft.negative = next.negative();
    SnapshotUpdate update;
    try {
        update = addSnapshot(photo_.path(), snapshots_.revision, std::move(draft));
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
    // Written: now the session changes. The gesture is settled first, and the step it makes,
    // or the position it leaves current, is the one the new snapshot anchors.
    if (editing()) {
        commitPhoto(std::move(next), EditOrigin::Edit, {});
    }
    snapshots_ = std::move(update.collection);
    base_ = update.id;
    history_[position_].baseSnapshot = update.id;
    return update.id;
}

void EditSession::restoreSnapshot(SnapshotId id) {
    const Snapshot* found = snapshots_.find(id);
    if (found == nullptr) {
        throw std::invalid_argument("There is no snapshot " + std::to_string(id.value));
    }
    // Copied: committing below cannot change the collection, but a copy keeps that obvious.
    const Snapshot snapshot = *found;
    // The snapshot's own recipe, exactly: no lens is matched again. Built before the open edit
    // is committed, so a recipe that is refused leaves the edit open.
    Photo next = photo_.with(snapshot.negative).with(snapshot.state);
    if (editing()) {
        commit();
        next = photo_.with(snapshot.negative).with(snapshot.state);
    }
    if (next.state() == photo_.state() && next.negative() == photo_.negative() && base_ == id) {
        return;
    }
    photo_ = std::move(next);
    base_ = id;
    appendStep({photo_.state(), EditOrigin::Restore, snapshot.name, photo_.negative(), base_});
}

void EditSession::renameSnapshot(SnapshotId id, std::string name) {
    if (snapshots_.find(id) == nullptr) {
        throw std::invalid_argument("There is no snapshot " + std::to_string(id.value));
    }
    try {
        snapshots_ = arraw::renameSnapshot(photo_.path(), snapshots_.revision, id, std::move(name))
                         .collection;
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
}

void EditSession::deleteSnapshot(SnapshotId id) {
    const Snapshot* found = snapshots_.find(id);
    if (found == nullptr) {
        throw std::invalid_argument("There is no snapshot " + std::to_string(id.value));
    }
    const std::optional<SnapshotId> parent = found->parent;
    try {
        snapshots_ = arraw::deleteSnapshot(photo_.path(), snapshots_.revision, id).collection;
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
    replaceBase(id, parent);
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
