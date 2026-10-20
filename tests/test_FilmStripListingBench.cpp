// Measurement of the film strip's background listing with its view (incremental-refresh plan,
// step 3; review 2026-10-20, B2 and R6). Hidden: build in Release and run
//   QT_QPA_PLATFORM=offscreen build/release/tests/arraw-widget-tests "[listing-bench]"
// The trees are those of tests/test_FolderListingBench.cpp (support/BenchTrees.h), or
// ARRAW_BENCH_TREE.

#include "support/BenchTrees.h"
#include "support/StallProbe.h"
#include "support/TempDir.h"
#include "ui/FilmStrip.h"

#include <Shot.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListView>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

using namespace arraw;

TEST_CASE("Listing a large tree into a shown film strip: GUI stalls", "[.bench][listing-bench]") {
    std::vector<std::unique_ptr<test::TempDir>> generated;
    const auto trees = test::benchTrees(generated);
    for (const auto& [name, root] : trees) {
        (void)listShots(root, true); // A warm cache.
        const test::TempDir cache;
        app::FilmStrip strip(nullptr, app::ThumbnailCache(cache.path()));
        strip.resize(1600, 160);
        strip.show();
        REQUIRE(QTest::qWaitForWindowExposed(&strip));
        strip.setIncludeSubfolders(true);
        const auto runUntil = [](const std::function<bool()>& done) {
            while (!done()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents |
                                                QEventLoop::WaitForMoreEvents);
            }
        };
        test::StallProbe probe;
        QElapsedTimer clock;
        for (int run = 1; run <= 3; ++run) {
            probe.start();
            clock.start();
            strip.setFolder(root);
            const double returned = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            const double resetStall = probe.longestMs();
            probe.start();
            runUntil([&] { return !strip.isListing(); });
            const double listed = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            const double listingStall = probe.longestMs();
            probe.stop();
            const int rows = strip.findChild<QListView*>()->model()->rowCount();
            std::cout << std::fixed << std::setprecision(1) << "film strip, " << name << ", run "
                      << run << ": " << rows << " shots; setFolder returned in " << returned
                      << " ms (stall " << resetStall << " ms), listing finished at " << listed
                      << " ms, longest GUI stall while listing " << listingStall << " ms\n";
            REQUIRE(rows == static_cast<int>(listShots(root, true).size()));
        }
    }
}
