#include <DevelopState.h>
#include <EditSession.h>
#include <Photo.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace arraw;

/// The state is what says how a photograph is developed; for now that is its
/// global settings alone.

namespace {

/// @brief Builds a state with an exposure no photograph can have.
DevelopState outOfRangeState() {
    DevelopState state;
    state.settings.tone.exposure = 1000.0F;
    return state;
}

/// @brief Builds a document without a file.
Photo testPhoto() {
    return {"photo.dng", ImageMetadata{ImageSize{32, 24}, workingEncoding}};
}

} // namespace

TEST_CASE("A default state is the default settings", "[state]") {
    REQUIRE(DevelopState{} == DevelopState{DevelopSettings{}});
    REQUIRE(DevelopState{}.settings == DevelopSettings{});
}

TEST_CASE("States compare by their settings", "[state]") {
    DevelopState brighter;
    brighter.settings.tone.exposure = 1.0F;

    REQUIRE(brighter != DevelopState{});
    REQUIRE(brighter == DevelopState{brighter.settings});
}

TEST_CASE("A state with a setting out of range is rejected", "[state]") {
    REQUIRE_NOTHROW(validate(DevelopState{}));
    REQUIRE_THROWS_AS(validate(outOfRangeState()), std::invalid_argument);
}

TEST_CASE("A photograph cannot be built with an invalid state", "[state][photo]") {
    REQUIRE_THROWS_AS(
        Photo("photo.dng", ImageMetadata{ImageSize{32, 24}, workingEncoding}, outOfRangeState()),
        std::invalid_argument);
    REQUIRE_THROWS_AS(testPhoto().with(outOfRangeState()), std::invalid_argument);
}

TEST_CASE("Developing a photograph differently keeps its file and marks", "[state][photo]") {
    const Photo original = testPhoto().with(PhotoMarks{.rating = 4});
    DevelopState brighter;
    brighter.settings.tone.exposure = 1.0F;

    const Photo other = original.with(brighter);

    REQUIRE(other.state() == brighter);
    REQUIRE(other.path() == original.path());
    REQUIRE(other.metadata() == original.metadata());
    REQUIRE(other.marks() == original.marks());
    REQUIRE(original.state() == DevelopState{});
}

TEST_CASE("A session replaces the state of its photograph", "[state][session]") {
    const Photo original = testPhoto();
    EditSession session(original);
    DevelopState brighter;
    brighter.settings.tone.exposure = 1.0F;

    session.setState(brighter);

    REQUIRE(session.photo().state() == brighter);
    REQUIRE(session.photo().path() == original.path());
    REQUIRE(original.state() == DevelopState{});
}
