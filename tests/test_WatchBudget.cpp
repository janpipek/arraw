#include "WatchBudget.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

using namespace arraw::app;

/// The assignment of the film strip's limited watches (incremental-refresh plan, section 3c).

namespace {

namespace fs = std::filesystem;

/// Depth of a path below `/r`: `/r` is 0, `/r/a` is 1.
int depthOf(const fs::path& path) {
    return static_cast<int>(std::distance(path.begin(), path.end())) - 2;
}

/// Adds a directory by its path, whose depth the path says.
WatchBudget::Grant add(WatchBudget& budget, const char* path) {
    return budget.add(path, depthOf(path));
}

/// Sorted paths, for comparing sets.
std::vector<fs::path> sorted(std::vector<fs::path> paths) {
    std::ranges::sort(paths);
    return paths;
}

} // namespace

TEST_CASE("The budget watches everything while there is room", "[watchbudget]") {
    WatchBudget budget(3);
    for (const char* path : {"/r", "/r/a", "/r/b"}) {
        const auto grant = add(budget, path);
        REQUIRE(grant.watch);
        REQUIRE_FALSE(grant.evict);
    }
    REQUIRE(budget.watched() == 3);
    REQUIRE(budget.unwatched() == 0);
    // Adding one known changes nothing.
    REQUIRE(add(budget, "/r/a").watch);
    REQUIRE(budget.watched() == 3);
}

TEST_CASE("A deeper directory than the watched ones waits", "[watchbudget]") {
    WatchBudget budget(2);
    REQUIRE(add(budget, "/r").watch);
    REQUIRE(add(budget, "/r/a").watch);
    const auto grant = add(budget, "/r/a/x");
    REQUIRE_FALSE(grant.watch);
    REQUIRE_FALSE(grant.evict);
    // So does a later sibling of equal depth: first come, in folder order.
    REQUIRE_FALSE(add(budget, "/r/b").watch);
    REQUIRE(budget.watched() == 2);
    REQUIRE(budget.unwatched() == 2);
    REQUIRE_FALSE(budget.isWatched("/r/a/x"));
    REQUIRE(budget.isWatched("/r/a"));
}

TEST_CASE("A shallower directory evicts the deepest one watched", "[watchbudget]") {
    WatchBudget budget(3);
    // A depth-first walk meets the deep directories before the shallow siblings.
    for (const char* path : {"/r", "/r/a", "/r/a/x"}) {
        REQUIRE(add(budget, path).watch);
    }
    const auto grant = add(budget, "/r/b");
    REQUIRE(grant.watch);
    REQUIRE(grant.evict == fs::path("/r/a/x"));
    REQUIRE_FALSE(budget.isWatched("/r/a/x"));
    REQUIRE(budget.isWatched("/r/b"));
    REQUIRE(budget.watched() == 3);
    REQUIRE(budget.unwatched() == 1);

    // Within a depth, folder order decides: /r/a/y is after /r/a/x, which is out already.
    const auto sibling = add(budget, "/r/a/y");
    REQUIRE_FALSE(sibling.watch);
    REQUIRE_FALSE(sibling.evict);
}

TEST_CASE("The assignment does not depend on the order of adding", "[watchbudget]") {
    const std::vector<const char*> paths{"/r",   "/r/a",   "/r/a/x", "/r/a/y",
                                         "/r/b", "/r/b/z", "/r/c",   "/r/c/w"};
    auto watchedAfter = [&](std::vector<const char*> order) {
        WatchBudget budget(4);
        for (const char* path : order) {
            add(budget, path);
        }
        std::vector<fs::path> watched;
        for (const char* path : paths) {
            if (budget.isWatched(path)) {
                watched.emplace_back(path);
            }
        }
        return watched;
    };
    const std::vector<fs::path> expected{"/r", "/r/a", "/r/b", "/r/c"};
    REQUIRE(watchedAfter(paths) == expected);
    REQUIRE(watchedAfter({paths.rbegin(), paths.rend()}) == expected);
    REQUIRE(watchedAfter({"/r/a/y", "/r/c/w", "/r/b", "/r", "/r/a/x", "/r/c", "/r/a", "/r/b/z"}) ==
            expected);
}

TEST_CASE("A place that is freed goes to the shallowest waiting directory", "[watchbudget]") {
    WatchBudget budget(2);
    add(budget, "/r");
    add(budget, "/r/a");
    add(budget, "/r/a/x");
    add(budget, "/r/b");
    REQUIRE(budget.unwatched() == 2);

    // /r/b waits ahead of /r/a/x: same depth is the folder order, and a lower depth first.
    const auto promoted = budget.remove("/r/a");
    REQUIRE(promoted == fs::path("/r/b"));
    REQUIRE(budget.isWatched("/r/b"));
    REQUIRE(budget.watched() == 2);

    // A waiting one that goes frees nothing.
    REQUIRE_FALSE(budget.remove("/r/a/x"));
    REQUIRE(budget.unwatched() == 0);
    REQUIRE_FALSE(budget.remove("/never"));
}

TEST_CASE("A subtree that goes promotes the ones outside it", "[watchbudget]") {
    WatchBudget budget(3);
    for (const char* path : {"/r", "/r/a", "/r/a/x", "/r/b", "/r/b/y", "/r/c"}) {
        add(budget, path);
    }
    // Watched: /r, /r/a, /r/b. Gone: /r/a and /r/a/x.
    const std::vector<fs::path> gone{"/r/a", "/r/a/x"};
    const auto promoted = budget.remove(gone);
    // /r/c (depth 1) then /r/b/y: never the removed /r/a/x.
    REQUIRE(sorted(promoted) == sorted({"/r/c"}));
    REQUIRE(budget.isWatched("/r/c"));
    REQUIRE(budget.watched() == 3);
    REQUIRE(budget.unwatched() == 1);
    REQUIRE_FALSE(budget.isWatched("/r/a"));
}

TEST_CASE("A refused watch shrinks the budget to what the system gave", "[watchbudget]") {
    WatchBudget budget(5);
    for (const char* path : {"/r", "/r/a", "/r/b", "/r/c"}) {
        REQUIRE(add(budget, path).watch);
    }
    // The system said no to the last one it was asked for: ENOSPC at 3 watches.
    budget.refused("/r/c");
    REQUIRE(budget.cap() == 3);
    REQUIRE(budget.watched() == 3);
    REQUIRE(budget.unwatched() == 1);
    // The next ones wait, however many places the cap was set to.
    REQUIRE_FALSE(add(budget, "/r/c/z").watch);
    REQUIRE(budget.watched() == 3);

    // A shallower directory still takes a place from a deeper one.
    const auto grant = add(budget, "/r/0");
    REQUIRE(grant.watch);
    REQUIRE(grant.evict == fs::path("/r/b"));
}

TEST_CASE("A refused directory does not take the places freed after it", "[watchbudget]") {
    // Review R3: a refusal of one directory alone (a share without change notification).
    WatchBudget budget(4);
    for (const char* path : {"/r", "/r/a", "/r/b", "/r/c", "/r/d"}) {
        add(budget, path);
    }
    budget.refused("/r/a");
    REQUIRE(budget.cap() == 3);
    REQUIRE(budget.unwatched() == 2);

    // The place /r/b leaves goes to /r/d, which waited, not to /r/a again.
    REQUIRE(budget.remove("/r/b") == fs::path("/r/d"));
    REQUIRE(budget.isWatched("/r/d"));
    REQUIRE_FALSE(budget.isWatched("/r/a"));
    REQUIRE(budget.cap() == 3);
    // Nothing else waits: the next place freed stays free.
    REQUIRE_FALSE(budget.remove("/r/c"));
    REQUIRE(budget.watched() == 2);
    REQUIRE(budget.unwatched() == 1);

    // The next walk tries it again; one that goes meanwhile is forgotten.
    auto moves = budget.setCap(4);
    REQUIRE(moves.promoted == std::vector<fs::path>{"/r/a"});
    REQUIRE(budget.isWatched("/r/a"));
    budget.refused("/r/a");
    REQUIRE_FALSE(budget.remove("/r/a"));
    REQUIRE(budget.unwatched() == 0);
    moves = budget.setCap(4);
    REQUIRE(moves.promoted.empty());
}

TEST_CASE("A new cap lifts or lowers the budget", "[watchbudget]") {
    WatchBudget budget(2);
    for (const char* path : {"/r", "/r/a", "/r/b", "/r/c"}) {
        add(budget, path);
    }
    budget.refused("/r/a");
    REQUIRE(budget.cap() == 1);

    // The start of a walk sets the configured cap again: the refused ones get another try.
    auto moves = budget.setCap(3);
    REQUIRE(sorted(moves.promoted) == sorted({"/r/a", "/r/b"}));
    REQUIRE(moves.evicted.empty());
    REQUIRE(budget.watched() == 3);

    moves = budget.setCap(1);
    REQUIRE(sorted(moves.evicted) == sorted({"/r/a", "/r/b"}));
    REQUIRE(moves.promoted.empty());
    REQUIRE(budget.isWatched("/r"));
    REQUIRE(budget.watched() == 1);
    REQUIRE(budget.unwatched() == 3);
}

TEST_CASE("A budget of nothing watches nothing, and reset forgets", "[watchbudget]") {
    WatchBudget budget(0);
    REQUIRE_FALSE(add(budget, "/r").watch);
    REQUIRE(budget.unwatched() == 1);
    budget.reset(2);
    REQUIRE(budget.unwatched() == 0);
    REQUIRE(add(budget, "/r").watch);
}
