#pragma once

#include <DevelopState.h>
#include <LocalAdjustments.h>
#include <Photo.h>
#include <SettingDescriptors.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw {

/// @brief Kind of change that made a history step, beyond what its states show.
///
/// Front ends word a step from this, the detail and ::arraw::describeChange, so
/// the list never claims a change the states do not show (ADR 022).
enum class EditOrigin {
    Opened, ///< The first step: the photograph as opened, or after discardChanges().
    Edit,   ///< An ordinary edit, worded from ::arraw::describeChange.
    Paste,  ///< Paste Settings.
    Preset, ///< A preset applied; the detail is the preset's name.
    Reset,  ///< A reset to defaults.
    Crop,   ///< A crop-mode session committed as one step.
};

/// @brief One entry of an edit history: the state it left the photograph in, and why.
struct HistoryStep {
    /// @brief Whole develop state after the step.
    DevelopState state;

    /// @brief Kind of change the step was.
    EditOrigin origin = EditOrigin::Edit;

    /// @brief What the origin applies to, such as a preset's name; empty otherwise.
    ///
    /// A name to show, not wording: the sentence is the front end's.
    std::string detail;

    friend bool operator==(const HistoryStep&, const HistoryStep&) = default;
};

/// @brief What changed in one local adjustment that two develop states both hold.
struct MaskChange {
    /// @brief Id of the adjustment.
    LocalAdjustmentId id{};

    /// @brief Whether the name differs.
    bool name = false;

    /// @brief Whether it was turned on or off.
    bool enabled = false;

    /// @brief Whether the opacity differs.
    bool opacity = false;

    /// @brief Whether the weight was inverted or restored.
    bool invert = false;

    /// @brief Whether the shape differs: its geometry, or its kind. False for two brush masks,
    /// which differ in ::arraw::MaskChange::strokes.
    bool shape = false;

    /// @brief Whether the brush's strokes differ (both masks are brushes).
    bool strokes = false;

    /// @brief Keys of ::arraw::localAdjustmentDescriptors whose deltas differ, in table order.
    std::vector<std::string_view> deltas;

    friend bool operator==(const MaskChange&, const MaskChange&) = default;
};

/// @brief Local adjustments two develop states differ in, for naming a history step (ADR 044).
struct LocalChange {
    /// @brief Ids the second state holds and the first does not, in the second's order.
    std::vector<LocalAdjustmentId> added;

    /// @brief Ids the first state holds and the second does not, in the first's order.
    std::vector<LocalAdjustmentId> removed;

    /// @brief Whether the adjustments both hold are in another order.
    bool reordered = false;

    /// @brief The adjustments both hold that differ, in the second state's order.
    std::vector<MaskChange> changed;

    /// @brief Tells whether the two lists are the same.
    [[nodiscard]] bool empty() const noexcept {
        return added.empty() && removed.empty() && !reordered && changed.empty();
    }

    friend bool operator==(const LocalChange&, const LocalChange&) = default;
};

/// @brief Settings and local adjustments two develop states differ in, for naming a history step.
struct ChangeDescription {
    /// @brief Keys of ::arraw::developSettingDescriptors whose values differ, in table order.
    std::vector<std::string_view> keys;

    /// @brief Group every key belongs to; empty when there are no keys or they span groups.
    std::optional<SettingGroup> group;

    /// @brief How the local adjustments differ; empty when they do not. The counter alone is not
    /// described.
    LocalChange local;

    friend bool operator==(const ChangeDescription&, const ChangeDescription&) = default;
};

/// @brief Lists the settings and the local adjustments two develop states differ in.
///
/// Compares each row of ::arraw::developSettingDescriptors through its
/// accessor, so a new setting is covered by its row alone. Rows of the
/// photograph's own settings, such as the grain seed, are compared like any
/// other: a step that changes only the seed still names it. The local part
/// matches adjustments by id (ADR 044): ids added, removed or reordered, and, for each id both
/// states hold, what differs in it.
/// @param before State a step started from.
/// @param after State it left.
[[nodiscard]] ChangeDescription describeChange(const DevelopState& before,
                                               const DevelopState& after);

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
/// History is a list of steps and a position in it, the opening state first.
/// Undo and redo move the position, and so does goTo(): navigating is not an
/// edit, so it adds nothing and drops nothing. The next edit after moving back
/// drops the steps beyond the position.
///
/// The session also holds the photograph as its sidecar holds it, saved(), so
/// that "unsaved changes" is a comparison and not a flag (ADR 030). Develop
/// edits reach the sidecar through save(); marks reach it at once, through
/// setMarks().
class EditSession {
public:
    /// @brief Starts editing a photograph, with a history of just its opening state.
    /// @param photo Document to edit, usually from ::arraw::openPhoto, which is
    /// taken to be what its sidecar holds (or defaults, when there is none).
    explicit EditSession(Photo photo)
        : photo_(photo), saved_(std::move(photo)),
          history_{{saved_.state(), EditOrigin::Opened, {}}} {}

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
    /// An edit that ends where it started leaves no step. A new step drops
    /// every step after the position, so what could be redone is gone.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::logic_error if no edit is open.
    void commit(EditOrigin origin = EditOrigin::Edit, std::string detail = {});

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
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if @p state is not valid; nothing changes.
    void setState(DevelopState state, EditOrigin origin = EditOrigin::Edit,
                  std::string detail = {});

    /// @brief Steps taken so far, the opening state first.
    ///
    /// Steps after position() can be redone. While an edit is open they do not
    /// include it.
    [[nodiscard]] const std::vector<HistoryStep>& history() const noexcept {
        return history_;
    }

    /// @brief Index in history() of the step the document is at, when no edit is open.
    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    /// @brief Moves to a step, keeping every step in the list.
    ///
    /// An open edit is committed first, and one that changed something drops
    /// the steps after the position, so @p index is taken after that commit.
    /// Navigating is not an edit: it adds no step and drops none.
    /// @param index Index in history() as it stands after the commit.
    /// @throws std::out_of_range if @p index is not in history(); nothing changes
    /// but the commit.
    void goTo(std::size_t index);

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
    ///
    /// History starts again from the saved state, as an Opened step.
    void discardChanges();

private:
    Photo photo_;

    /// Photograph as the sidecar holds it.
    Photo saved_;

    /// State the open edit started from; empty when no edit is open.
    std::optional<DevelopState> baseline_;

    /// Steps taken, the opening state first; never empty.
    std::vector<HistoryStep> history_;

    /// Index in history_ of the step the document is at, when no edit is open.
    std::size_t position_ = 0;
};

} // namespace arraw
