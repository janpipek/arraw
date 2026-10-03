#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <ExifInfo.h>

#include <QImage>
#include <QString>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <exiv2/exiv2.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace arraw;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

/// @brief Writes a small JPEG, with EXIF tags set by exiv2 itself.
/// @param tags Pairs of tag key and value, in exiv2's text form.
fs::path jpegWith(const test::TempDir& directory,
                  const std::vector<std::pair<std::string, std::string>>& tags) {
    const auto path = directory.file("shot.jpg");
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::gray);
    REQUIRE(image.save(QString::fromStdString(path.string())));
    auto file = Exiv2::ImageFactory::open(path.string());
    file->readMetadata();
    for (const auto& [key, value] : tags) {
        file->exifData()[key] = value;
    }
    file->writeMetadata();
    return path;
}

} // namespace

TEST_CASE("A fraction is its quotient, or NaN when it has no denominator", "[exif]") {
    REQUIRE(URational{1, 250}.value() == Approx(0.004));
    REQUIRE(SRational{-1, 3}.value() == Approx(-1.0 / 3.0));
    REQUIRE(std::isnan(URational{5, 0}.value()));
    REQUIRE(std::isnan(SRational{-5, 0}.value()));
    REQUIRE(URational{1, 2} == URational{1, 2});
    REQUIRE(SRational{-1, 2} != SRational{1, 2});
    REQUIRE(URational{4000000000U, 1}.value() == Approx(4.0e9));
}

TEST_CASE("readExif reads every field a RAW records", "[exif][integration]") {
    CollectedDiagnostics log;
    const ExifInfo info = readExif(test::fixture("exif-32x24.dng"), log);

    REQUIRE(log.entries().empty());
    REQUIRE(info.make == "Arraw");
    REQUIRE(info.model == "Fixture One");
    REQUIRE(info.lensModel == "Fixture 35mm F2.8");
    REQUIRE(info.dateTimeOriginal == "2024:05:01 10:00:00");
    REQUIRE(info.offsetTimeOriginal == "+02:00");
    REQUIRE(info.exposureTime == URational{1, 250});
    REQUIRE(info.fNumber == URational{28, 10});
    REQUIRE(info.photographicSensitivity == 400);
    REQUIRE(info.focalLength == URational{35, 1});
    REQUIRE(info.focalLengthIn35mmFilm == 52);
    REQUIRE(info.exposureBiasValue == SRational{-1, 3});
    REQUIRE(info.flash == 16);
    REQUIRE(info.artist == "Ada Lovelace");
    REQUIRE(info.copyright == "(c) 2024 Ada Lovelace");
    REQUIRE(info.gps);
    // 50 deg 5' 15.75" N, 14 deg 25' 18" E, 235.5 m above sea level.
    REQUIRE(info.gps->latitude == Approx(50.0 + 5.0 / 60.0 + 15.75 / 3600.0));
    REQUIRE(info.gps->longitude == Approx(14.0 + 25.0 / 60.0 + 18.0 / 3600.0));
    REQUIRE(info.gps->altitude == Approx(235.5));
}

TEST_CASE("readExif gives nothing, and says so, for a file without EXIF", "[exif][integration]") {
    SECTION("a RAW that records only what the camera needs to decode it") {
        CollectedDiagnostics log;
        REQUIRE(readExif(test::fixture("linear-32x24-neutral.dng"), log) == ExifInfo{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::ExifUnreadable);
        REQUIRE(log.entries().front().severity == Severity::Info);
    }
    SECTION("a PNG") {
        CollectedDiagnostics log;
        REQUIRE(readExif(test::fixture("testcard-61x41-srgb8.png"), log) == ExifInfo{});
        REQUIRE(log.entries().size() == 1);
        const auto& entry = log.entries().front();
        REQUIRE(entry.notice == Notice::ExifUnreadable);
        REQUIRE(entry.subject);
        REQUIRE(entry.subject->filename() == "testcard-61x41-srgb8.png");
        REQUIRE_THAT(describe(entry), ContainsSubstring("no EXIF"));
    }
    SECTION("a file that is not an image at all") {
        const test::TempDir directory;
        std::ofstream(directory.file("notes.txt")) << "not a photograph";
        CollectedDiagnostics log;
        REQUIRE(readExif(directory.file("notes.txt"), log) == ExifInfo{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::ExifUnreadable);
        REQUIRE(log.entries().front().severity == Severity::Warning);
    }
}

TEST_CASE("readExif throws for a file that does not exist", "[exif]") {
    const test::TempDir directory;
    REQUIRE_THROWS_AS(readExif(directory.file("absent.dng")), std::runtime_error);
}

TEST_CASE("readExif reads a JPEG, with the signs the hemispheres give", "[exif][integration]") {
    const test::TempDir directory;

    SECTION("south and west are negative, and below sea level too") {
        const auto path = jpegWith(directory, {{"Exif.GPSInfo.GPSLatitudeRef", "S"},
                                               {"Exif.GPSInfo.GPSLatitude", "33/1 52/1 0/1"},
                                               {"Exif.GPSInfo.GPSLongitudeRef", "W"},
                                               {"Exif.GPSInfo.GPSLongitude", "70/1 30/1 0/1"},
                                               {"Exif.GPSInfo.GPSAltitudeRef", "1"},
                                               {"Exif.GPSInfo.GPSAltitude", "42/1"}});
        const ExifInfo info = readExif(path);
        REQUIRE(info.gps);
        REQUIRE(info.gps->latitude == Approx(-(33.0 + 52.0 / 60.0)));
        REQUIRE(info.gps->longitude == Approx(-70.5));
        REQUIRE(info.gps->altitude == Approx(-42.0));
    }
    SECTION("north and east are positive, and an absent altitude stays absent") {
        const auto path = jpegWith(directory, {{"Exif.GPSInfo.GPSLatitudeRef", "N"},
                                               {"Exif.GPSInfo.GPSLatitude", "10/1 0/1 0/1"},
                                               {"Exif.GPSInfo.GPSLongitudeRef", "E"},
                                               {"Exif.GPSInfo.GPSLongitude", "20/1 0/1 0/1"}});
        const ExifInfo info = readExif(path);
        REQUIRE(info.gps);
        REQUIRE(info.gps->latitude == Approx(10.0));
        REQUIRE(info.gps->longitude == Approx(20.0));
        REQUIRE_FALSE(info.gps->altitude);
    }
    SECTION("a latitude without its hemisphere is no position") {
        const auto path = jpegWith(directory, {{"Exif.GPSInfo.GPSLatitude", "10/1 0/1 0/1"},
                                               {"Exif.GPSInfo.GPSLongitudeRef", "E"},
                                               {"Exif.GPSInfo.GPSLongitude", "20/1 0/1 0/1"}});
        REQUIRE_FALSE(readExif(path).gps);
    }
    SECTION("a latitude beyond the pole is no position") {
        const auto path = jpegWith(directory, {{"Exif.GPSInfo.GPSLatitudeRef", "N"},
                                               {"Exif.GPSInfo.GPSLatitude", "95/1 0/1 0/1"},
                                               {"Exif.GPSInfo.GPSLongitudeRef", "E"},
                                               {"Exif.GPSInfo.GPSLongitude", "20/1 0/1 0/1"}});
        REQUIRE_FALSE(readExif(path).gps);
    }
    SECTION("camera text loses its padding, and a blank one is absent") {
        const auto path = jpegWith(directory, {{"Exif.Image.Make", "Canon  "},
                                               {"Exif.Image.Model", "   "},
                                               {"Exif.Photo.ISOSpeedRatings", "1600"}});
        const ExifInfo info = readExif(path);
        REQUIRE(info.make == "Canon");
        REQUIRE_FALSE(info.model);
        REQUIRE_FALSE(info.lensModel);
        REQUIRE(info.photographicSensitivity == 1600);
        REQUIRE_FALSE(info.gps);
    }
}

TEST_CASE("readExif opens a path outside ASCII", "[exif][integration]") {
    const test::TempDir directory;
    const auto folder = directory.path() / fs::path(u8"fotografie-české-日本");
    fs::create_directories(folder);
    fs::copy_file(test::fixture("exif-32x24.dng"), folder / fs::path(u8"snímek.dng"));

    REQUIRE(readExif(folder / fs::path(u8"snímek.dng")).make == "Arraw");
}

TEST_CASE("readExif may be called from several threads at once", "[exif][integration]") {
    const ExifInfo expected = readExif(test::fixture("exif-32x24.dng"));
    const std::vector<fs::path> files = {test::fixture("exif-32x24.dng"),
                                         test::fixture("testcard-61x41-srgb8.png"),
                                         test::fixture("linear-32x24-neutral.dng")};
    std::vector<std::thread> threads;
    std::vector<int> mismatches(8, 0);
    for (std::size_t t = 0; t < mismatches.size(); ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < 25; ++round) {
                const auto& file = files[(t + round) % files.size()];
                const ExifInfo info = readExif(file);
                const bool wantsExif = file.filename() == "exif-32x24.dng";
                if ((wantsExif && info != expected) || (!wantsExif && info != ExifInfo{})) {
                    ++mismatches[t];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const int count : mismatches) {
        REQUIRE(count == 0);
    }
}
