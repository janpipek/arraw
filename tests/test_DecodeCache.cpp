#include "DecodeCache.h"
#include "ImagePipeline.h"
#include <catch2/catch_test_macros.hpp>

namespace {

// A LoadResult whose fullRes is w*h*3 floats, so its byte size is predictable.
LoadResult makeResult(int w, int h, float fill = 0.5f) {
    LoadResult r;
    r.fullRes.width = w;
    r.fullRes.height = h;
    r.fullRes.data.assign(size_t(w) * size_t(h) * 3u, fill);
    return r;
}

} // namespace

TEST_CASE("get returns the inserted result, nullptr on a miss", "[decodecache]") {
    DecodeCache cache(1u << 30); // 1 GiB budget

    REQUIRE(cache.get("a") == nullptr);

    cache.insert("a", makeResult(4, 4, 0.25f));

    const LoadResult* hit = cache.get("a");
    REQUIRE(hit != nullptr);
    REQUIRE(hit->fullRes.width == 4);
    REQUIRE(hit->fullRes.height == 4);
    REQUIRE(hit->fullRes.data.front() == 0.25f);

    REQUIRE(cache.get("b") == nullptr);
}

TEST_CASE("inserting past the byte budget evicts the least-recently-used entry",
          "[decodecache]") {
    // Each makeResult(10,10) is 10*10*3*4 = 1200 bytes; budget holds two, not three.
    DecodeCache cache(2500);
    cache.insert("a", makeResult(10, 10));
    cache.insert("b", makeResult(10, 10));
    cache.insert("c", makeResult(10, 10)); // pushes over budget → evict LRU ("a")

    REQUIRE(cache.get("a") == nullptr);
    REQUIRE(cache.get("b") != nullptr);
    REQUIRE(cache.get("c") != nullptr);
}

TEST_CASE("get refreshes recency so the accessed entry survives the next eviction",
          "[decodecache]") {
    DecodeCache cache(2500);
    cache.insert("a", makeResult(10, 10));
    cache.insert("b", makeResult(10, 10));

    REQUIRE(cache.get("a") != nullptr); // bumps "a" to MRU; "b" is now LRU

    cache.insert("c", makeResult(10, 10)); // evicts "b", not "a"

    REQUIRE(cache.get("a") != nullptr);
    REQUIRE(cache.get("b") == nullptr);
    REQUIRE(cache.get("c") != nullptr);
}

TEST_CASE("re-inserting a key replaces it without double-counting bytes",
          "[decodecache]") {
    DecodeCache cache(1u << 30);
    cache.insert("a", makeResult(10, 10)); // 1200 bytes
    cache.insert("a", makeResult(10, 10)); // replace — still one entry, 1200 bytes

    REQUIRE(cache.byteSize() == 1200);
    REQUIRE(cache.get("a") != nullptr);
}

TEST_CASE("a pinned entry is never evicted, even as the least-recently-used",
          "[decodecache]") {
    DecodeCache cache(2500); // holds two 1200-byte entries
    cache.insert("a", makeResult(10, 10));
    cache.pin("a");                        // "a" is the current image
    cache.insert("b", makeResult(10, 10)); // "a" is now LRU
    cache.insert("c", makeResult(10, 10)); // over budget → evict LRU, but skip pinned "a"

    REQUIRE(cache.get("a") != nullptr); // pinned → survives
    REQUIRE(cache.get("b") == nullptr); // evicted in its place
    REQUIRE(cache.get("c") != nullptr);
}
