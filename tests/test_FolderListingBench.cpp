// Measurements for the folder listing (incremental-refresh plan, step 1). Hidden: build in
// Release and run
//   build/release/tests/arraw-tests "[listing-bench]"
// (the case carries [.bench] and [listing-bench], so it runs only when a tag filter names it).
// It generates 200 folders of 250 shots each (ARW+JPG+XMP, empty files) in a temporary
// directory; set ARRAW_BENCH_TREE to a folder to list a real tree, a NAS share for instance,
// instead.

#include "support/ReferenceListing.h"
#include "support/TempDir.h"

#include <Shot.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace arraw;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

/// @brief Measures the milliseconds a function takes, once.
double timed(const std::function<void()>& work) {
    const auto start = Clock::now();
    work();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// @brief Generates 10 years of 20 days, each day with 250 RAW+JPEG shots and their sidecars.
void generateTree(const fs::path& root) {
    for (int year = 0; year < 10; ++year) {
        const fs::path yearFolder = root / std::to_string(2010 + year);
        for (int day = 1; day <= 20; ++day) {
            const fs::path dayFolder = yearFolder / ("day " + std::to_string(day));
            fs::create_directories(dayFolder);
            for (int shot = 1; shot <= 250; ++shot) {
                const std::string stem = "DSC0" + std::to_string(1000 + shot);
                for (const char* extension : {".ARW", ".JPG", ".ARW.xmp"}) {
                    std::ofstream(dayFolder / (stem + extension));
                }
            }
        }
    }
}

/// @brief Collects every listed folder of a tree, root first, in walk order.
std::vector<fs::path> foldersOf(const fs::path& root) {
    std::vector<fs::path> folders;
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        fs::path folder = std::move(pending.back());
        pending.pop_back();
        const FolderListing listing = listFolder(folder);
        pending.insert(pending.end(), listing.folders.rbegin(), listing.folders.rend());
        folders.push_back(std::move(folder));
    }
    return folders;
}

} // namespace

TEST_CASE("Listing a large tree: the walk over listFolder against the old listing",
          "[.bench][listing-bench]") {
    std::optional<test::TempDir> generated;
    fs::path root;
    if (const char* tree = std::getenv("ARRAW_BENCH_TREE"); tree != nullptr && *tree != '\0') {
        root = tree;
    } else {
        generated.emplace();
        root = generated->path();
        const double generating = timed([&] { generateTree(root); });
        std::cout << "generated 200 x 250 shots (150 000 files) in " << std::fixed
                  << std::setprecision(0) << generating << " ms\n";
    }

    std::vector<Shot> before;
    std::vector<Shot> after;
    // A first, uncounted pass of each, so both start from a warm cache.
    (void)test::reference::listShotsRecursively(root);
    (void)listShots(root, true);
    double bestBefore = 1e300;
    double bestAfter = 1e300;
    for (int i = 0; i < 5; ++i) {
        bestBefore = std::min(bestBefore,
                              timed([&] { before = test::reference::listShotsRecursively(root); }));
        bestAfter = std::min(bestAfter, timed([&] { after = listShots(root, true); }));
    }
    REQUIRE(after == before);

    const std::vector<fs::path> folders = foldersOf(root);
    double slowestFolder = 0;
    fs::path slowest;
    double first = 0;
    for (const auto& folder : folders) {
        const double ms = timed([&] { (void)listFolder(folder); });
        if (folder == root) {
            first = ms;
        }
        if (ms > slowestFolder) {
            slowestFolder = ms;
            slowest = folder;
        }
    }
    double bestOneFolder = 1e300;
    for (int i = 0; i < 5; ++i) {
        bestOneFolder = std::min(bestOneFolder, timed([&] { (void)listFolder(slowest); }));
    }

    std::cout << std::fixed << std::setprecision(1) << "tree " << root.string() << ": "
              << after.size() << " shots in " << folders.size() << " folders\n"
              << "listShots recursive, old (one walk, map, sort): " << bestBefore
              << " ms (best of 5)\n"
              << "listShots recursive, pre-order over listFolder: " << bestAfter
              << " ms (best of 5)\n"
              << "listFolder of the root: " << first << " ms\n"
              << "listFolder of the slowest folder (" << slowest.filename().string()
              << "): " << bestOneFolder << " ms (best of 5)\n";
}
