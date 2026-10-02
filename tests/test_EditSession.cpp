#include <EditSession.h>

#include <catch2/catch_test_macros.hpp>

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
