#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <EditSession.h>
#include <SettingDescriptors.h>
#include <Sidecar.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace arraw;

/// A session is where a photograph is edited; the photograph itself stays a
/// value, so editing replaces the document rather than changing one in place.

namespace {

/// @brief Builds a document without a file, since a session never reads one.
Photo testPhoto() {
    return {"photo.dng", ImageMetadata{ImageSize{32, 24}, workingEncoding}};
}

} // namespace

TEST_CASE("A session starts from the photograph it is given", "[session]") {
    const Photo photo = testPhoto();
    const EditSession session(photo);

    REQUIRE(session.photo() == photo);
}

TEST_CASE("Changing the state replaces it and keeps the photograph", "[session]") {
    const Photo original = testPhoto();
    EditSession session(original);
    DevelopState brighter;
    brighter.settings.tone.exposure = 1.0F;

    session.setState(brighter);

    REQUIRE(session.photo().state() == brighter);
    REQUIRE(session.photo().path() == original.path());
    REQUIRE(session.photo().metadata() == original.metadata());
    REQUIRE(original.state() == DevelopState{});
}

namespace {

/// @brief Builds a state that differs from the defaults by its exposure.
DevelopState exposed(float exposure) {
    DevelopState state;
    state.settings.tone.exposure = exposure;
    return state;
}

} // namespace

TEST_CASE("A new session has nothing to undo or redo", "[session][history]") {
    EditSession session(testPhoto());

    REQUIRE_FALSE(session.editing());
    REQUIRE_FALSE(session.canUndo());
    REQUIRE_FALSE(session.canRedo());
    REQUIRE_THROWS_AS(session.undo(), std::logic_error);
    REQUIRE_THROWS_AS(session.redo(), std::logic_error);
}

TEST_CASE("An edit shows every update and commits as one step", "[session][history]") {
    EditSession session(testPhoto());

    session.begin();
    REQUIRE(session.editing());
    for (const float exposure : {0.25F, 0.5F, 1.0F}) {
        session.update(exposed(exposure));
        REQUIRE(session.photo().state() == exposed(exposure));
    }
    session.commit();

    REQUIRE_FALSE(session.editing());
    REQUIRE(session.photo().state() == exposed(1.0F));
    session.undo();
    REQUIRE(session.photo().state() == DevelopState{});
    REQUIRE_FALSE(session.canUndo());
}

TEST_CASE("An edit that ends where it began leaves no step", "[session][history]") {
    EditSession session(testPhoto());

    session.begin();
    session.update(exposed(1.0F));
    session.update(DevelopState{});
    session.commit();

    REQUIRE_FALSE(session.canUndo());
}

TEST_CASE("Cancelling an edit restores where it began", "[session][history]") {
    EditSession session(testPhoto());
    session.setState(exposed(0.5F));

    session.begin();
    session.update(exposed(2.0F));
    session.cancel();

    REQUIRE_FALSE(session.editing());
    REQUIRE(session.photo().state() == exposed(0.5F));
    session.undo();
    REQUIRE(session.photo().state() == DevelopState{});
    REQUIRE_FALSE(session.canUndo());
}

TEST_CASE("Updating, committing or cancelling needs an open edit", "[session][history]") {
    EditSession session(testPhoto());

    REQUIRE_THROWS_AS(session.update(exposed(1.0F)), std::logic_error);
    REQUIRE_THROWS_AS(session.commit(), std::logic_error);
    REQUIRE_THROWS_AS(session.cancel(), std::logic_error);
    REQUIRE(session.photo().state() == DevelopState{});
}

TEST_CASE("An invalid update changes nothing and keeps the edit open", "[session][history]") {
    EditSession session(testPhoto());
    session.begin();
    session.update(exposed(1.0F));

    REQUIRE_THROWS_AS(session.update(exposed(99.0F)), std::invalid_argument);

    REQUIRE(session.editing());
    REQUIRE(session.photo().state() == exposed(1.0F));
}

TEST_CASE("An invalid state set at once opens no edit and leaves no step", "[session][history]") {
    EditSession session(testPhoto());

    REQUIRE_THROWS_AS(session.setState(exposed(99.0F)), std::invalid_argument);

    REQUIRE_FALSE(session.editing());
    REQUIRE_FALSE(session.canUndo());
}

TEST_CASE("Beginning while an edit is open commits it first", "[session][history]") {
    EditSession session(testPhoto());
    session.begin();
    session.update(exposed(1.0F));

    session.begin();
    session.update(exposed(2.0F));
    session.commit();

    session.undo();
    REQUIRE(session.photo().state() == exposed(1.0F));
    session.undo();
    REQUIRE(session.photo().state() == DevelopState{});
}

TEST_CASE("Undo and redo walk the steps both ways", "[session][history]") {
    EditSession session(testPhoto());
    session.setState(exposed(1.0F));
    session.setState(exposed(2.0F));

    session.undo();
    REQUIRE(session.photo().state() == exposed(1.0F));
    REQUIRE(session.canRedo());
    session.undo();
    REQUIRE(session.photo().state() == DevelopState{});
    session.redo();
    session.redo();
    REQUIRE(session.photo().state() == exposed(2.0F));
    REQUIRE_FALSE(session.canRedo());
}

TEST_CASE("A new step clears what could be redone", "[session][history]") {
    EditSession session(testPhoto());
    session.setState(exposed(1.0F));
    session.undo();

    session.setState(exposed(3.0F));

    REQUIRE_FALSE(session.canRedo());
    session.undo();
    REQUIRE(session.photo().state() == DevelopState{});
}

TEST_CASE("Undoing in the middle of an edit reverts it and leaves it to redo",
          "[session][history]") {
    EditSession session(testPhoto());
    session.setState(exposed(1.0F));
    session.begin();
    session.update(exposed(2.0F));
    REQUIRE(session.canUndo());

    session.undo();

    REQUIRE_FALSE(session.editing());
    REQUIRE(session.photo().state() == exposed(1.0F));
    session.redo();
    REQUIRE(session.photo().state() == exposed(2.0F));
}

TEST_CASE("Redo is still offered while an open edit has changed nothing", "[session][history]") {
    EditSession session(testPhoto());
    session.setState(exposed(1.0F));
    session.undo();

    session.begin();
    REQUIRE(session.canRedo());
    session.update(exposed(2.0F));
    REQUIRE_FALSE(session.canRedo());
    session.update(DevelopState{});
    REQUIRE(session.canRedo());

    session.redo();
    REQUIRE(session.photo().state() == exposed(1.0F));
}

namespace {

/// @brief Copies a RAW fixture into a directory, so that a sidecar can sit beside it.
std::filesystem::path rawIn(const test::TempDir& directory) {
    const std::filesystem::path path = directory.file("IMG_1.dng");
    std::filesystem::copy_file(test::fixture("linear-32x24-neutral.dng"), path);
    return path;
}

} // namespace

TEST_CASE("A fresh session has no unsaved changes", "[session][save]") {
    const test::TempDir directory;
    const EditSession session(openPhoto(rawIn(directory)));

    REQUIRE_FALSE(session.hasUnsavedChanges());
    REQUIRE(session.saved() == session.photo());
}

TEST_CASE("An edit makes a session dirty and undoing it makes it clean", "[session][save]") {
    const test::TempDir directory;
    EditSession session(openPhoto(rawIn(directory)));

    session.setState(exposed(1.0F));
    REQUIRE(session.hasUnsavedChanges());
    session.undo();
    REQUIRE_FALSE(session.hasUnsavedChanges());
}

TEST_CASE("An open edit with a changed state counts as unsaved", "[session][save]") {
    const test::TempDir directory;
    EditSession session(openPhoto(rawIn(directory)));

    session.begin();
    REQUIRE_FALSE(session.hasUnsavedChanges());
    session.update(exposed(0.5F));
    REQUIRE(session.hasUnsavedChanges());
    session.update(session.saved().state());
    REQUIRE_FALSE(session.hasUnsavedChanges());
}

TEST_CASE("Saving writes the sidecar, cleans the session and keeps history", "[session][save]") {
    const test::TempDir directory;
    const std::filesystem::path path = rawIn(directory);
    EditSession session(openPhoto(path));
    session.begin();
    session.update(exposed(0.75F));

    session.save();

    REQUIRE_FALSE(session.editing());
    REQUIRE_FALSE(session.hasUnsavedChanges());
    REQUIRE(session.saved() == session.photo());
    REQUIRE(openPhoto(path) == session.photo());
    REQUIRE(session.canUndo());
    session.undo();
    REQUIRE(session.hasUnsavedChanges());
}

TEST_CASE("Marks are written at once and unsaved edits stay out of the file", "[session][save]") {
    const test::TempDir directory;
    const std::filesystem::path path = rawIn(directory);
    EditSession session(openPhoto(path));
    session.setState(exposed(1.0F));
    const PhotoMarks marks{.rating = 4, .label = ColorLabel::Blue};

    session.setMarks(marks);

    REQUIRE(session.photo().marks() == marks);
    REQUIRE(session.saved().marks() == marks);
    REQUIRE(session.photo().state() == exposed(1.0F));
    REQUIRE(session.saved().state() == defaultStateFor(session.saved().metadata().encoding));
    REQUIRE(session.hasUnsavedChanges());
    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->marks == marks);
    // The saved document is written with the marks: its state, not the unsaved one.
    REQUIRE(contents->state == session.saved().state());
    // Not an undo step: undoing takes back the exposure and keeps the marks.
    session.undo();
    REQUIRE(session.photo().marks() == marks);
    REQUIRE_FALSE(session.canUndo());
}

TEST_CASE("Saving after setting marks keeps the marks", "[session][save]") {
    const test::TempDir directory;
    const std::filesystem::path path = rawIn(directory);
    EditSession session(openPhoto(path));
    const PhotoMarks marks{.rating = 2};
    session.setMarks(marks);
    session.setState(exposed(0.5F));

    session.save();

    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->marks == marks);
    REQUIRE(contents->state == exposed(0.5F));
}

TEST_CASE("Invalid marks change nothing", "[session][save]") {
    const test::TempDir directory;
    EditSession session(openPhoto(rawIn(directory)));

    REQUIRE_THROWS_AS(session.setMarks({.rating = 9}), std::invalid_argument);
    REQUIRE(session.photo().marks() == PhotoMarks{});
    REQUIRE(session.saved().marks() == PhotoMarks{});
}

TEST_CASE("Discarding returns to the saved state and clears history", "[session][save]") {
    const test::TempDir directory;
    EditSession session(openPhoto(rawIn(directory)));
    session.setState(exposed(1.0F));
    session.save();
    session.setState(exposed(2.0F));
    session.undo();
    session.begin();
    session.update(exposed(3.0F));

    session.discardChanges();

    REQUIRE(session.photo() == session.saved());
    REQUIRE(session.photo().state() == exposed(1.0F));
    REQUIRE_FALSE(session.hasUnsavedChanges());
    REQUIRE_FALSE(session.editing());
    REQUIRE_FALSE(session.canUndo());
    REQUIRE_FALSE(session.canRedo());
}

TEST_CASE("A save that fails throws and leaves the session as it was", "[session][save]") {
    const test::TempDir directory;
    const std::filesystem::path path = rawIn(directory);
    EditSession session(openPhoto(path));
    session.begin();
    session.update(exposed(1.0F));
    // Not XML: writing refuses to replace it (ADR 019).
    std::ofstream(sidecarPath(path), std::ios::binary) << "not xml at all";

    REQUIRE_THROWS_AS(session.save(), std::runtime_error);
    REQUIRE_THROWS_AS(session.setMarks({.rating = 3}), std::runtime_error);

    REQUIRE(session.editing());
    REQUIRE(session.photo().state() == exposed(1.0F));
    REQUIRE(session.photo().marks() == PhotoMarks{});
    REQUIRE(session.saved().state() == defaultStateFor(session.saved().metadata().encoding));
    REQUIRE(session.saved().marks() == PhotoMarks{});
    REQUIRE(session.hasUnsavedChanges());
}

TEST_CASE("A save into a read-only directory throws and changes nothing", "[session][save]") {
    namespace fs = std::filesystem;
    const test::TempDir directory;
    const fs::path path = rawIn(directory);
    EditSession session(openPhoto(path));
    session.setState(exposed(1.0F));
    fs::permissions(directory.path(), fs::perms::owner_write, fs::perm_options::remove);
    const bool enforced = [&] {
        std::ofstream probe(directory.file("probe"));
        return !probe.is_open();
    }();
    if (enforced) {
        REQUIRE_THROWS_AS(session.save(), std::runtime_error);
        REQUIRE(session.hasUnsavedChanges());
        REQUIRE(session.saved().state() == defaultStateFor(session.saved().metadata().encoding));
    }
    fs::permissions(directory.path(), fs::perms::owner_write, fs::perm_options::add);
    if (!enforced) {
        SKIP("permissions are not enforced here (running as root?)");
    }
}

namespace {

/// @brief Builds a session that has taken one exposure step per value, in order.
EditSession sessionWithSteps(std::initializer_list<float> exposures) {
    EditSession session(testPhoto());
    for (const float exposure : exposures) {
        session.setState(exposed(exposure));
    }
    return session;
}

} // namespace

TEST_CASE("History of a new session is just the opening state", "[session][history]") {
    const EditSession session(testPhoto());

    REQUIRE(session.history().size() == 1);
    CHECK(session.history()[0] == HistoryStep{DevelopState{}, EditOrigin::Opened, {}});
    CHECK(session.position() == 0);
}

TEST_CASE("Each edit adds a step and moves the position to it", "[session][history]") {
    const EditSession session = sessionWithSteps({1.0F, 2.0F});

    REQUIRE(session.history().size() == 3);
    CHECK(session.position() == 2);
    CHECK(session.history()[1].state == exposed(1.0F));
    CHECK(session.history()[2].state == exposed(2.0F));
    CHECK(session.history()[1].origin == EditOrigin::Edit);
    CHECK(session.photo().state() == session.history()[session.position()].state);
}

TEST_CASE("goTo moves both ways and keeps every step", "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F});
    const std::vector<HistoryStep> before = session.history();

    session.goTo(0);
    CHECK(session.position() == 0);
    CHECK(session.photo().state() == DevelopState{});
    CHECK(session.canRedo());
    CHECK(session.history() == before);

    session.goTo(2);
    CHECK(session.position() == 2);
    CHECK(session.photo().state() == exposed(2.0F));

    session.goTo(1);
    CHECK(session.photo().state() == exposed(1.0F));
    CHECK(session.history() == before);
}

TEST_CASE("An edit after going back drops the later steps", "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F, 3.0F});
    session.goTo(1);

    session.setState(exposed(4.5F));

    REQUIRE(session.history().size() == 3);
    CHECK(session.position() == 2);
    CHECK(session.history()[1].state == exposed(1.0F));
    CHECK(session.history()[2].state == exposed(4.5F));
    CHECK_FALSE(session.canRedo());
}

TEST_CASE("goTo commits an open edit that changed nothing and keeps the steps",
          "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F});
    session.goTo(1);
    session.begin();

    session.goTo(2);

    CHECK_FALSE(session.editing());
    CHECK(session.history().size() == 3);
    CHECK(session.position() == 2);
}

TEST_CASE("goTo takes its index after committing an open edit that changed something",
          "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F, 3.0F});
    session.goTo(1);
    session.begin();
    session.update(exposed(4.0F));

    // The commit drops steps 2 and 3 and adds the edit as step 2, so index 2 is the edit.
    session.goTo(2);

    REQUIRE(session.history().size() == 3);
    CHECK(session.position() == 2);
    CHECK(session.photo().state() == exposed(4.0F));
    CHECK_FALSE(session.editing());

    // What was step 3 is gone, so the index is out of range now.
    session.goTo(1);
    session.begin();
    session.update(exposed(3.5F));
    CHECK_THROWS_AS(session.goTo(3), std::out_of_range);
}

TEST_CASE("goTo out of range throws and changes nothing", "[session][history]") {
    EditSession session = sessionWithSteps({1.0F});
    const std::vector<HistoryStep> before = session.history();

    CHECK_THROWS_AS(session.goTo(2), std::out_of_range);

    CHECK(session.history() == before);
    CHECK(session.position() == 1);
    CHECK(session.photo().state() == exposed(1.0F));
}

TEST_CASE("Commit and setState record the origin and detail", "[session][history]") {
    EditSession session(testPhoto());

    session.begin();
    session.update(exposed(1.0F));
    session.commit(EditOrigin::Crop);
    session.setState(exposed(2.0F), EditOrigin::Preset, "Warm");
    session.setState(DevelopState{}, EditOrigin::Reset);

    REQUIRE(session.history().size() == 4);
    CHECK(session.history()[1].origin == EditOrigin::Crop);
    CHECK(session.history()[1].detail.empty());
    CHECK(session.history()[2] == HistoryStep{exposed(2.0F), EditOrigin::Preset, "Warm"});
    CHECK(session.history()[3].origin == EditOrigin::Reset);
}

TEST_CASE("An edit that ends where it began leaves no step whatever its origin",
          "[session][history]") {
    EditSession session(testPhoto());

    session.setState(DevelopState{}, EditOrigin::Paste);

    CHECK(session.history().size() == 1);
}

TEST_CASE("Undo and redo move the position and keep the steps", "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F});

    session.undo();
    CHECK(session.position() == 1);
    CHECK(session.history().size() == 3);

    session.redo();
    CHECK(session.position() == 2);
}

TEST_CASE("Discarding changes restarts history at the saved state", "[session][history]") {
    EditSession session = sessionWithSteps({1.0F, 2.0F});

    session.discardChanges();

    REQUIRE(session.history().size() == 1);
    CHECK(session.history()[0] == HistoryStep{DevelopState{}, EditOrigin::Opened, {}});
    CHECK(session.position() == 0);
}

TEST_CASE("Saving keeps history", "[session][history][save]") {
    const test::TempDir directory;
    EditSession session(openPhoto(rawIn(directory)));
    session.setState(exposed(1.0F));
    session.setState(exposed(2.0F));
    const std::vector<HistoryStep> before = session.history();

    session.save();

    CHECK(session.history() == before);
    CHECK(session.position() == 2);
}

TEST_CASE("describeChange finds no difference between equal states", "[session][history]") {
    const ChangeDescription change = describeChange(exposed(1.0F), exposed(1.0F));

    CHECK(change.keys.empty());
    CHECK_FALSE(change.group.has_value());
}

TEST_CASE("describeChange names one key and its group", "[session][history]") {
    const ChangeDescription change = describeChange(DevelopState{}, exposed(1.0F));

    CHECK(change.keys == std::vector<std::string_view>{"exposure"});
    CHECK(change.group == SettingGroup::Tone);
}

TEST_CASE("describeChange lists several keys of one group in table order", "[session][history]") {
    DevelopState after;
    after.settings.tone.highlights = 20.0F;
    after.settings.tone.contrast = 10.0F;

    const ChangeDescription change = describeChange(DevelopState{}, after);

    CHECK(change.keys == std::vector<std::string_view>{"contrast", "highlights"});
    CHECK(change.group == SettingGroup::Tone);
}

TEST_CASE("describeChange leaves the group empty when keys span groups", "[session][history]") {
    DevelopState after = exposed(1.0F);
    after.settings.color.saturation = 10.0F;

    const ChangeDescription change = describeChange(DevelopState{}, after);

    CHECK(change.keys.size() == 2);
    CHECK_FALSE(change.group.has_value());
}

TEST_CASE("describeChange compares a curve as one key", "[session][history]") {
    DevelopState after;
    after.settings.toneCurve.red.points = {{0.0F, 0.1F}, {0.5F, 0.6F}, {1.0F, 1.0F}};

    const ChangeDescription change = describeChange(DevelopState{}, after);

    CHECK(change.keys == std::vector<std::string_view>{"toneCurveRed"});
    CHECK(change.group == SettingGroup::ToneCurve);
}

TEST_CASE("describeChange names the white balance keys", "[session][history]") {
    DevelopState after;
    after.settings.color.whiteBalance = WhiteBalanceMode::Custom;
    after.settings.color.temperature = 5200.0F;
    after.settings.color.tint = 4.0F;

    const ChangeDescription change = describeChange(DevelopState{}, after);

    CHECK(change.keys == std::vector<std::string_view>{"whiteBalance", "temperature", "tint"});
    CHECK(change.group == SettingGroup::Color);
}

TEST_CASE("describeChange names exactly the row that changed, for every row",
          "[session][history]") {
    std::vector<std::string_view> skipped;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        DevelopState after;
        const bool perturbed = visitField(descriptor, after.settings, [&](auto& field) -> bool {
            using Field = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_same_v<Field, bool>) {
                field = !field;
            } else if constexpr (std::is_same_v<Field, float> || std::is_same_v<Field, double>) {
                // A step to the far end of the range; one past the value if unranged.
                const double now = static_cast<double>(field);
                field = static_cast<Field>(descriptor.range ? (now == descriptor.range->maximum
                                                                   ? descriptor.range->minimum
                                                                   : descriptor.range->maximum)
                                                            : now + 1.0);
            } else if constexpr (std::is_same_v<Field, std::optional<float>>) {
                field = field.value_or(0.0F) + 1.0F;
            } else if constexpr (std::is_same_v<Field, std::uint32_t>) {
                field += 1U;
            } else if constexpr (std::is_same_v<Field, WhiteBalanceMode>) {
                field = WhiteBalanceMode::Custom;
            } else if constexpr (std::is_same_v<Field, QuarterTurn>) {
                field = QuarterTurn::Clockwise90;
            } else if constexpr (std::is_same_v<Field, std::optional<UprightCropRect>>) {
                field = UprightCropRect{0.1, 0.1, 0.9, 0.9};
            } else if constexpr (std::is_same_v<Field, CropAspect>) {
                field = CropRatio{2.0};
            } else if constexpr (std::is_same_v<Field, ToneCurve>) {
                field.points = {{0.0F, 0.1F}, {0.5F, 0.6F}, {1.0F, 1.0F}};
            } else {
                // GrainModel and LuminanceNoiseFilter have one value each so far: a lone
                // change of them cannot be expressed.
                return false;
            }
            return true;
        });
        if (!perturbed) {
            skipped.push_back(descriptor.key);
            continue;
        }
        INFO("row " << descriptor.key);
        const ChangeDescription change = describeChange(DevelopState{}, after);
        CHECK(change.keys == std::vector<std::string_view>{descriptor.key});
        CHECK(change.group == descriptor.group);
    }
    // Only the single-valued enumerations are skipped.
    CHECK(skipped.size() == 2);
}
