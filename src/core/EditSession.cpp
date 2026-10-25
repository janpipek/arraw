#include "EditSession.h"

#include <LensResolution.h>
#include <LocalAdjustmentEdits.h>
#include <Sidecar.h>
#include <SnapshotThumbnails.h>
#include <SnapshotWrites.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    unreadable_ = contents ? contents->unreadableSnapshots : 0;
}

void EditSession::adoptSnapshots(SnapshotCollection fresh, std::size_t unreadable) {
    if (fresh.revision < snapshots_.revision) {
        return; // An older result must not move the session backwards.
    }
    adopt(std::move(fresh));
    unreadable_ = unreadable;
}

std::vector<SnapshotId> EditSession::snapshotsNeedingThumbnails() const {
    std::vector<SnapshotId> wanted;
    for (const Snapshot& snapshot : snapshots_.entries) {
        if (!hasCurrentThumbnail(photo_.path(), snapshot)) {
            wanted.push_back(snapshot.id);
        }
    }
    return wanted;
}

bool EditSession::applySnapshotThumbnail(SnapshotId id, SnapshotThumbnail thumbnail) {
    // From the recipe the session holds, worked out by the write before the lock (ADR 056,
    // decision 6). The file now holds exactly this; keeping the copy equal to it is also what
    // lets a reader of the sidecar tell this session's own write from someone else's change.
    const auto write = prepareSnapshotThumbnail(id, std::move(thumbnail));
    if (!write) {
        return false;
    }
    const SnapshotWriteOutcome outcome = writeSnapshotChange(*write);
    if (!outcome.written) {
        return false;
    }
    finishSnapshotWrite(*write, outcome);
    return true;
}

void EditSession::refreshAfterConflict() noexcept {
    try {
        refreshSnapshots();
    } catch (...) {
        // The conflict is the error to report; the collection stays as it was.
    }
}

SnapshotWrite EditSession::createWrite(std::string name, const Photo& look) const {
    Snapshot draft;
    draft.parent = base_;
    draft.name = std::move(name);
    draft.created = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    draft.state = look.state();
    draft.negative = look.negative();
    // Checked now, as the write does, so that an invalid name fails before anything is queued.
    Snapshot probe = draft;
    probe.id = SnapshotId{std::numeric_limits<std::uint64_t>::max()};
    validate(probe);
    return {photo_.path(), snapshots_.revision, SnapshotCreate{std::move(draft), position_}};
}

SnapshotWrite EditSession::prepareCreateSnapshot(std::string name) const {
    if (editing()) {
        throw std::logic_error("An open edit must be settled before a snapshot is prepared");
    }
    return createWrite(std::move(name), photo_);
}

SnapshotWrite EditSession::prepareRenameSnapshot(SnapshotId id, std::string name) const {
    const Snapshot* found = snapshots_.find(id);
    if (found == nullptr) {
        throw std::invalid_argument("There is no snapshot " + std::to_string(id.value));
    }
    Snapshot renamed = *found;
    renamed.name = name;
    validate(renamed);
    return {photo_.path(), snapshots_.revision, SnapshotRename{id, std::move(name)}};
}

SnapshotWrite EditSession::prepareDeleteSnapshot(SnapshotId id) const {
    if (snapshots_.find(id) == nullptr) {
        throw std::invalid_argument("There is no snapshot " + std::to_string(id.value));
    }
    return {photo_.path(), snapshots_.revision, SnapshotDelete{id}};
}

SnapshotWrite EditSession::prepareDiscardUnreadableSnapshots() const {
    return {photo_.path(), snapshots_.revision, SnapshotDiscardUnreadable{}};
}

std::optional<SnapshotWrite>
EditSession::prepareSnapshotThumbnail(SnapshotId id, SnapshotThumbnail thumbnail) const {
    validate(thumbnail);
    const Snapshot* found = snapshots_.find(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return SnapshotWrite{
        photo_.path(), snapshots_.revision,
        SnapshotThumbnailPatch{id, std::move(thumbnail), found->state, found->negative}};
}

bool EditSession::finishSnapshotWrite(const SnapshotWrite& write,
                                      const SnapshotWriteOutcome& outcome) {
    if (write.photo != photo_.path()) {
        throw std::invalid_argument("The write is for " + write.photo.string() +
                                    ", not for this session's photograph");
    }
    bool anchored = true;
    std::visit(
        [&](const auto& change) {
            using Change = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Change, SnapshotThumbnailPatch>) {
                if (!outcome.written) {
                    return;
                }
                // The recipe of an id never changes, so no key is checked again.
                const auto found = std::ranges::find(snapshots_.entries, change.id, &Snapshot::id);
                if (found != snapshots_.entries.end()) {
                    found->thumbnail = change.thumbnail;
                }
            } else if (!outcome.update) {
                // A discard that found nothing unusable: the file holds none.
                if constexpr (std::is_same_v<Change, SnapshotDiscardUnreadable>) {
                    unreadable_ = 0;
                }
            } else {
                const SnapshotUpdate& update = *outcome.update;
                // An older result must not move the session backwards (a newer one was adopted
                // in between); the references are kept as they are then.
                if (update.collection.revision >= snapshots_.revision) {
                    adopt(update.collection);
                    unreadable_ = update.unreadable;
                }
                if constexpr (std::is_same_v<Change, SnapshotCreate>) {
                    const Snapshot& draft = change.draft;
                    anchored = snapshots_.find(update.id) != nullptr && !editing() &&
                               position_ == change.position && photo_.state() == draft.state &&
                               photo_.negative() == draft.negative && base_ == draft.parent;
                    if (anchored) {
                        base_ = update.id;
                        history_[position_].baseSnapshot = update.id;
                    }
                }
            }
        },
        write.change);
    return anchored;
}

SnapshotId EditSession::createSnapshot(std::string name) {
    // The photograph as settling a gesture will leave it; matched before anything is written.
    Photo next = editing() ? withLensRecorded(baseline_->negative.lens, photo_) : photo_;
    SnapshotWrite write = createWrite(std::move(name), next);
    SnapshotWriteOutcome outcome;
    try {
        outcome = writeSnapshotChange(write);
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
    // Written: now the session changes. The gesture is settled first, and the step it makes,
    // or the position it leaves current, is the one the new snapshot anchors.
    if (editing()) {
        commitPhoto(std::move(next), EditOrigin::Edit, {});
    }
    std::get<SnapshotCreate>(write.change).position = position_;
    finishSnapshotWrite(write, outcome);
    return outcome.update->id;
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
    const SnapshotWrite write = prepareRenameSnapshot(id, std::move(name));
    try {
        finishSnapshotWrite(write, writeSnapshotChange(write));
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
}

void EditSession::deleteSnapshot(SnapshotId id) {
    const SnapshotWrite write = prepareDeleteSnapshot(id);
    try {
        finishSnapshotWrite(write, writeSnapshotChange(write));
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
}

void EditSession::discardUnreadableSnapshots() {
    const SnapshotWrite write = prepareDiscardUnreadableSnapshots();
    try {
        finishSnapshotWrite(write, writeSnapshotChange(write));
    } catch (const SnapshotRevisionConflict&) {
        refreshAfterConflict();
        throw;
    }
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
