#include "Diagnostics.h"

#include "ImageImport.h"

#include "support/Fixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string_view>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("A RAW that recorded no white balance says so", "[diagnostics][integration]") {
    /// The substitution is silent in the pixels: the frame looks plausible and
    /// nothing about it says the camera never chose that light. This is the
    /// only place a photographer can find out.
    CollectedDiagnostics log;

    const auto image = loadImage(test::fixture("linear-32x24-nowb.dng"), log);

    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(log.entries().size() == 1);
    const auto& entry = log.entries().front();
    REQUIRE(entry.notice == Notice::SubstitutedWhiteBalance);
    REQUIRE(entry.severity == Severity::Warning);
    REQUIRE(entry.subject);
    REQUIRE(entry.subject->filename() == "linear-32x24-nowb.dng");
    REQUIRE_THAT(describe(entry), ContainsSubstring("estimate"));
}

TEST_CASE("A RAW that recorded one says nothing", "[diagnostics][integration]") {
    CollectedDiagnostics log;

    const auto image = loadImage(test::fixture("linear-32x24-warmwb.dng"), log);

    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(log.entries().empty());
}

TEST_CASE("A photograph that is not a RAW says nothing either", "[diagnostics][integration]") {
    CollectedDiagnostics log;

    const auto image = loadImage(test::fixture("testcard-61x41-srgb8.png"), log);

    REQUIRE(image.size() == ImageSize{61, 41});
    REQUIRE(log.entries().empty());
}

TEST_CASE("Loading without a log is allowed", "[diagnostics][integration]") {
    /// Nothing should have to find somewhere to report to before it can read a
    /// file, so the default is a log that listens and forgets.
    const auto image = loadImage(test::fixture("linear-32x24-nowb.dng"));

    REQUIRE(image.size() == ImageSize{32, 24});
}

TEST_CASE("A diagnostic's details are values, not prose", "[diagnostics]") {
    /// The sentence is presentation, rendered from the notice and its details;
    /// what a program matches on is the notice itself.
    const Diagnostic exported{.notice = Notice::Exported,
                              .severity = Severity::Info,
                              .subject = "holiday.arw",
                              .values = {std::string("out/holiday.jpg")}};

    REQUIRE(describe(exported) == "written to out/holiday.jpg");

    const Diagnostic finished{
        .notice = Notice::BatchFinished, .severity = Severity::Error, .values = {3.0, 40.0}};

    REQUIRE(describe(finished) == "3 of 40 failed");
}
