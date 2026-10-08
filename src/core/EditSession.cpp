#include "EditSession.h"

#include <Sidecar.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace arraw;

void EditSession::begin() {
    if (editing()) {
        commit();
    }
    baseline_ = photo_.state();
}

void EditSession::update(DevelopState state) {
    if (!editing()) {
        throw std::logic_error("An edit must be begun before it is updated");
    }
    photo_ = photo_.with(std::move(state));
}

void EditSession::commit(EditOrigin origin, std::string detail) {
    if (!editing()) {
        throw std::logic_error("There is no open edit to commit");
    }
    DevelopState before = std::move(*baseline_);
    baseline_.reset();
    if (before != photo_.state()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(position_) + 1,
                       history_.end());
        history_.push_back({photo_.state(), origin, std::move(detail)});
        ++position_;
    }
}

void EditSession::cancel() {
    if (!editing()) {
        throw std::logic_error("There is no open edit to cancel");
    }
    // The baseline was valid when it was taken, so this cannot throw on its contents.
    photo_ = photo_.with(std::move(*baseline_));
    baseline_.reset();
}

void EditSession::setState(DevelopState state, EditOrigin origin, std::string detail) {
    // Checked before anything opens, so an invalid state leaves no edit behind.
    Photo next = photo_.with(std::move(state));
    begin();
    photo_ = std::move(next);
    commit(origin, std::move(detail));
}

bool EditSession::canUndo() const noexcept {
    return position_ > 0 || (editing() && *baseline_ != photo_.state());
}

bool EditSession::canRedo() const noexcept {
    // Redoing commits an open edit first, and one that changed something clears redo.
    return position_ + 1 < history_.size() && (!editing() || *baseline_ == photo_.state());
}

void EditSession::goTo(std::size_t index) {
    if (editing()) {
        commit();
    }
    if (index >= history_.size()) {
        throw std::out_of_range("There is no history step " + std::to_string(index));
    }
    photo_ = photo_.with(history_[index].state);
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
    // Written first, so a failure leaves even an open edit open.
    writeSidecar(photo_);
    saved_ = photo_;
    if (editing()) {
        commit();
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
    history_ = {{saved_.state(), EditOrigin::Opened, {}}};
    position_ = 0;
}

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
    return change;
}
