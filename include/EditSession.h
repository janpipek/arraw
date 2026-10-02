#pragma once

#include <DevelopState.h>
#include <Photo.h>

#include <optional>
#include <utility>
#include <vector>

namespace arraw {

/// @brief One photograph being edited: the document as it stands now, and how it got there.
///
/// A session exists only while a photograph is open, so there is no empty
/// session: "nothing open" is a caller holding no session, not a session
/// holding nothing. It owns no pixels; decoding and rendering belong to
/// whoever displays or exports the photograph (ADR 001).
///
/// It is where edits happen, as opposed to ::arraw::Photo, which is a value
/// that edits produce. An edit is opened with begin(), changed any number of
/// times with update() and closed with commit() or cancel(), so that a slider
/// drag or a brush stroke is one history step rather than a hundred (ADR 022).
/// History holds develop states only: marks are not undone. Unsaved-change
/// tracking will live here too.
class EditSession {
public:
    /// @brief Starts editing a photograph, with no history.
    /// @param photo Document to edit, usually from ::arraw::openPhoto.
    explicit EditSession(Photo photo) : photo_(std::move(photo)) {}

    /// @brief Current state of the document, including an edit in progress.
    ///
    /// This is what to render: during a drag it already carries the latest update.
    [[nodiscard]] const Photo& photo() const noexcept {
        return photo_;
    }

    /// @brief Opens an edit, remembering the state it started from.
    ///
    /// An edit already open is committed first.
    void begin();

    /// @brief Changes the state provisionally, inside the open edit.
    /// @param state State the document carries from now on, until the next update.
    /// @throws std::logic_error if no edit is open.
    /// @throws std::invalid_argument if @p state is not valid; nothing changes.
    void update(DevelopState state);

    /// @brief Closes the open edit as one history step.
    ///
    /// An edit that ends where it started leaves no step. A new step clears
    /// what could be redone.
    /// @throws std::logic_error if no edit is open.
    void commit();

    /// @brief Closes the open edit, restoring the state it started from.
    /// @throws std::logic_error if no edit is open.
    void cancel();

    /// @brief Whether an edit is open.
    [[nodiscard]] bool editing() const noexcept {
        return baseline_.has_value();
    }

    /// @brief Replaces the develop state as one history step.
    ///
    /// The same as begin(), update() and commit() in turn: for changes that
    /// happen at once, such as a reset or a choice from a list.
    /// @param state State the document carries from now on.
    /// @throws std::invalid_argument if @p state is not valid; nothing changes.
    void setState(DevelopState state);

    /// @brief Whether undo() has a step to take back, counting an open edit that changed something.
    [[nodiscard]] bool canUndo() const noexcept;

    /// @brief Whether redo() has a step to bring back, after committing an open edit.
    [[nodiscard]] bool canRedo() const noexcept;

    /// @brief Takes back the latest history step.
    ///
    /// An open edit is committed first, so undoing in the middle of a drag
    /// reverts the drag and leaves it to redo.
    /// @throws std::logic_error if there is nothing to undo.
    void undo();

    /// @brief Brings back the step undo() last took back.
    ///
    /// An open edit is committed first, which clears what could be redone.
    /// @throws std::logic_error if there is nothing to redo.
    void redo();

private:
    Photo photo_;

    /// State the open edit started from; empty when no edit is open.
    std::optional<DevelopState> baseline_;

    /// States before each step, the latest last.
    std::vector<DevelopState> undo_;

    /// States after each step undone, the latest undone last.
    std::vector<DevelopState> redo_;
};

} // namespace arraw
