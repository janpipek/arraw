#include "cli/SystemInfoArgs.h"
#include <catch2/catch_test_macros.hpp>

// The parser's own contract, tested without going through dispatch: exit -1
// means "proceed with the invocation", 0 means "print message to stdout" and
// 2 means "print message to stderr" (docs/adr/0050).

TEST_CASE("system-info with no arguments proceeds with json off") {
    const cli::SystemInfoParse parsed = cli::parseSystemInfoArgs({});

    REQUIRE(parsed.exitCode == -1);
    REQUIRE_FALSE(parsed.invocation.json);
}

TEST_CASE("system-info --json proceeds with json on") {
    const cli::SystemInfoParse parsed = cli::parseSystemInfoArgs({"--json"});

    REQUIRE(parsed.exitCode == -1);
    REQUIRE(parsed.invocation.json);
}

TEST_CASE("system-info rejects an unknown flag rather than ignoring it") {
    const cli::SystemInfoParse parsed = cli::parseSystemInfoArgs({"--exif-only"});

    REQUIRE(parsed.exitCode == 2);
    REQUIRE(parsed.message.contains("--exif-only"));
}

TEST_CASE("system-info rejects a stray positional argument") {
    // No paths to take here, unlike `info` — a stray argument is a mistake.
    const cli::SystemInfoParse parsed = cli::parseSystemInfoArgs({"shot.arw"});

    REQUIRE(parsed.exitCode == 2);
}

TEST_CASE("system-info --help is a success that prints usage to stdout") {
    const cli::SystemInfoParse parsed = cli::parseSystemInfoArgs({"--help"});

    REQUIRE(parsed.exitCode == 0);
    REQUIRE(parsed.message.contains("arraw system-info"));
    REQUIRE(parsed.message.contains("--json"));
}
