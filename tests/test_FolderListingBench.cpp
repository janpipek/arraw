// Measurements for the folder listing (incremental-refresh plan, steps 1 and 3). Hidden: build in
// Release and run
//   build/release/tests/arraw-tests "[listing-bench]"
//   QT_QPA_PLATFORM=offscreen build/release/tests/arraw-widget-tests "[listing-bench]"
// (the case carries [.bench] and [listing-bench], so it runs only when a tag filter names it).
// It generates trees of 50 000 shots (empty files) in a temporary directory: 200 folders of 250
// shots (ARW+JPG+XMP), and for the background listing also one flat folder and 20 000 folders of
// two; set ARRAW_BENCH_TREE to a folder to list a real tree, a NAS share for instance, instead.

#include "FolderWatcher.h"
#include "ShotFilterModel.h"
#include "ShotModel.h"
#include "support/BenchTrees.h"
#include "support/ReferenceListing.h"
#include "support/StallProbe.h"
#include "support/TempDir.h"

#include <Shot.h>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
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

/// @brief Gives the tree to measure: ARRAW_BENCH_TREE, or a generated one kept in @p generated.
fs::path benchTree(std::optional<test::TempDir>& generated) {
    if (const char* tree = std::getenv("ARRAW_BENCH_TREE"); tree != nullptr && *tree != '\0') {
        return tree;
    }
    generated.emplace();
    const double generating =
        timed([&] { test::generateBenchTree(generated->path(), test::benchShapes().front()); });
    std::cout << "generated 200 x 250 shots (150 000 files) in " << std::fixed
              << std::setprecision(0) << generating << " ms\n";
    return generated->path();
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
    const fs::path root = benchTree(generated);

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

TEST_CASE("Listing a large tree in the background: time to the first batch, and GUI stalls",
          "[.bench][listing-bench]") {
    // Three shapes of 50 000 shots (support/BenchTrees.h): 200 day folders, one flat folder,
    // and 20 000 folders of two shots. No view: this binary has a QCoreApplication and no Qt
    // Widgets; the strip's variant, with its QListView, is in the widget tests.
    std::vector<std::unique_ptr<test::TempDir>> generated;
    const auto trees = test::benchTrees(generated);
    for (const auto& [name, root] : trees) {
        (void)listShots(root, true); // A warm cache, as for the case above.

        // The model as the strip has it: a filter proxy over it, and the watcher with its lister.
        app::ShotModel model;
        app::ShotFilterModel proxy;
        proxy.setSourceModel(&model);
        app::FolderWatcher watcher(model);
        QElapsedTimer clock;
        double firstBatch = -1;
        int inserts = 0;
        int largest = 0;
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                         [&](const QModelIndex&, int first, int last) {
                             if (firstBatch < 0) {
                                 firstBatch = static_cast<double>(clock.nsecsElapsed()) / 1e6;
                             }
                             ++inserts;
                             largest = std::max(largest, last - first + 1);
                         });
        const auto runUntil = [](const std::function<bool()>& done) {
            while (!done()) {
                // The probe's 1 ms timer wakes the loop.
                QCoreApplication::processEvents(QEventLoop::AllEvents |
                                                QEventLoop::WaitForMoreEvents);
            }
        };
        test::StallProbe probe;
        // The first run lists into an empty model; the later ones replace 50 000 rows, as a
        // change of folder does.
        for (int run = 1; run <= 3; ++run) {
            firstBatch = -1;
            inserts = 0;
            largest = 0;
            probe.start();
            clock.start();
            model.setFolder(root, true);
            const double returned = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            // The stall of the synchronous reset, apart from the listing's.
            const double resetStall = probe.longestMs();
            probe.start();
            runUntil([&] { return !model.isListing(); });
            const double listed = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            const double listingStall = probe.longestMs();
            runUntil([&] { return model.pendingLoads() == 0; });
            const double loaded = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            const double loadingStall = probe.longestMs();
            probe.stop();
            std::cout << std::fixed << std::setprecision(1) << name << ", run " << run << ": "
                      << model.rowCount() << " shots in " << inserts << " inserts (largest "
                      << largest << "); setFolder returned in " << returned << " ms (stall "
                      << resetStall << " ms), first batch at " << firstBatch
                      << " ms, listing finished at " << listed
                      << " ms, longest GUI stall while listing " << listingStall
                      << " ms; marks and EXIF read at " << loaded
                      << " ms, longest GUI stall until then " << loadingStall << " ms\n";
            REQUIRE(model.rowCount() == static_cast<int>(listShots(root, true).size()));
        }
    }
}

TEST_CASE("A change in one folder of a large tree: the cost of reading that folder again",
          "[.bench][listing-bench]") {
    // Step 4 of the plan: a change reads again only the directory that reported it. Measured
    // from the file's creation (a real inotify event, debounce 0) to its row, and against a
    // refresh of all, which is what every event cost before.
    std::optional<test::TempDir> generated;
    const fs::path root = benchTree(generated);
    app::ShotModel model;
    app::ShotFilterModel proxy;
    proxy.setSourceModel(&model);
    app::FolderWatcher watcher(model, std::chrono::milliseconds(0));
    const auto runUntil = [](const std::function<bool()>& done) {
        while (!done()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents,
                                            5);
        }
    };
    model.setFolder(root, true);
    runUntil([&] { return !model.isListing() && model.pendingLoads() == 0; });
    const std::vector<fs::path> folders = foldersOf(root);

    test::StallProbe probe;
    std::vector<double> latencies;
    double stall = 0;
    for (int n = 0; n < 20; ++n) {
        const fs::path& folder = folders[(static_cast<std::size_t>(n) * 37 + 1) % folders.size()];
        const fs::path added = folder / ("ZZ_bench_" + std::to_string(n) + ".jpg");
        probe.start();
        const auto start = Clock::now();
        std::ofstream(added, std::ios::binary).put('\0');
        runUntil([&] { return model.rowOf(added) >= 0; });
        latencies.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        runUntil([&] { return model.pendingLoads() == 0; });
        stall = std::max(stall, probe.longestMs());
        probe.stop();
    }
    std::ranges::sort(latencies);

    double refreshAll = 1e300;
    for (int i = 0; i < 3; ++i) {
        refreshAll = std::min(
            refreshAll, timed([&] {
                watcher.refreshAll();
                runUntil([&] { return !model.isRefreshing() && model.pendingLoads() == 0; });
            }));
    }
    std::cout << std::fixed << std::setprecision(1) << "per-folder refresh (" << model.rowCount()
              << " shots, " << folders.size() << " folders): file to row median "
              << latencies[latencies.size() / 2] << " ms, max " << latencies.back()
              << " ms, longest GUI stall " << stall << " ms; refresh of all (walk, merge, "
              << "recheck every sidecar) " << refreshAll << " ms (best of 3)\n";
    for (int n = 0; n < 20; ++n) {
        const fs::path& folder = folders[(static_cast<std::size_t>(n) * 37 + 1) % folders.size()];
        fs::remove(folder / ("ZZ_bench_" + std::to_string(n) + ".jpg"));
    }
}
