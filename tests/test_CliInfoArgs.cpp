#include "cli/InfoArgs.h"
#include <catch2/catch_test_macros.hpp>

// The parser's own contract, tested without going through dispatch: exit -1
// means "proceed with the invocation", 0 means "print message to stdout" and
// 2 means "print message to stderr" (docs/adr/0050).

TEST_CASE("info parses one path into an invocation") {
    const cli::InfoParse parsed = cli::parseInfoArgs({"shot.arw"});

    REQUIRE(parsed.exitCode == -1); // proceed
    REQUIRE(parsed.invocation.paths == QStringList{"shot.arw"});
    REQUIRE_FALSE(parsed.invocation.json);
}

TEST_CASE("info takes a whole batch of paths in one invocation") {
    const cli::InfoParse parsed = cli::parseInfoArgs({"a.arw", "b.dng", "c.jpg"});

    REQUIRE(parsed.exitCode == -1);
    REQUIRE(parsed.invocation.paths == QStringList{"a.arw", "b.dng", "c.jpg"});
}

TEST_CASE("info accepts --json on either side of the paths") {
    // A flag's position is not a thing a user should have to remember.
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--json", "shot.arw"},
          std::vector<std::string>{"shot.arw", "--json"}}) {
        const cli::InfoParse parsed = cli::parseInfoArgs(args);
        REQUIRE(parsed.exitCode == -1);
        CHECK(parsed.invocation.json);
        CHECK(parsed.invocation.paths == QStringList{"shot.arw"});
    }
}

TEST_CASE("info without a path is a usage error, not an empty report") {
    const cli::InfoParse parsed = cli::parseInfoArgs({});

    REQUIRE(parsed.exitCode == 2);
    REQUIRE_FALSE(parsed.message.isEmpty());
    REQUIRE(parsed.message.startsWith("arraw info:")); // the verb owns its errors
}

TEST_CASE("info rejects an unknown flag rather than ignoring it") {
    // A silently ignored flag is a lie (docs/adr/0049).
    const cli::InfoParse parsed = cli::parseInfoArgs({"--exif-only", "shot.arw"});

    REQUIRE(parsed.exitCode == 2);
    REQUIRE(parsed.message.contains("--exif-only"));
}

TEST_CASE("info --help is a success that prints usage to stdout") {
    const cli::InfoParse parsed = cli::parseInfoArgs({"--help"});

    REQUIRE(parsed.exitCode == 0);
    REQUIRE(parsed.message.contains("arraw info"));
    REQUIRE(parsed.message.contains("--json"));
}
