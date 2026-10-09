#include "ThumbnailCache.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <DevelopState.h>

#include <QColor>
#include <QFile>
#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace arraw;
using namespace arraw::app;

/// The thumbnail disk cache: what its keys depend on, the round trip, damage and the size cap
/// (src/app/ThumbnailCache.h, ADR 031).

namespace {

namespace fs = std::filesystem;

/// Copies a fixture into a folder.
fs::path copyFixture(const test::TempDir& folder, const std::string& name) {
    const fs::path path = folder.file(name);
    fs::copy_file(test::fixture("exif-32x24.dng"), path);
    return path;
}

/// A plain image of a colour.
QImage plain(int width, int height, QColor colour) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

/// Keeps modification times apart, as a file system's coarse clock may not.
void setAge(const fs::path& path, std::chrono::hours age) {
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

} // namespace

TEST_CASE("Cache keys follow the file, and developed ones the saved state", "[thumbnailcache]") {
    const test::TempDir folder;
    const fs::path file = copyFixture(folder, "a.dng");
    const auto embedded = ThumbnailCache::embeddedKey(file);
    const DevelopState plainState;
    DevelopState bright;
    bright.settings.tone.exposure = 1.0F;
    const auto developed = ThumbnailCache::developedKey(file, plainState);
    REQUIRE(embedded);
    REQUIRE(developed);

    SECTION("the same inputs give the same keys, and the kinds differ") {
        REQUIRE(*embedded == *ThumbnailCache::embeddedKey(file));
        REQUIRE(*developed == *ThumbnailCache::developedKey(file, DevelopState{}));
        REQUIRE(*embedded != *developed);
    }
    SECTION("the saved state changes the developed key and not the embedded one") {
        REQUIRE(*ThumbnailCache::developedKey(file, bright) != *developed);
    }
    SECTION("the masks change the developed key, and the id counter does not") {
        DevelopState masked;
        masked.localAdjustments.push_back(
            {.id = LocalAdjustmentId{1}, .shape = LinearMask{}, .deltas = {.exposure = 1.0F}});
        masked.nextLocalAdjustmentId = LocalAdjustmentId{2};
        const std::string withMask = *ThumbnailCache::developedKey(file, masked);
        REQUIRE(withMask != *developed);

        DevelopState other = masked;
        other.localAdjustments[0].deltas.exposure = 2.0F;
        REQUIRE(*ThumbnailCache::developedKey(file, other) != withMask);

        DevelopState counted = masked;
        counted.nextLocalAdjustmentId = LocalAdjustmentId{9};
        REQUIRE(*ThumbnailCache::developedKey(file, counted) == withMask);
    }
    SECTION("the modification time changes both") {
        setAge(file, std::chrono::hours(5));
        REQUIRE(*ThumbnailCache::embeddedKey(file) != *embedded);
        REQUIRE(*ThumbnailCache::developedKey(file, plainState) != *developed);
    }
    SECTION("the size changes both") {
        const auto before = fs::last_write_time(file);
        {
            std::ofstream append(file, std::ios::binary | std::ios::app);
            append << "x";
        }
        fs::last_write_time(file, before); // only the size differs
        REQUIRE(*ThumbnailCache::embeddedKey(file) != *embedded);
        REQUIRE(*ThumbnailCache::developedKey(file, plainState) != *developed);
    }
    SECTION("the path changes both") {
        const fs::path other = copyFixture(folder, "b.dng");
        fs::last_write_time(other, fs::last_write_time(file));
        REQUIRE(*ThumbnailCache::embeddedKey(other) != *embedded);
    }
    SECTION("a relative spelling of the path is the same file") {
        const fs::path spelled = folder.path() / "." / "a.dng";
        REQUIRE(*ThumbnailCache::embeddedKey(spelled) == *embedded);
    }
    SECTION("a missing file has no key") {
        REQUIRE_FALSE(ThumbnailCache::embeddedKey(folder.file("nope.dng")));
        REQUIRE_FALSE(ThumbnailCache::developedKey(folder.file("nope.dng"), plainState));
    }
}

TEST_CASE("The cache keeps what is stored, reduced to 512 pixels", "[thumbnailcache]") {
    const test::TempDir root;
    const ThumbnailCache cache(root.file("cache"));
    REQUIRE(cache.load("abcdef").isNull());

    REQUIRE(cache.store("abcdef", plain(40, 30, QColor(200, 30, 30))));
    const QImage back = cache.load("abcdef");
    REQUIRE(back.size() == QSize(40, 30));
    // JPEG is lossy: the colour is close, not equal.
    const QColor colour = back.pixelColor(20, 15);
    REQUIRE(colour.red() > 180);
    REQUIRE(colour.green() < 60);

    REQUIRE(cache.store("123456", plain(2000, 1000, Qt::blue)));
    REQUIRE(cache.load("123456").size() == QSize(512, 256));
    REQUIRE(cache.store("abcdef", plain(10, 10, Qt::green)));
    REQUIRE(cache.load("abcdef").size() == QSize(10, 10));
    REQUIRE_FALSE(cache.store("abcdef", QImage()));
}

TEST_CASE("A damaged entry is ignored and can be replaced", "[thumbnailcache]") {
    const test::TempDir root;
    const ThumbnailCache cache(root.file("cache"));
    REQUIRE(cache.store("abcdef", plain(8, 8, Qt::red)));
    const fs::path entry = root.file("cache") / "ab" / "abcdef.jpg";
    REQUIRE(fs::exists(entry));
    {
        std::ofstream damaged(entry, std::ios::binary | std::ios::trunc);
        damaged << "this is not a JPEG";
    }
    REQUIRE(cache.load("abcdef").isNull());
    REQUIRE_FALSE(fs::exists(entry));
    REQUIRE(cache.store("abcdef", plain(8, 8, Qt::red)));
    REQUIRE_FALSE(cache.load("abcdef").isNull());
}

TEST_CASE("Pruning removes the least recently used entries beyond the cap", "[thumbnailcache]") {
    const test::TempDir root;
    const fs::path dir = root.file("cache");
    // A noisy picture does not compress away, so that each entry has a real size.
    QImage noisy(200, 200, QImage::Format_RGB32);
    for (int y = 0; y < noisy.height(); ++y) {
        for (int x = 0; x < noisy.width(); ++x) {
            noisy.setPixel(x, y,
                           qRgb((x * 37 + y * 11) % 256, (x * 5 + y * 91) % 256, (x ^ y) % 256));
        }
    }
    const ThumbnailCache unlimited(dir);
    for (const std::string key : {"aa0001", "bb0002", "cc0003", "dd0004"}) {
        REQUIRE(unlimited.store(key, noisy));
    }
    const std::uint64_t total = unlimited.sizeBytes();
    REQUIRE(total > 0);
    const std::uint64_t each = total / 4;
    // Oldest first: aa, bb, cc, dd.
    setAge(dir / "aa" / "aa0001.jpg", std::chrono::hours(40));
    setAge(dir / "bb" / "bb0002.jpg", std::chrono::hours(30));
    setAge(dir / "cc" / "cc0003.jpg", std::chrono::hours(20));
    setAge(dir / "dd" / "dd0004.jpg", std::chrono::hours(10));

    SECTION("within the cap nothing goes") {
        REQUIRE(ThumbnailCache(dir, total).prune() == 0);
        REQUIRE(unlimited.sizeBytes() == total);
    }
    SECTION("the oldest go first") {
        const ThumbnailCache capped(dir, total - each / 2);
        REQUIRE(capped.prune() == 1);
        REQUIRE(capped.load("aa0001").isNull());
        REQUIRE_FALSE(capped.load("bb0002").isNull());
        REQUIRE_FALSE(capped.load("dd0004").isNull());
    }
    SECTION("a use makes an entry young again") {
        REQUIRE_FALSE(unlimited.load("aa0001").isNull()); // refreshes it
        const ThumbnailCache capped(dir, total - each / 2);
        REQUIRE(capped.prune() == 1);
        REQUIRE_FALSE(capped.load("aa0001").isNull());
        REQUIRE(capped.load("bb0002").isNull());
    }
    SECTION("a cap of nothing empties it") {
        REQUIRE(ThumbnailCache(dir, 0).prune() == 4);
        REQUIRE(unlimited.sizeBytes() == 0);
    }
}
