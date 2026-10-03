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
/// History holds develop states only: marks are not undone.
///
/// The session also holds the photograph as its sidecar holds it, saved(), so
/// that "unsaved changes" is a comparison and not a flag (ADR 030). Develop
/// edits reach the sidecar through save(); marks reach it at once, through
/// setMarks().
class EditSession {
public:
    /// @brief Starts editing a photograph, with no history.
    /// @param photo Document to edit, usually from ::arraw::openPhoto, which is
    /// taken to be what its sidecar holds (or defaults, when there is none).
    explicit EditSession(Photo photo) : photo_(photo), saved_(std::move(photo)) {}

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

    /// @brief Photograph as its sidecar holds it: the baseline of unsaved changes.
    [[nodiscard]] const Photo& saved() const noexcept {
        return saved_;
    }

    /// @brief Whether the develop state, an open edit included, differs from the saved one.
    ///
    /// Marks never count: they are written as they change (see setMarks()).
    [[nodiscard]] bool hasUnsavedChanges() const {
        return photo_.state() != saved_.state();
    }

    /// @brief Writes the photograph to its sidecar and makes that the saved state.
    ///
    /// An open edit is committed first. History is kept: saving is not an edit.
    /// @throws std::runtime_error as ::arraw::writeSidecar does; nothing changes.
    void save();

    /// @brief Sets the marks, writing them to the sidecar at once.
    ///
    /// Writes the saved photograph with the new marks, so develop edits not yet
    /// saved stay out of the file, then updates saved() and photo(). This is
    /// not an undo step (ADR 021, 022).
    /// @param marks Marks the photograph carries from now on.
    /// @throws std::invalid_argument if @p marks are not valid.
    /// @throws std::runtime_error as ::arraw::writeSidecar does.
    /// Either way nothing changes.
    void setMarks(PhotoMarks marks);

    /// @brief Returns the develop state to the saved one, dropping history and any open edit.
    void discardChanges();

private:
    Photo photo_;

    /// Photograph as the sidecar holds it.
    Photo saved_;

    /// State the open edit started from; empty when no edit is open.
    std::optional<DevelopState> baseline_;

    /// States before each step, the latest last.
    std::vector<DevelopState> undo_;

    /// States after each step undone, the latest undone last.
    std::vector<DevelopState> redo_;
};

} // namespace arraw
