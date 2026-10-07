#include "RowBands.h"
#include "support/RowBandLimit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

using namespace arraw;

TEST_CASE("Every band runs, each row in exactly one", "[threads]") {
    const test::ScopedRowBandLimit limit(4);
    constexpr std::uint32_t rows = 517;
    std::vector<int> visits(rows, 0);
    detail::splitRowBands(rows, 1024, [&](std::uint32_t first, std::uint32_t last, bool) {
        for (std::uint32_t row = first; row < last; ++row) {
            ++visits[row];
        }
    });
    CHECK(std::ranges::all_of(visits, [](int count) { return count == 1; }));
}

#if defined(__linux__)
TEST_CASE("Band threads run at their caller's priority", "[threads]") {
    // As the thumbnail worker does (ADR 039): nice 10 for one thread alone.
    const test::ScopedRowBandLimit limit(4);
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::vector<int> priorities;
    std::thread caller([&] {
        setpriority(PRIO_PROCESS, 0, 10);
        detail::splitRowBands(512, 1024, [&](std::uint32_t, std::uint32_t, bool) {
            const int priority = getpriority(PRIO_PROCESS, 0);
            const std::scoped_lock lock(mutex);
            threads.insert(std::this_thread::get_id());
            priorities.push_back(priority);
        });
    });
    caller.join();
    REQUIRE(threads.size() == 4);
    for (const int priority : priorities) {
        CHECK(priority == 10);
    }
}
#endif
