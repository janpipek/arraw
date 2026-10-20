#include "FolderDiff.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The linear merge of a folder's rows with its fresh listing (src/app/FolderDiff.h).

namespace {

/// Makes the shot number @p n of a folder; numbers order as the names do.
Shot shotNumber(int n, bool withJpeg = false) {
    char name[32];
    std::snprintf(name, sizeof name, "IMG_%06d", n);
    Shot shot{.primary = std::filesystem::path("/photos") / (std::string(name) + ".dng"),
              .companions = {}};
    if (withJpeg) {
        shot.companions.push_back(std::filesystem::path("/photos") / (std::string(name) + ".jpg"));
    }
    return shot;
}

/// Makes the shots of the numbers given, in that order.
std::vector<Shot> shots(const std::vector<int>& numbers) {
    std::vector<Shot> result;
    for (const int n : numbers) {
        result.push_back(shotNumber(n));
    }
    return result;
}

using Runs = std::vector<RowRun>;

} // namespace

TEST_CASE("Equal lists have nothing to merge", "[folderdiff]") {
    const FolderDiff diff = diffFolder(shots({1, 2, 3}), shots({1, 2, 3}));
    REQUIRE_FALSE(diff.changed());
    REQUIRE(diff.runCount() == 0);
    REQUIRE_FALSE(diffFolder(shots({}), shots({})).changed());
}

TEST_CASE("Inserts come as runs of adjacent rows, at their final indices", "[folderdiff]") {
    SECTION("Into the middle, at the front and at the back") {
        const FolderDiff diff = diffFolder(shots({2, 3, 6}), shots({1, 2, 3, 4, 5, 6, 7, 8}));
        REQUIRE(diff.removes.empty());
        REQUIRE(diff.inserts == Runs{{0, 1}, {3, 2}, {6, 2}});
        REQUIRE_FALSE(diff.needsReset);
    }
    SECTION("Into an empty folder") {
        REQUIRE(diffFolder(shots({}), shots({1, 2, 3})).inserts == Runs{{0, 3}});
    }
}

TEST_CASE("Removes come as runs of old indices, the last run first", "[folderdiff]") {
    SECTION("Scattered") {
        const FolderDiff diff = diffFolder(shots({1, 2, 3, 4, 5, 6, 7}), shots({1, 4, 5}));
        REQUIRE(diff.removes == Runs{{5, 2}, {1, 2}});
        REQUIRE(diff.inserts.empty());
    }
    SECTION("Everything") {
        REQUIRE(diffFolder(shots({1, 2, 3}), shots({})).removes == Runs{{0, 3}});
    }
}

TEST_CASE("Removes and inserts mix, and applying them gives the fresh list", "[folderdiff]") {
    const std::vector<int> before{1, 2, 3, 5, 6, 9};
    const std::vector<int> after{2, 4, 5, 7, 8, 9, 10};
    const FolderDiff diff = diffFolder(shots(before), shots(after));
    REQUIRE(diff.removes == Runs{{4, 1}, {2, 1}, {0, 1}}); // 6, 3, 1
    REQUIRE(diff.inserts == Runs{{1, 1}, {3, 2}, {6, 1}}); // 4; 7, 8; 10
    // Replay as the model does.
    std::vector<int> rows = before;
    for (const RowRun& run : diff.removes) {
        rows.erase(rows.begin() + run.first, rows.begin() + run.first + run.count);
    }
    for (const RowRun& run : diff.inserts) {
        rows.insert(rows.begin() + run.first, after.begin() + run.first,
                    after.begin() + run.first + run.count);
    }
    REQUIRE(rows == after);
}

TEST_CASE("A shot whose companions changed is an update, not a remove and an insert",
          "[folderdiff]") {
    const std::vector<Shot> before{shotNumber(1), shotNumber(2), shotNumber(3, true)};
    const std::vector<Shot> after{shotNumber(1, true), shotNumber(2), shotNumber(3)};
    const FolderDiff diff = diffFolder(before, after);
    REQUIRE(diff.removes.empty());
    REQUIRE(diff.inserts.empty());
    REQUIRE(diff.updates == std::vector<int>{0, 2});
    REQUIRE(diff.changed());
}

TEST_CASE("A change of primary is a remove and an insert", "[folderdiff]") {
    // A JPEG on its own, then its RAW arrives and becomes the primary.
    const Shot jpeg{.primary = "/photos/IMG_000001.jpg", .companions = {}};
    const std::vector<Shot> after{shotNumber(1, true)};
    const FolderDiff diff = diffFolder(std::vector<Shot>{jpeg}, after);
    REQUIRE(diff.removes == Runs{{0, 1}});
    REQUIRE(diff.inserts == Runs{{0, 1}});
}

TEST_CASE("Updates are indices of the fresh list, counted with the inserts", "[folderdiff]") {
    const std::vector<Shot> before{shotNumber(2), shotNumber(4, true)};
    const std::vector<Shot> after{shotNumber(1), shotNumber(2), shotNumber(3), shotNumber(4)};
    const FolderDiff diff = diffFolder(before, after);
    REQUIRE(diff.inserts == Runs{{0, 1}, {2, 1}});
    REQUIRE(diff.updates == std::vector<int>{3});
}

TEST_CASE("More runs than allowed ask for a reset", "[folderdiff]") {
    SECTION("Interleaved copy") {
        std::vector<int> all;
        std::vector<int> evens;
        for (int n = 0; n < 200; ++n) {
            all.push_back(n);
            if (n % 2 == 0) {
                evens.push_back(n);
            }
        }
        const FolderDiff diff = diffFolder(shots(evens), shots(all));
        REQUIRE(diff.runCount() == 100);
        REQUIRE(diff.needsReset);
    }
    SECTION("Exactly the limit is still merged") {
        // 2, 4 and 6 are three inserts, 9 a remove: four runs.
        const std::vector<int> before{1, 3, 5, 7, 9};
        const std::vector<int> after{1, 2, 3, 4, 5, 6, 7};
        REQUIRE(diffFolder(shots(before), shots(after), 4).runCount() == 4);
        REQUIRE_FALSE(diffFolder(shots(before), shots(after), 4).needsReset);
        REQUIRE(diffFolder(shots(before), shots(after), 3).needsReset);
    }
    SECTION("The limit is a count of runs, removes and inserts together") {
        const std::vector<int> before{1, 2, 3, 4, 5};
        const std::vector<int> after{1, 3, 6, 7, 5};
        REQUIRE(diffFolder(shots(before), shots(after), 4).needsReset == false);
        REQUIRE(diffFolder(shots(before), shots(after), 2).needsReset == true);
    }
}

TEST_CASE("Rows that stay in another order than the listing ask for a reset", "[folderdiff]") {
    REQUIRE(diffFolder(shots({1, 3, 2}), shots({1, 2, 3})).needsReset);
}

TEST_CASE("Fifty thousand rows are merged in a moment, even in the worst shape", "[folderdiff]") {
    constexpr int total = 50000;
    std::vector<int> all;
    std::vector<int> evens;
    for (int n = 0; n < total; ++n) {
        all.push_back(n);
        if (n % 2 == 0) {
            evens.push_back(n);
        }
    }
    const std::vector<Shot> full = shots(all);
    const std::vector<Shot> half = shots(evens);
    // A quadratic merge needs minutes here, a linear one milliseconds; the margin is for debug
    // builds and loaded machines.
    const auto limit = std::chrono::seconds(2);

    SECTION("An interleaved copy, 25 000 runs") {
        const auto start = std::chrono::steady_clock::now();
        const FolderDiff diff = diffFolder(half, full);
        REQUIRE(std::chrono::steady_clock::now() - start < limit);
        REQUIRE(diff.inserts.size() == 25000);
        REQUIRE(diff.needsReset);
    }
    SECTION("The same, the other way") {
        const auto start = std::chrono::steady_clock::now();
        const FolderDiff diff = diffFolder(full, half);
        REQUIRE(std::chrono::steady_clock::now() - start < limit);
        REQUIRE(diff.removes.size() == 25000);
        REQUIRE(diff.needsReset);
    }
    SECTION("Nothing changed") {
        const auto start = std::chrono::steady_clock::now();
        const FolderDiff diff = diffFolder(full, full);
        REQUIRE(std::chrono::steady_clock::now() - start < limit);
        REQUIRE_FALSE(diff.changed());
    }
}
