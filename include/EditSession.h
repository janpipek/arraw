#pragma once

#include <DevelopState.h>
#include <LocalAdjustments.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingDescriptors.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw {

class LensDatabase;

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

    /// @brief Negative settings after the step (ADR 049).
    ///
    /// Only ::arraw::EditSession::setNegative, the two-part ::arraw::EditSession::setState and
    /// ::arraw::EditSession::updateNegative change it from one step to the next.
    NegativeSettings negative;

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

    /// @brief Keys of the negative settings that differ, in the order `"demosaic"`,
    /// `"lensDistortion"`, `"lensVignetting"`, `"lensLateralCa"` and `"lens"` (the recorded lens);
    /// empty when the two negatives are equal. Filled only by the overload over history steps.
    std::vector<std::string_view> negative;

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

/// @brief Lists what two history steps differ in: the develop state and the negative.
///
/// As the overload over states, plus ::arraw::ChangeDescription::negative (ADR 049).
/// @param before Step a change started from.
/// @param after Step it left.
[[nodiscard]] ChangeDescription describeChange(const HistoryStep& before, const HistoryStep& after);

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
/// History holds develop states and negative settings: marks are not undone. An open edit
/// remembers both, so update() and updateNegative() may change either and commit() makes one
/// step if either differs; cancel() restores both. setNegative() makes a step of its own
/// (ADR 049, 050).
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
    /// @param lensDatabase Database the session records lens matches in, as setLensDatabase().
    explicit EditSession(Photo photo, const LensDatabase* lensDatabase = nullptr)
        : photo_(photo), saved_(std::move(photo)),
          history_{{saved_.state(), EditOrigin::Opened, {}, saved_.negative()}},
          lensDatabase_(lensDatabase) {}

    /// @brief Sets the lens database the session records matches in.
    ///
    /// The session records the lens match itself (ADR 050): a step that changes the lens settings
    /// and leaves a correction on with no lens recorded records ::arraw::recordLensMatch in that
    /// same step. That is commit() (so begin(), goTo(), undo(), redo() and save() too),
    /// setNegative() and the two-part setState(), before they ask whether anything changed.
    /// update(), updateNegative(), cancel(), discardChanges() and navigation never record, and
    /// neither does a step that leaves the lens settings alone. A recorded lens, matched or
    /// chosen, is never replaced. With no database the step is made as given. Only the next step
    /// uses a new database: nothing already made or open is touched. Not thread-safe, like the
    /// rest of the session.
    /// @param database Database to match in, which must outlive the session or be detached first;
    /// null for none. Not owned.
    void setLensDatabase(const LensDatabase* database) noexcept {
        lensDatabase_ = database;
    }

    /// @brief Gives the lens database the session records matches in, or null.
    [[nodiscard]] const LensDatabase* lensDatabase() const noexcept {
        return lensDatabase_;
    }

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

    /// @brief Changes the negative settings provisionally, inside the open edit.
    ///
    /// For a slider that drags a negative setting, such as the vignetting amount (ADR 050): the
    /// drag is one history step with any develop updates. photo() carries the latest negative,
    /// and a caller that decodes compares it with the one its pixels were decoded with. The lens
    /// match is not recorded here, but by commit() (see setLensDatabase()).
    /// @param negative Negative settings the document carries from now on, until the next update.
    /// @throws std::logic_error if no edit is open.
    /// @throws std::invalid_argument if @p negative is not valid; nothing changes.
    void updateNegative(NegativeSettings negative);

    /// @brief Closes the open edit as one history step.
    ///
    /// An edit that ends where it started, in both the state and the negative, leaves no step. A
    /// new step drops every step after the position, so what could be redone is gone. A step that
    /// changes the lens settings records the lens match first (see setLensDatabase()), so an
    /// edit that only adds the record still makes a step. If matching throws, the edit stays open
    /// and the history is as it was.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::logic_error if no edit is open.
    void commit(EditOrigin origin = EditOrigin::Edit, std::string detail = {});

    /// @brief Closes the open edit, restoring the state and negative it started from.
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

    /// @brief Replaces the negative settings as one history step.
    ///
    /// An open edit is committed first. Setting the negative it already has leaves no step, after
    /// the lens match is recorded when the step changes the lens settings (see setLensDatabase()).
    /// A new step drops every step after the position. A caller that decodes (the window) compares
    /// photo().negative() with the one its pixels were decoded with after this, as after undo.
    /// @param negative Negative settings the document carries from now on.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if @p negative is not valid; nothing changes.
    void setNegative(NegativeSettings negative, EditOrigin origin = EditOrigin::Edit,
                     std::string detail = {});

    /// @brief Replaces the negative settings and the develop state as one history step.
    ///
    /// For a paste or a preset, which carry both. An open edit is committed first; when neither
    /// part differs there is no step. The lens match is recorded as for setNegative(); if matching
    /// throws, the edit and the history stay as they were.
    /// @param negative Negative settings the document carries from now on.
    /// @param state State the document carries from now on.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if either is not valid; nothing changes.
    void setState(NegativeSettings negative, DevelopState state,
                  EditOrigin origin = EditOrigin::Edit, std::string detail = {});

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

    /// @brief Whether the develop state or the negative, an open edit included, differs from
    /// the saved one.
    ///
    /// Marks never count: they are written as they change (see setMarks()).
    [[nodiscard]] bool hasUnsavedChanges() const {
        return photo_.state() != saved_.state() || photo_.negative() != saved_.negative();
    }

    /// @brief Writes the photograph to its sidecar and makes that the saved state.
    ///
    /// An open edit is committed first. History is kept: saving is not an edit. With an open edit
    /// that is due a lens record, the file holds the photograph as the commit leaves it, with the
    /// match recorded, so nothing is unsaved straight after.
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

    /// @brief Returns the develop state and the negative to the saved ones, dropping history and
    /// any open edit.
    ///
    /// History starts again from the saved state, as an Opened step.
    void discardChanges();

private:
    Photo photo_;

    /// Photograph as the sidecar holds it.
    Photo saved_;

    /// What an open edit started from.
    struct Baseline {
        DevelopState state;        ///< Develop state at begin().
        NegativeSettings negative; ///< Negative settings at begin().
    };

    /// Gives @p after with the lens match recorded when its lens settings differ from @p before.
    [[nodiscard]] Photo withLensRecorded(const LensSettings& before, const Photo& after) const;

    /// Makes @p next, which already carries any record, the photograph and ends the open edit.
    void commitPhoto(Photo next, EditOrigin origin, std::string detail);

    /// Tells whether the open edit is back where it began. Requires an open edit.
    [[nodiscard]] bool baselineIsCurrent() const noexcept;

    /// Start of the open edit; empty when no edit is open.
    std::optional<Baseline> baseline_;

    /// Steps taken, the opening state first; never empty.
    std::vector<HistoryStep> history_;

    /// Index in history_ of the step the document is at, when no edit is open.
    std::size_t position_ = 0;

    /// Database the lens match is recorded in; not owned, may be null.
    const LensDatabase* lensDatabase_ = nullptr;
};

} // namespace arraw
