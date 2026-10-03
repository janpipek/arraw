#include "EditSession.h"

#include <Sidecar.h>

#include <stdexcept>
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

void EditSession::commit() {
    if (!editing()) {
        throw std::logic_error("There is no open edit to commit");
    }
    DevelopState before = std::move(*baseline_);
    baseline_.reset();
    if (before != photo_.state()) {
        undo_.push_back(std::move(before));
        redo_.clear();
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

void EditSession::setState(DevelopState state) {
    // Checked before anything opens, so an invalid state leaves no edit behind.
    Photo next = photo_.with(std::move(state));
    begin();
    photo_ = std::move(next);
    commit();
}

bool EditSession::canUndo() const noexcept {
    return !undo_.empty() || (editing() && *baseline_ != photo_.state());
}

bool EditSession::canRedo() const noexcept {
    // Redoing commits an open edit first, and one that changed something clears redo.
    return !redo_.empty() && (!editing() || *baseline_ == photo_.state());
}

void EditSession::undo() {
    if (editing()) {
        commit();
    }
    if (undo_.empty()) {
        throw std::logic_error("There is nothing to undo");
    }
    Photo previous = photo_.with(undo_.back());
    redo_.push_back(photo_.state());
    undo_.pop_back();
    photo_ = std::move(previous);
}

void EditSession::redo() {
    if (editing()) {
        commit();
    }
    if (redo_.empty()) {
        throw std::logic_error("There is nothing to redo");
    }
    Photo next = photo_.with(redo_.back());
    undo_.push_back(photo_.state());
    redo_.pop_back();
    photo_ = std::move(next);
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
    undo_.clear();
    redo_.clear();
}
