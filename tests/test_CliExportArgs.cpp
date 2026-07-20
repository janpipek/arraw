#include "cli/ExportArgs.h"
#include <catch2/catch_test_macros.hpp>

using cli::parseExportArgs;

TEST_CASE("export defaults are the default-constructed ExportOptions") {
    const auto r = parseExportArgs({"a.arw", "b.arw", "-o", "out"});
    REQUIRE(r.exitCode == -1);
    REQUIRE(r.invocation.inputs == QStringList{"a.arw", "b.arw"});
    REQUIRE(r.invocation.outDir == "out");
    REQUIRE(r.invocation.options == ExportOptions{});
}

TEST_CASE("flags map onto ExportOptions") {
    const auto r = parseExportArgs({"a.arw", "-o", "out", "--format", "tiff", "--bit-depth", "16",
                                    "--width", "800", "--height", "600", "--profile", "adobergb",
                                    "--sharpen", "2", "--include-location", "--no-capture-info",
                                    "--no-descriptive"});
    REQUIRE(r.exitCode == -1);
    const ExportOptions& o = r.invocation.options;
    REQUIRE(o.format == ExportOptions::Format::TIFF);
    REQUIRE(o.bitDepth == 16);
    REQUIRE(o.width == 800);
    REQUIRE(o.height == 600);
    REQUIRE(o.profile == OutputProfile::AdobeRgb);
    REQUIRE(o.sharpening == 2);
    REQUIRE(o.metadata.includeLocation);
    REQUIRE_FALSE(o.metadata.includeCaptureInfo);
    REQUIRE_FALSE(o.metadata.includeDescriptive);
}

TEST_CASE("missing -o is a usage error") {
    REQUIRE(parseExportArgs({"a.arw"}).exitCode == 2);
}

TEST_CASE("no inputs is a usage error") {
    REQUIRE(parseExportArgs({"-o", "out"}).exitCode == 2);
}

// Names below avoid a leading "--" (unlike the brief's literal names): Catch2's
// catch_discover_tests passes each discovered test name verbatim as a CLI arg to
// the test binary and does not escape a leading dash, so ctest would otherwise
// misparse "--quality ..." / "--bit-depth ..." as unrecognised Catch2 flags (hard
// failures) and "--help ..." as Catch2's own --help (a false pass that never runs
// the test body). Assertions are unchanged from the brief.
TEST_CASE("quality is JPEG-only") {
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--quality", "80"}).exitCode == -1);
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--format", "tiff", "--quality", "80"}).exitCode
            == 2);
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--quality", "0"}).exitCode == 2);
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--quality", "101"}).exitCode == 2);
}

TEST_CASE("bit-depth 16 requires tiff") {
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--bit-depth", "16"}).exitCode == 2);
    REQUIRE(parseExportArgs({"a.arw", "-o", "out", "--bit-depth", "12"}).exitCode == 2);
}

TEST_CASE("help exits 0 with text") {
    const auto r = parseExportArgs({"--help"});
    REQUIRE(r.exitCode == 0);
    REQUIRE(r.message.contains("--format"));
}
