#include "cli/PresetArgs.h"
#include <catch2/catch_test_macros.hpp>

using cli::parsePresetArgs;
using cli::PresetVerb;

TEST_CASE("preset list defaults to the table (json off)") {
    const auto r = parsePresetArgs({"list"});
    REQUIRE(r.exitCode == -1);
    REQUIRE(r.invocation.verb == PresetVerb::List);
    REQUIRE_FALSE(r.invocation.json);
}

TEST_CASE("preset list --json sets the json flag") {
    const auto r = parsePresetArgs({"list", "--json"});
    REQUIRE(r.exitCode == -1);
    REQUIRE(r.invocation.verb == PresetVerb::List);
    REQUIRE(r.invocation.json);
}

TEST_CASE("no subcommand is a usage error") {
    REQUIRE(parsePresetArgs({}).exitCode == 2);
}

TEST_CASE("an unknown subcommand is a usage error") {
    REQUIRE(parsePresetArgs({"frobnicate"}).exitCode == 2);
}

TEST_CASE("preset help exits 0 with text") {
    const auto r = parsePresetArgs({"--help"});
    REQUIRE(r.exitCode == 0);
    REQUIRE(r.message.contains("list"));
}
