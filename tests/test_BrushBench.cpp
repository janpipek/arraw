// Measurements for the brush prototype (plan step 4). Hidden: build in Release and run
//   build/release/tests/arraw-tests "[brush-bench]"
// (the cases carry [.bench] and [brush-bench], not [brush], so that "[brush]" stays the fast
// unit tests; a hidden case runs only when a tag filter names it).
// Set ARRAW_BENCH_OUT to a directory to get PNG crops of the precision and resolution cases.

#include "BrushCoverage.h"
#include "BrushCoverageCache.h"
#include "LocalPlan.h"
#include "RowBands.h"
#include "StrokeCodec.h"
#include "support/BrushGenerators.h"
#include "support/Fixtures.h"
#include "support/RowBandLimit.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <Diagnostics.h>
#include <ImageBuffer.h>
#include <ImageExport.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Sidecar.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

using namespace arraw;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t seed = 20261009;

/// Milliseconds a function takes, once.
double timed(const std::function<void()>& work) {
    const auto start = Clock::now();
    work();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Milliseconds the best of five runs of a small function takes.
double bestOfFive(const std::function<void()>& work) {
    double best = 1e300;
    for (int i = 0; i < 5; ++i) {
        best = std::min(best, timed(work));
    }
    return best;
}

/// Milliseconds the best of three runs of a function takes.
double bestOfThree(const std::function<void()>& work) {
    double best = 1e300;
    for (int i = 0; i < 3; ++i) {
        best = std::min(best, timed(work));
    }
    return best;
}

struct Spread {
    double median = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

Spread spreadOf(std::vector<double> values) {
    if (values.empty()) {
        return {};
    }
    std::ranges::sort(values);
    const auto at = [&](double fraction) {
        return values[std::min(
            values.size() - 1,
            static_cast<std::size_t>(fraction * static_cast<double>(values.size())))];
    };
    return {at(0.5), at(0.95), values.back()};
}

/// One result line: `brush.bench <case> key=value ...`, printed when it goes out of scope.
class Line {
public:
    explicit Line(std::string name) : name_(std::move(name)) {}
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() {
        std::cout << "brush.bench " << name_ << text_.str() << std::endl;
    }

    template <typename T> Line& operator()(const char* key, const T& value) {
        text_ << ' ' << key << '=' << value;
        return *this;
    }

    Line& operator()(const char* key, double value) {
        text_ << ' ' << key << '=' << std::fixed << std::setprecision(3) << value;
        return *this;
    }

    Line& operator()(const char* key, const Spread& spread) {
        return (*this)((std::string(key) + ".median").c_str(),
                       spread.median)((std::string(key) + ".p95").c_str(),
                                      spread.p95)((std::string(key) + ".max").c_str(), spread.max);
    }

private:
    std::string name_;
    std::ostringstream text_;
};

long peakKilobytes() {
#if defined(__linux__)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
#else
    return 0;
#endif
}

/// Says which case runs, with the thread count and a warning for an unoptimised build.
void begin(const char* name) {
    Line line(std::string(name) + ".setup");
    line("threads", std::thread::hardware_concurrency());
#ifndef NDEBUG
    line("warning", "NDEBUG-unset-timings-are-meaningless");
#endif
}

void finish(const char* name) {
    Line line(std::string(name) + ".memory");
#if defined(__linux__)
    line("peakRssMiB", static_cast<double>(peakKilobytes()) / 1024.0);
#else
    line("peakRssMiB", "unavailable");
#endif
}

std::size_t countDabs(const StrokeList& list, ImageSize raster) {
    std::size_t dabs = 0;
    for (const PlacedStroke& stroke : placedStrokes(list, raster)) {
        dabs += stroke.dabs.size();
    }
    return dabs;
}

/// Pixels a pixel loop visits for the dabs of a list, once clipped to the raster.
double dabPixels(const StrokeList& list, ImageSize raster) {
    double total = 0.0;
    for (const PlacedStroke& stroke : placedStrokes(list, raster)) {
        for (const DabCentre& dab : stroke.dabs) {
            const PixelSpan xs = dabSpan(dab.x, stroke.radius);
            const PixelSpan ys = dabSpan(dab.y, stroke.radius);
            const double w = std::max<double>(0.0, std::min<std::int64_t>(xs.last, raster.width) -
                                                       std::max<std::int64_t>(xs.first, 0));
            const double h = std::max<double>(0.0, std::min<std::int64_t>(ys.last, raster.height) -
                                                       std::max<std::int64_t>(ys.first, 0));
            total += w * h;
        }
    }
    return total;
}

std::optional<std::filesystem::path> benchOutput() {
    const char* directory = std::getenv("ARRAW_BENCH_OUT");
    if (directory == nullptr || *directory == '\0') {
        return std::nullopt;
    }
    std::filesystem::create_directories(directory);
    return std::filesystem::path(directory);
}

/// Writes a crop of a plane of values (0 to 1) as a 16-bit grey PNG.
void exportGrey(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                const std::function<float(std::uint32_t, std::uint32_t)>& value) {
    // exportImage takes only formats QImage has: 16-bit RGBA (opaque here), not RGB.
    ImageBuffer image({width, height}, PixelFormat::RgbaU16, NamedEncoding::Srgb);
    auto samples = image.samples<std::uint16_t>();
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float v = std::clamp(value(x, y), 0.0F, 1.0F);
            const auto code = static_cast<std::uint16_t>(std::lround(v * 65535.0F));
            std::uint16_t* pixel = &samples[(static_cast<std::size_t>(y) * width + x) * 4];
            pixel[0] = pixel[1] = pixel[2] = code;
            pixel[3] = 65535;
        }
    }
    ExportOptions options;
    options.format = ImageFileFormat::Png;
    options.bitDepth = 16;
    exportImage(image, path, options);
}

// ---------------------------------------------------------------------------------------------
// B1: the encodings

struct EncodingRun {
    std::size_t points = 0;
    std::size_t textBytes = 0;
    std::size_t binaryBytes = 0;
    double plainBytes = 0.0;
    double textEncodeMs = 0.0;
    double textDecodeMs = 0.0;
    double binaryEncodeMs = 0.0;
    double binaryDecodeMs = 0.0;
};

EncodingRun encodingsOf(const std::vector<std::shared_ptr<const StrokeList>>& masks) {
    EncodingRun run;
    std::vector<std::string> texts;
    std::vector<std::string> binaries;
    for (const auto& mask : masks) {
        for (const auto& stroke : mask->strokes()) {
            run.points += stroke->points.size();
            run.plainBytes += (14.0 + 8.0 * static_cast<double>(stroke->points.size())) * 4.0 / 3.0;
        }
    }
    run.textEncodeMs = timed([&] {
        for (const auto& mask : masks) {
            for (const auto& stroke : mask->strokes()) {
                texts.push_back(strokeText(*stroke));
            }
        }
    });
    run.binaryEncodeMs = timed([&] {
        for (const auto& mask : masks) {
            for (const auto& stroke : mask->strokes()) {
                binaries.push_back(strokeBase64(*stroke));
            }
        }
    });
    for (const std::string& s : texts) {
        run.textBytes += s.size();
    }
    for (const std::string& s : binaries) {
        run.binaryBytes += s.size();
    }
    std::vector<Stroke> backText;
    std::vector<Stroke> backBinary;
    run.textDecodeMs = bestOfThree([&] {
        backText.clear();
        for (const std::string& s : texts) {
            backText.push_back(strokeFromText(s).stroke);
        }
    });
    run.binaryDecodeMs = bestOfThree([&] {
        backBinary.clear();
        for (const std::string& s : binaries) {
            backBinary.push_back(strokeFromBase64(s).stroke);
        }
    });
    std::size_t n = 0;
    for (const auto& mask : masks) {
        for (const auto& stroke : mask->strokes()) {
            REQUIRE(backText[n] == *stroke);
            REQUIRE(backBinary[n] == *stroke);
            ++n;
        }
    }
    return run;
}

void reportEncodings(const char* label, const EncodingRun& run) {
    const double points = static_cast<double>(run.points);
    Line("B1.encoding")("set", label)("points", run.points)("text.bytes", run.textBytes)(
        "text.bytesPerPoint", static_cast<double>(run.textBytes) / points)(
        "base64.bytes", run.binaryBytes)("base64.bytesPerPoint",
                                         static_cast<double>(run.binaryBytes) / points)(
        "plainF32.bytes", run.plainBytes)("plainF32.bytesPerPoint", run.plainBytes / points)(
        "text.encodeMs", run.textEncodeMs)("text.decodeMs", run.textDecodeMs)(
        "base64.encodeMs", run.binaryEncodeMs)("base64.decodeMs", run.binaryDecodeMs);
}

/// Writes a sidecar with a brush entry per mask, then reads it back and decodes every stroke.
///
/// `readSidecar` drops a brush entry before it decodes any stroke, so its time is the XMP parse
/// alone; the decoding is timed on its own and the two are added.
std::pair<std::uintmax_t, double>
sidecarRun(const char* label, const char* codec,
           const std::vector<std::shared_ptr<const StrokeList>>& masks,
           const std::function<std::string(const Stroke&)>& encode,
           const std::function<Stroke(const std::string&)>& decode) {
    const test::TempDir directory;
    const std::filesystem::path photo = directory.file("bench.dng");
    std::ofstream(photo, std::ios::binary) << "x";
    std::ostringstream xmp;
    std::vector<std::string> encoded;
    xmp << R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
<rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/"><arraw:localAdjustments><rdf:Seq>)";
    int id = 1;
    for (const auto& mask : masks) {
        xmp << R"(<rdf:li rdf:parseType="Resource"><arraw:id>)" << id++
            << "</arraw:id><arraw:type>brush</arraw:type><arraw:name></arraw:name>"
               "<arraw:enabled>True</arraw:enabled><arraw:opacity>1</arraw:opacity>"
               "<arraw:invert>False</arraw:invert><arraw:rasteriser>1</arraw:rasteriser>"
               "<arraw:strokes><rdf:Seq>";
        for (const auto& stroke : mask->strokes()) {
            encoded.push_back(encode(*stroke));
            xmp << "<rdf:li>" << encoded.back() << "</rdf:li>";
        }
        xmp << "</rdf:Seq></arraw:strokes></rdf:li>";
    }
    xmp << "</rdf:Seq></arraw:localAdjustments></rdf:Description></rdf:RDF></x:xmpmeta>"
           "<?xpacket end=\"w\"?>\n";
    const std::filesystem::path sidecar = sidecarPath(photo);
    std::ofstream(sidecar, std::ios::binary) << xmp.str();
    std::size_t dropped = 0;
    const double readMs = bestOfThree([&] {
        CollectedDiagnostics log;
        const std::optional<SidecarContents> contents = readSidecar(photo, log);
        REQUIRE(contents);
        dropped = 0;
        for (const Diagnostic& entry : log.entries()) {
            dropped += entry.notice == Notice::LocalAdjustmentDropped ? 1 : 0;
        }
    });
    // Guard: a brush entry is still unreachable, so the reader drops it.
    REQUIRE(dropped >= 1);
    std::size_t points = 0;
    const double decodeMs = bestOfThree([&] {
        points = 0;
        for (const std::string& text : encoded) {
            points += decode(text).points.size();
        }
    });
    Line("B1.sidecar")("set", label)("codec", codec)(
        "fileBytes", std::filesystem::file_size(sidecar))("readSidecarMs", readMs)(
        "decodeMs", decodeMs)("readPlusDecodeMs", readMs + decodeMs)("decodedPoints",
                                                                     points)("dropped", dropped);
    return {std::filesystem::file_size(sidecar), readMs + decodeMs};
}

} // namespace

TEST_CASE("B1 the encodings of a stroke", "[.bench][brush-bench]") {
    begin("B1");
    std::vector<std::shared_ptr<const StrokeList>> realistic;
    for (std::uint32_t i = 0; i < 16; ++i) {
        realistic.push_back(test::paintedMask(seed + i, 60, test::everydayStyle, 0.667));
    }
    const auto zigzag = std::make_shared<const StrokeList>(
        std::vector<Stroke>{test::zigzagStroke(10'000, 40, 0.02F, 0.5F, 0.8F)});
    std::vector<std::shared_ptr<const StrokeList>> heavy;
    for (std::uint32_t i = 0; i < 16; ++i) {
        test::StrokeStyle style = test::everydayStyle;
        style.pointsLow = style.pointsHigh = 500;
        heavy.push_back(test::paintedMask(seed + 100 + i, 100, style, 0.667));
    }
    reportEncodings("realistic", encodingsOf(realistic));
    reportEncodings("zigzag10000", encodingsOf({zigzag}));
    reportEncodings("heavy", encodingsOf(heavy));
    for (const auto& [label, masks] :
         {std::pair{"realistic", realistic}, std::pair{"heavy", heavy}}) {
        const auto [textBytes, textMs] = sidecarRun(
            label, "text", masks, [](const Stroke& s) { return strokeText(s); },
            [](const std::string& text) { return strokeFromText(text).stroke; });
        const auto [binaryBytes, binaryMs] = sidecarRun(
            label, "base64", masks, [](const Stroke& s) { return strokeBase64(s); },
            [](const std::string& text) { return strokeFromBase64(text).stroke; });
        // Decision rule 3: text, unless (text sidecar over 5 MB or read-plus-decode over 250 ms)
        // and base64 is at least 2 times better on both.
        const double sizeGain = static_cast<double>(textBytes) / static_cast<double>(binaryBytes);
        const double timeGain = textMs / binaryMs;
        const bool textTooBig = static_cast<double>(textBytes) > 5.0e6 || textMs > 250.0;
        Line("B1.rule3")("set", label)("textOver5MBorOver250ms", textTooBig)("sizeGain", sizeGain)(
            "readPlusDecodeGain", timeGain)(
            "picks", textTooBig && sizeGain >= 2.0 && timeGain >= 2.0 ? "base64" : "text");
    }
    finish("B1");
}

// ---------------------------------------------------------------------------------------------

TEST_CASE("B2 sixteen full-size masks", "[.bench][brush-bench]") {
    begin("B2");
    constexpr ImageSize frame{6000, 4000};
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    std::vector<std::shared_ptr<const StrokeList>> masks;
    for (std::uint32_t i = 0; i < 16; ++i) {
        masks.push_back(test::paintedMask(seed + i, 60, test::everydayStyle, 4000.0 / 6000.0));
    }
    double totalMs = 0.0;
    double packMs = 0.0;
    std::size_t dabs = 0;
    std::vector<std::uint64_t> digests;
    std::vector<CoveragePlane> group;
    for (std::size_t i = 0; i < masks.size(); ++i) {
        CoveragePlane plane;
        const double ms = timed([&] { plane = rasteriseBrush(*masks[i], frame); });
        totalMs += ms;
        dabs += countDabs(*masks[i], frame);
        digests.push_back(test::planeDigest(plane));
        Line("B2.mask")("index", i)("rasteriseMs", ms)("dabs", countDabs(*masks[i], frame));
        group.push_back(std::move(plane));
        if (group.size() == 4) {
            std::vector<std::uint8_t> packed(pixels * 4);
            packMs += timed([&] {
                detail::forEachRowBand(
                    frame.height, frame.width, [&](std::uint32_t a, std::uint32_t b) {
                        for (std::uint32_t y = a; y < b; ++y) {
                            for (std::uint32_t x = 0; x < frame.width; ++x) {
                                const std::size_t at =
                                    static_cast<std::size_t>(y) * frame.width + x;
                                for (std::size_t c = 0; c < 4; ++c) {
                                    packed[at * 4 + c] = static_cast<std::uint8_t>(
                                        std::clamp(group[c].values[at], 0.0F, 1.0F) * 255.0F +
                                        0.5F);
                                }
                            }
                        }
                    });
            });
            group.clear();
        }
    }
    double single = 0.0;
    {
        const test::ScopedRowBandLimit one(1);
        single = timed([&] { (void)rasteriseBrush(*masks[0], frame); });
    }
    const double mb = 1.0 / (1024.0 * 1024.0);
    Line("B2.total")("masks", masks.size())("rasteriseTotalMs", totalMs)("singleThreadMaskMs",
                                                                         single)("dabs", dabs)(
        "floatMiBPerMask", static_cast<double>(pixels * 4) * mb)("packRGBA8MsPer4", packMs / 4.0)(
        "rgba8.MiBPerTexture", static_cast<double>(pixels * 4) * mb)(
        "rgba16f.MiBPerTexture", static_cast<double>(pixels * 8) * mb)(
        "coarse.rgba8.MiBPerTexture", static_cast<double>(pixels) * mb)(
        "coarse.rgba16f.MiBPerTexture", static_cast<double>(pixels) * 2.0 * mb);
    // Replay after a reload, through both encodings, mask by mask.
    for (std::size_t i = 0; i < masks.size(); ++i) {
        for (const bool binary : {false, true}) {
            std::vector<Stroke> again;
            for (const auto& stroke : masks[i]->strokes()) {
                again.push_back(binary ? strokeFromBase64(strokeBase64(*stroke)).stroke
                                       : strokeFromText(strokeText(*stroke)).stroke);
            }
            const StrokeList reloaded(std::move(again));
            REQUIRE(test::planeDigest(rasteriseBrush(reloaded, frame)) == digests[i]);
        }
    }
    Line("B2.replay")("identical", "all16-both-codecs");
    finish("B2");
}

// ---------------------------------------------------------------------------------------------

namespace {

/// Paints dabs of a stroke onto clones of the tiles they touch and shares all the others.
///
/// @param style A placed stroke that supplies radius, core, flow and erase.
/// @param dabs The dabs to paint, in order.
/// @param touched Receives the tiles that were drawn.
std::shared_ptr<const CoverageTiles> paintedOnto(const std::shared_ptr<const CoverageTiles>& base,
                                                 const PlacedStroke& style,
                                                 const std::vector<DabCentre>& dabs,
                                                 std::vector<std::uint32_t>& touched) {
    const std::uint32_t tile = base->tileSize();
    const ImageSize raster = base->size();
    PlacedStroke placed = style;
    placed.dabs = dabs;
    std::vector<bool> marks(static_cast<std::size_t>(base->columns()) * base->rows(), false);
    placed.left = placed.top = INT64_MAX;
    placed.right = placed.bottom = INT64_MIN;
    for (const DabCentre& dab : dabs) {
        const PixelSpan xs = dabSpan(dab.x, placed.radius);
        const PixelSpan ys = dabSpan(dab.y, placed.radius);
        placed.left = std::min(placed.left, xs.first);
        placed.right = std::max(placed.right, xs.last);
        placed.top = std::min(placed.top, ys.first);
        placed.bottom = std::max(placed.bottom, ys.last);
        const std::int64_t x0 = std::max<std::int64_t>(xs.first, 0);
        const std::int64_t x1 = std::min<std::int64_t>(xs.last, raster.width);
        const std::int64_t y0 = std::max<std::int64_t>(ys.first, 0);
        const std::int64_t y1 = std::min<std::int64_t>(ys.last, raster.height);
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        for (std::int64_t row = y0 / tile; row <= (y1 - 1) / tile; ++row) {
            for (std::int64_t column = x0 / tile; column <= (x1 - 1) / tile; ++column) {
                marks[static_cast<std::size_t>(row) * base->columns() + column] = true;
            }
        }
    }
    touched.clear();
    for (std::uint32_t i = 0; i < marks.size(); ++i) {
        if (marks[i]) {
            touched.push_back(i);
        }
    }
    std::vector<std::shared_ptr<const CoverageTile>> grid(marks.size());
    for (std::uint32_t i = 0; i < grid.size(); ++i) {
        grid[i] = base->tile(i);
    }
    std::vector<std::shared_ptr<CoverageTile>> fresh(touched.size());
    detail::forEachRowBand(
        static_cast<std::uint32_t>(touched.size()), tile * tile,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t n = first; n < last; ++n) {
                const std::uint32_t x = touched[n] % base->columns() * tile;
                const std::uint32_t y = touched[n] / base->columns() * tile;
                const std::uint32_t width = std::min(tile, raster.width - x);
                const std::uint32_t height = std::min(tile, raster.height - y);
                auto copy =
                    grid[touched[n]]
                        ? std::make_shared<CoverageTile>(*grid[touched[n]])
                        : std::make_shared<CoverageTile>(CoverageTile{
                              width, height,
                              std::vector<float>(static_cast<std::size_t>(width) * height)});
                paintRegion({&placed, 1}, {x, y, width, height}, copy->values);
                fresh[n] = std::move(copy);
            }
        });
    for (std::size_t n = 0; n < fresh.size(); ++n) {
        grid[touched[n]] = std::move(fresh[n]);
    }
    return std::make_shared<const CoverageTiles>(raster, tile, std::move(grid));
}

} // namespace

TEST_CASE("B3 long strokes and a live stroke", "[.bench][brush-bench]") {
    begin("B3");
    for (const ImageSize frame : {ImageSize{6000, 4000}, ImageSize{1500, 1000}}) {
        const StrokeList list(
            std::vector<Stroke>{test::zigzagStroke(10'000, 12, 0.02F, 0.5F, 0.8F)});
        const double placeMs = bestOfFive([&] { (void)placedStrokes(list, frame); });
        CoveragePlane plane;
        const double rasteriseMs = timed([&] { plane = rasteriseBrush(list, frame); });
        Line("B3.long")("size", std::to_string(frame.width) + "x" + std::to_string(frame.height))(
            "dabs", countDabs(list, frame))("placementMs", placeMs)("rasteriseMs", rasteriseMs);
    }
    // A live stroke grows by ten points per update. Two ways to draw each update:
    //  - whole: `prefix.appended(partial)` through the cache, which repaints the whole partial
    //    stroke on top of the prefix;
    //  - tail: keep the tiles of "prefix + the dabs that settled", paint only the new settled dabs
    //    onto a clone of them, then the end dab onto a clone of that (exact, because the end dab
    //    is the last thing painted).
    for (const ImageSize frame : {ImageSize{1500, 1000}, ImageSize{6000, 4000}}) {
        for (const std::uint32_t tile : {128U, 256U}) {
            BrushCoverageCache cache(std::size_t{4} << 30, tile);
            std::shared_ptr<const StrokeList> prefix =
                test::paintedMask(seed + 7, 20, test::everydayStyle, 0.667);
            const auto base = cache.coverage(prefix, frame).tiles;
            std::mt19937_64 g(seed + 8);
            test::StrokeStyle style = test::everydayStyle;
            style.pointsLow = style.pointsHigh = 2000;
            const Stroke live = test::wanderingStroke(g, style, 0.667);
            std::vector<double> whole;
            std::vector<double> wholeDirty;
            std::vector<double> tail;
            std::vector<double> tailDirty;
            std::vector<double> placement;
            std::shared_ptr<const CoverageTiles> settled = base;
            std::shared_ptr<const CoverageTiles> shown;
            std::size_t settledDabs = 0;
            for (std::size_t count = 10; count <= live.points.size(); count += 10) {
                Stroke partial = live;
                partial.points.resize(count);
                const auto list = prefix->appended(partial);
                BrushCoverageCache::Result result;
                whole.push_back(timed([&] { result = cache.coverage(list, frame, false); }));
                wholeDirty.push_back(static_cast<double>(result.dirty.size()));

                std::vector<std::uint32_t> touched;
                std::vector<std::uint32_t> endTouched;
                tail.push_back(timed([&] {
                    PlacedStroke placed;
                    std::vector<DabCentre> regular;
                    placement.push_back(timed([&] {
                        placed = placedStroke(partial, frame);
                        regular = dabCentres(partial, frame, false);
                    }));
                    settled = paintedOnto(
                        settled, placed,
                        {regular.begin() + static_cast<std::ptrdiff_t>(settledDabs), regular.end()},
                        touched);
                    settledDabs = regular.size();
                    shown = settled;
                    if (placed.dabs.size() > regular.size()) {
                        shown = paintedOnto(settled, placed, {placed.dabs.back()}, endTouched);
                    }
                }));
                tailDirty.push_back(static_cast<double>(touched.size() + endTouched.size()));
            }
            // The tail variant is exact: the same bits as the cache gives for the whole stroke.
            const auto finalList = prefix->appended(live);
            REQUIRE(shown->gathered() == cache.coverage(finalList, frame, false).tiles->gathered());
            Line("B3.live")("size",
                            std::to_string(frame.width) + "x" + std::to_string(frame.height))(
                "tile", tile)("updates", whole.size())("whole.updateMs", spreadOf(whole))(
                "whole.dirtyTiles", spreadOf(wholeDirty))("tail.updateMs", spreadOf(tail))(
                "tail.dirtyTiles", spreadOf(tailDirty))("tail.placementMs",
                                                        spreadOf(placement))("tail.exact", true);
        }
    }
    finish("B3");
}

// ---------------------------------------------------------------------------------------------

namespace {

/// Redraws the given tiles from zero with every stroke: the baseline that keeps no float tiles.
void redoneTiles(const StrokeList& list, ImageSize raster, std::uint32_t tile,
                 const std::vector<std::uint32_t>& dirty) {
    const std::vector<PlacedStroke> placed = placedStrokes(list, raster);
    const std::uint32_t columns = (raster.width + tile - 1) / tile;
    detail::forEachRowBand(static_cast<std::uint32_t>(dirty.size()), tile * tile,
                           [&](std::uint32_t first, std::uint32_t last) {
                               std::vector<float> values;
                               for (std::uint32_t n = first; n < last; ++n) {
                                   const std::uint32_t x = dirty[n] % columns * tile;
                                   const std::uint32_t y = dirty[n] / columns * tile;
                                   const std::uint32_t w = std::min(tile, raster.width - x);
                                   const std::uint32_t h = std::min(tile, raster.height - y);
                                   values.assign(static_cast<std::size_t>(w) * h, 0.0F);
                                   paintRegion(placed, {x, y, w, h}, values);
                               }
                           });
}

} // namespace

TEST_CASE("B4 a painting session through the cache", "[.bench][brush-bench]") {
    begin("B4");
    struct Session {
        const char* name;
        std::uint32_t strokes;
        std::optional<std::array<double, 4>> box;
    };
    for (const ImageSize frame : {ImageSize{1500, 1000}, ImageSize{6000, 4000}}) {
        for (const std::uint32_t tile : {128U, 256U, 512U}) {
            for (const Session& session :
                 {Session{"painting", 200, std::nullopt},
                  Session{"overpaint", 300, std::array<double, 4>{0.4, 0.6, 0.4, 0.6}}}) {
                const std::string label = std::to_string(frame.width) + "x" +
                                          std::to_string(frame.height) +
                                          " tile=" + std::to_string(tile) + " " + session.name;
                std::mt19937_64 g(seed + 40);
                std::vector<Stroke> strokes;
                StrokeBudget spent;
                for (std::uint32_t i = 0; i < session.strokes; ++i) {
                    Stroke next =
                        test::wanderingStroke(g, test::everydayStyle, 4000.0 / 6000.0, session.box);
                    spent += budgetOf(next);
                    if (!spent.withinMaskLimits()) {
                        break; // the mask is full: the edit rules refuse the next stroke
                    }
                    strokes.push_back(std::move(next));
                }
                BrushCoverageCache cache(std::size_t{16} << 30, tile);
                std::vector<std::shared_ptr<const StrokeList>> lists{
                    std::make_shared<const StrokeList>()};
                std::vector<double> append;
                std::vector<double> dirty;
                std::vector<double> full;
                std::vector<double> redone;
                std::vector<double> incrementalOnSampled;
                for (std::size_t i = 0; i < strokes.size(); ++i) {
                    lists.push_back(lists.back()->appended(strokes[i]));
                    BrushCoverageCache::Result result;
                    const double ms = timed([&] { result = cache.coverage(lists.back(), frame); });
                    append.push_back(ms);
                    dirty.push_back(static_cast<double>(result.dirty.size()));
                    if (i % 10 == 9) {
                        full.push_back(timed([&] { (void)rasteriseBrush(*lists.back(), frame); }));
                        redone.push_back(
                            timed([&] { redoneTiles(*lists.back(), frame, tile, result.dirty); }));
                        incrementalOnSampled.push_back(ms);
                    }
                }
                const std::size_t memory = cache.memoryBytes();
                // Keeping the undo states compactly: float tiles for the newest state only (the
                // base that an append is painted on), 8-bit or half-float tiles for the others.
                // Tiles shared between states are counted once.
                std::unordered_set<const CoverageTile*> newest;
                std::size_t newestPixels = 0;
                const auto last = cache.coverage(lists.back(), frame).tiles;
                for (std::uint32_t i = 0; i < last->columns() * last->rows(); ++i) {
                    if (last->tile(i) && newest.insert(last->tile(i).get()).second) {
                        newestPixels += last->tile(i)->values.size();
                    }
                }
                std::unordered_set<const CoverageTile*> older;
                std::size_t olderPixels = 0;
                for (std::size_t i = 0; i + 1 < lists.size(); ++i) {
                    const auto tiles = cache.coverage(lists[i], frame).tiles;
                    for (std::uint32_t t = 0; t < tiles->columns() * tiles->rows(); ++t) {
                        const auto& tilePtr = tiles->tile(t);
                        if (tilePtr && !newest.contains(tilePtr.get()) &&
                            older.insert(tilePtr.get()).second) {
                            olderPixels += tilePtr->values.size();
                        }
                    }
                }
                constexpr double mebi = 1024.0 * 1024.0;
                Line("B4.compact")("config", label)("states", lists.size())(
                    "allFloatMiB", static_cast<double>(memory) / mebi)(
                    "newestFloatPlusOlderU8MiB",
                    (static_cast<double>(newestPixels) * 4.0 + static_cast<double>(olderPixels)) /
                        mebi)("newestFloatPlusOlderHalfMiB",
                              (static_cast<double>(newestPixels) * 4.0 +
                               static_cast<double>(olderPixels) * 2.0) /
                                  mebi)(
                    "olderU8MiBPerState",
                    static_cast<double>(olderPixels) / mebi /
                        static_cast<double>(std::max<std::size_t>(lists.size() - 1, 1)));
                // The same session under the default budget, then fifty undos.
                BrushCoverageCache bounded(std::size_t{512} << 20, tile);
                for (std::size_t i = 1; i < lists.size(); ++i) {
                    (void)bounded.coverage(lists[i], frame);
                }
                std::vector<double> undo;
                int hits = 0;
                int others = 0;
                for (std::size_t back = 1; back <= 50 && back < lists.size() - 1; ++back) {
                    const auto& earlier = lists[lists.size() - 1 - back];
                    BrushCoverageCache::Result result;
                    undo.push_back(timed([&] { result = bounded.coverage(earlier, frame); }));
                    (result.lookup == CoverageLookup::Hit ? hits : others)++;
                }
                Line("B4.session")("config", label)("appendMs", spreadOf(append))(
                    "dirtyTiles", spreadOf(dirty))("unboundedMemoryMiB",
                                                   static_cast<double>(memory) / (1024.0 * 1024.0))(
                    "boundedMemoryMiB",
                    static_cast<double>(bounded.memoryBytes()) / (1024.0 * 1024.0))(
                    "undoMs", spreadOf(undo))("undoHits", hits)("undoNotHits", others)(
                    "fullEveryAppendMedianMs", spreadOf(full).median)("redoneTilesMedianMs",
                                                                      spreadOf(redone).median)(
                    "incrementalSampledMedianMs", spreadOf(incrementalOnSampled).median)(
                    "incrementalOverRedone", spreadOf(incrementalOnSampled).median /
                                                 std::max(1e-9, spreadOf(redone).median));
            }
        }
    }
    finish("B4");
}

// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::array<int, 16> bayer{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float halfRounded(float value) {
    auto bits = std::bit_cast<std::uint32_t>(value);
    bits += 0xFFFU + ((bits >> 13) & 1U);
    bits &= ~0x1FFFU;
    return std::bit_cast<float>(bits);
}

struct Quantiser {
    const char* name;
    std::function<float(float, std::uint32_t, std::uint32_t)> apply;
};

} // namespace

namespace {

/// Linear luminance of a mid-grey (0.18) pushed by `ev` stops in proportion to coverage.
double luminance(float q, float ev) {
    return 0.18 * std::exp2(static_cast<double>(ev) * static_cast<double>(q));
}

/// Lightness L* of a relative luminance.
double lightnessOf(double y) {
    return 116.0 * std::cbrt(y) - 16.0;
}

/// The profile down one column of a plane after an 8 by 8 box blur in linear light, as L*.
///
/// A dither makes steps between neighbouring pixels by design and the eye integrates them, so
/// the judgement is on what is left after averaging over the dither period (4 pixels).
std::vector<double> blurredProfile(const std::function<float(std::uint32_t, std::uint32_t)>& value,
                                   std::uint32_t column, std::uint32_t height, float ev) {
    constexpr std::uint32_t half = 4;
    std::vector<double> rowMean(height);
    for (std::uint32_t y = 0; y < height; ++y) {
        double sum = 0.0;
        for (std::uint32_t x = column - half; x < column + half; ++x) {
            sum += luminance(value(x, y), ev);
        }
        rowMean[y] = sum / (2.0 * half);
    }
    std::vector<double> out(height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t first = y >= half ? y - half : 0;
        const std::uint32_t last = std::min(height, y + half);
        double sum = 0.0;
        for (std::uint32_t row = first; row < last; ++row) {
            sum += rowMean[row];
        }
        out[y] = lightnessOf(sum / (last - first));
    }
    return out;
}

} // namespace

TEST_CASE("B5 precision of the coverage", "[.bench][brush-bench]") {
    begin("B5");
    constexpr ImageSize frame{6000, 4000};
    const std::vector<Quantiser> quantisers{
        {"float", [](float m, std::uint32_t, std::uint32_t) { return m; }},
        {"u8",
         [](float m, std::uint32_t, std::uint32_t) {
             return static_cast<float>(static_cast<std::uint8_t>(m * 255.0F + 0.5F)) / 255.0F;
         }},
        {"u8bayer",
         [](float m, std::uint32_t x, std::uint32_t y) {
             const float threshold =
                 (static_cast<float>(bayer[(y & 3U) * 4 + (x & 3U)]) + 0.5F) / 16.0F;
             return std::floor(m * 255.0F + threshold) / 255.0F;
         }},
        {"half", [](float m, std::uint32_t, std::uint32_t) { return halfRounded(m); }},
        {"u16", [](float m, std::uint32_t, std::uint32_t) {
             return static_cast<float>(static_cast<std::uint16_t>(m * 65535.0F + 0.5F)) / 65535.0F;
         }}};
    const auto output = benchOutput();
    for (const std::uint32_t overlaps : {1U, 4U}) {
        std::vector<Stroke> strokes;
        for (std::uint32_t i = 0; i < overlaps; ++i) {
            const float shift = 0.02F * static_cast<float>(i);
            strokes.push_back(test::straightStroke({0.1F, 0.5F - shift}, {0.9F, 0.5F + shift}, 40,
                                                   0.3F, 0.0F, 0.05F));
        }
        const CoveragePlane plane = rasteriseBrush(StrokeList(strokes), frame);
        constexpr std::uint32_t column = 3000;
        const auto exact = [&](std::uint32_t x, std::uint32_t y) {
            return plane.values[static_cast<std::size_t>(y) * frame.width + x];
        };
        for (const Quantiser& quantiser : quantisers) {
            const auto quantised = [&](std::uint32_t x, std::uint32_t y) {
                return quantiser.apply(exact(x, y), x, y);
            };
            // Down the column: the exact and the quantised value of each pixel.
            std::vector<float> m(frame.height);
            std::vector<float> q(frame.height);
            for (std::uint32_t y = 0; y < frame.height; ++y) {
                m[y] = exact(column, y);
                q[y] = quantised(column, y);
            }
            double maxError = 0.0;
            std::size_t plateau = 0;
            std::size_t run = 1;
            for (std::uint32_t y = 0; y < frame.height; ++y) {
                maxError = std::max(maxError, static_cast<double>(std::abs(q[y] - m[y])));
                if (y > 0) {
                    run = (q[y] == q[y - 1] && m[y] != m[y - 1]) ? run + 1 : 1;
                    plateau = std::max(plateau, run);
                }
            }
            Line line("B5.precision");
            line("overlaps", overlaps)("quantiser", quantiser.name)("maxAbsError", maxError)(
                "longestPlateauPx", plateau);
            for (const float ev : {4.0F, 1.0F}) {
                const std::string tag = ev == 4.0F ? ".ev+4" : ".ev+1";
                // 1. The step between vertical neighbours of one column, as the old metric
                //    had it: a dither shows up here by design.
                double neighbourStep = 0.0;
                for (std::uint32_t y = 1; y < frame.height; ++y) {
                    neighbourStep =
                        std::max(neighbourStep, std::abs(lightnessOf(luminance(q[y], ev)) -
                                                         lightnessOf(luminance(q[y - 1], ev))));
                }
                // 2. The 8 by 8 blurred profile: its worst step and its worst departure from
                //    the blurred float profile are what the eye sees.
                const std::vector<double> blurred =
                    blurredProfile(quantised, column, frame.height, ev);
                const std::vector<double> truth = blurredProfile(exact, column, frame.height, ev);
                double blurredStep = 0.0;
                double meanError = 0.0;
                for (std::uint32_t y = 0; y < frame.height; ++y) {
                    meanError = std::max(meanError, std::abs(blurred[y] - truth[y]));
                    if (y > 0) {
                        blurredStep = std::max(blurredStep, std::abs(blurred[y] - blurred[y - 1]));
                    }
                }
                // 3. The contour: the step in L* between neighbouring plateaus of at least four
                //    pixels, which is a visible edge. A dither has no such plateau.
                double contour = 0.0;
                std::size_t start = 0;
                double previousLevel = -1.0;
                for (std::uint32_t y = 1; y <= frame.height; ++y) {
                    if (y == frame.height || q[y] != q[y - 1]) {
                        if (y - start >= 4 && m[start] != m[y - 1]) {
                            const double level = lightnessOf(luminance(q[start], ev));
                            if (previousLevel >= 0.0) {
                                contour = std::max(contour, std::abs(level - previousLevel));
                            }
                            previousLevel = level;
                        } else {
                            previousLevel = -1.0;
                        }
                        start = y;
                    }
                }
                line(("neighbourStepDeltaLstar" + tag).c_str(),
                     neighbourStep)(("blurredStepDeltaLstar" + tag).c_str(), blurredStep)(
                    ("blurredMeanErrorDeltaLstar" + tag).c_str(),
                    meanError)(("contourDeltaLstar" + tag).c_str(), contour);
            }
            if (output && overlaps == 1 &&
                (std::string(quantiser.name) == "float" || std::string(quantiser.name) == "u8" ||
                 std::string(quantiser.name) == "u8bayer")) {
                const Quantiser& chosen = quantiser;
                exportGrey(*output / (std::string("B5_") + chosen.name + "_ev4.png"), 800, 1200,
                           [&](std::uint32_t x, std::uint32_t y) {
                               const std::uint32_t px = 2600 + x;
                               const std::uint32_t py = 1400 + y;
                               const float v = chosen.apply(
                                   plane.values[static_cast<std::size_t>(py) * frame.width + px],
                                   px, py);
                               return static_cast<float>(lightnessOf(luminance(v, 4.0F)) / 100.0);
                           });
            }
        }
    }
    finish("B5");
}

// ---------------------------------------------------------------------------------------------

namespace {

CoveragePlane boxHalved(const CoveragePlane& plane) {
    CoveragePlane out{{plane.size.width / 2, plane.size.height / 2}, {}};
    out.values.resize(static_cast<std::size_t>(out.size.width) * out.size.height);
    for (std::uint32_t y = 0; y < out.size.height; ++y) {
        for (std::uint32_t x = 0; x < out.size.width; ++x) {
            const auto at = [&](std::uint32_t dx, std::uint32_t dy) {
                return plane
                    .values[static_cast<std::size_t>(2 * y + dy) * plane.size.width + 2 * x + dx];
            };
            out.values[static_cast<std::size_t>(y) * out.size.width + x] =
                (at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1)) * 0.25F;
        }
    }
    return out;
}

/// Bilinear upsampling with aligned centres and clamped edges.
CoveragePlane upsampled(const CoveragePlane& coarse, ImageSize size) {
    CoveragePlane out{size, std::vector<float>(static_cast<std::size_t>(size.width) * size.height)};
    const auto at = [&](std::int64_t x, std::int64_t y) {
        x = std::clamp<std::int64_t>(x, 0, coarse.size.width - 1);
        y = std::clamp<std::int64_t>(y, 0, coarse.size.height - 1);
        return coarse.values[static_cast<std::size_t>(y) * coarse.size.width + x];
    };
    for (std::uint32_t y = 0; y < size.height; ++y) {
        const double sy = (y + 0.5) * coarse.size.height / size.height - 0.5;
        const auto y0 = static_cast<std::int64_t>(std::floor(sy));
        const auto fy = static_cast<float>(sy - static_cast<double>(y0));
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double sx = (x + 0.5) * coarse.size.width / size.width - 0.5;
            const auto x0 = static_cast<std::int64_t>(std::floor(sx));
            const auto fx = static_cast<float>(sx - static_cast<double>(x0));
            const float top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
            const float bottom = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
            out.values[static_cast<std::size_t>(y) * size.width + x] = top * (1 - fy) + bottom * fy;
        }
    }
    return out;
}

struct Errors {
    double mean = 0.0;
    double max = 0.0;
    double over4 = 0.0;
    double over32 = 0.0;
};

Errors errorsOf(const CoveragePlane& a, const CoveragePlane& ideal) {
    Errors e;
    for (std::size_t i = 0; i < a.values.size(); ++i) {
        const double d = std::abs(static_cast<double>(a.values[i]) - ideal.values[i]);
        e.mean += d;
        e.max = std::max(e.max, d);
        e.over4 += d > 4.0 / 255.0 ? 1.0 : 0.0;
        e.over32 += d > 32.0 / 255.0 ? 1.0 : 0.0;
    }
    const auto n = static_cast<double>(a.values.size());
    e.mean /= n;
    e.over4 /= n;
    e.over32 /= n;
    return e;
}

} // namespace

TEST_CASE("B6 resolution of a hardness 1 coverage", "[.bench][brush-bench]") {
    begin("B6");
    const auto output = benchOutput();
    for (const ImageSize frame : {ImageSize{3000, 2000}, ImageSize{1500, 1000}}) {
        std::vector<Stroke> strokes;
        std::mt19937_64 g(seed + 60);
        for (int i = 0; i < 40; ++i) {
            strokes.push_back(test::wanderingStroke(g, test::detailStyle, 2.0 / 3.0));
        }
        for (int i = 0; i < 3; ++i) {
            Stroke big = test::wanderingStroke(g, test::everydayStyle, 2.0 / 3.0);
            big.radius = 0.05F;
            big.hardness = 1.0F;
            big.erase = false;
            big.flow = 1.0F;
            strokes.push_back(std::move(big));
        }
        const StrokeList list(std::move(strokes));
        const ImageSize coarseSize{(frame.width + 1) / 2, (frame.height + 1) / 2};
        const ImageSize idealSize{frame.width * 2, frame.height * 2};
        CoveragePlane full;
        CoveragePlane coarse;
        const double fullMs = timed([&] { full = rasteriseBrush(list, frame); });
        const double coarseMs = timed([&] { coarse = rasteriseBrush(list, coarseSize); });
        const CoveragePlane ideal = boxHalved(rasteriseBrush(list, idealSize));
        const CoveragePlane lifted = upsampled(coarse, frame);
        const Errors fullErrors = errorsOf(full, ideal);
        const Errors coarseErrors = errorsOf(lifted, ideal);
        const auto name = std::to_string(frame.width) + "x" + std::to_string(frame.height);
        Line("B6.error")("frame", name)("variant", "full")("mean", fullErrors.mean)(
            "max", fullErrors.max)("fractionOver4of255", fullErrors.over4)("fractionOver32of255",
                                                                           fullErrors.over32)(
            "rasteriseMs", fullMs)("MiB", static_cast<double>(full.values.size() * 4) / 1048576.0);
        Line("B6.error")("frame", name)("variant", "coarse")("mean", coarseErrors.mean)(
            "max", coarseErrors.max)("fractionOver4of255", coarseErrors.over4)(
            "fractionOver32of255", coarseErrors.over32)("rasteriseMs", coarseMs)(
            "MiB", static_cast<double>(coarse.values.size() * 4) / 1048576.0);
        if (output) {
            const auto crop = [&](const char* label, const CoveragePlane& plane) {
                const std::uint32_t w = std::min<std::uint32_t>(600, frame.width);
                const std::uint32_t h = std::min<std::uint32_t>(400, frame.height);
                const std::uint32_t x0 = frame.width / 3;
                const std::uint32_t y0 = frame.height / 3;
                exportGrey(*output / ("B6_" + name + "_" + label + ".png"), w, h,
                           [&](std::uint32_t x, std::uint32_t y) {
                               return plane
                                   .values[static_cast<std::size_t>(y0 + y) * frame.width + x0 + x];
                           });
            };
            crop("full", full);
            crop("coarse", lifted);
            crop("ideal", ideal);
        }
    }
    finish("B6");
}

// ---------------------------------------------------------------------------------------------

namespace {

/// A zigzag of points that cross the frame, left to right and back, with as many points as the
/// swept-area and dab budgets of a mask allow at this radius (or ::arraw::maximumStrokePoints).
Stroke budgetFillingStroke(float radius) {
    Stroke stroke{radius, 0.5F, 0.5F, false, {}};
    for (std::uint32_t i = 0; i < maximumStrokePoints; ++i) {
        stroke.points.push_back(
            {i % 2 == 0 ? 0.05F : 0.95F, 0.05F + 0.9F * static_cast<float>(i % 7) / 6.0F});
        if (!budgetOf(stroke).withinMaskLimits()) {
            stroke.points.pop_back();
            break;
        }
    }
    return stroke;
}

/// Everyday strokes of 100 points, as many as a mask holds.
StrokeList everydayFill() {
    test::StrokeStyle style = test::everydayStyle;
    style.pointsLow = style.pointsHigh = 100;
    std::mt19937_64 g(seed + 70);
    std::shared_ptr<const StrokeList> list = std::make_shared<const StrokeList>();
    for (;;) {
        Stroke next = test::wanderingStroke(g, style, 2.0 / 3.0);
        if (!list->accepts(next)) {
            return *list;
        }
        list = list->appended(std::move(next));
    }
}

} // namespace

TEST_CASE("B7 what the budgets allow", "[.bench][brush-bench]") {
    begin("B7");
    // Worst lists the caps of ADR 044 section 6 admit: one stroke that fills the swept-area or
    // dab budget at a radius, and everyday strokes up to a budget. Measured, not extrapolated.
    struct Stand {
        const char* name;
        std::function<StrokeList()> make;
    };
    const std::vector<Stand> stands{
        {"radius1-fills-area",
         [] { return StrokeList(std::vector<Stroke>{budgetFillingStroke(maximumBrushRadius)}); }},
        {"radius0.1-fills-area",
         [] { return StrokeList(std::vector<Stroke>{budgetFillingStroke(0.1F)}); }},
        {"radius0.0005-fills-dabs",
         [] { return StrokeList(std::vector<Stroke>{budgetFillingStroke(minimumBrushRadius)}); }},
        {"everyday-100-point-strokes", [] { return everydayFill(); }}};
    for (const ImageSize frame : {ImageSize{1500, 1000}, ImageSize{6000, 4000}}) {
        for (const Stand& stand : stands) {
            const StrokeList list = stand.make();
            std::size_t dabs = 0;
            const double placeMs = bestOfFive([&] { dabs = countDabs(list, frame); });
            const double pixels = dabPixels(list, frame);
            const double rasteriseMs = timed([&] { (void)rasteriseBrush(list, frame); });
            const double seconds = rasteriseMs / 1000.0;
            Line("B7.standIn")("frame",
                               std::to_string(frame.width) + "x" + std::to_string(frame.height))(
                "case", stand.name)("strokes", list.size())("points", list.pointCount())(
                "sweptArea", list.budget().sweptArea)("budgetDabs", list.budget().dabs)(
                "dabs", dabs)("placementMs", placeMs)("rasteriseMs", rasteriseMs)(
                "dabsPerSecond", static_cast<double>(dabs) / seconds)("dabPixelsPerSecond",
                                                                      pixels / seconds);
        }
    }
    // Sidecar bytes of a mask at its point cap, from one realistic 10 000-point stroke.
    std::mt19937_64 g(seed + 71);
    test::StrokeStyle style = test::everydayStyle;
    style.pointsLow = style.pointsHigh = 10'000;
    const Stroke stroke = test::wanderingStroke(g, style, 2.0 / 3.0);
    constexpr double strokesAtCap = static_cast<double>(maximumPointsPerMask) / 10'000.0;
    Line("B7.sidecarAtCaps")("textMiB", static_cast<double>(strokeText(stroke).size()) *
                                            strokesAtCap / 1048576.0)(
        "base64MiB", static_cast<double>(strokeBase64(stroke).size()) * strokesAtCap / 1048576.0);
    finish("B7");
}

// ---------------------------------------------------------------------------------------------

TEST_CASE("B8 the cost of coverage in a render", "[.bench][brush-bench]") {
    // Step 5.2: the constants of RenderProgress.cpp (coverageCost, packCost) and a cache miss of
    // B2's sixteen masks through the cache's bucketed `drawn`, beside the banded path.
    begin("B8");
    constexpr ImageSize frame{6000, 4000};
    const double pixels = static_cast<double>(frame.pixelCount());
    const double edge = std::max(frame.width, frame.height);
    std::vector<std::shared_ptr<const StrokeList>> masks;
    DevelopState state;
    for (std::uint32_t i = 0; i < 16; ++i) {
        masks.push_back(test::paintedMask(seed + i, 60, test::everydayStyle, 4000.0 / 6000.0));
        LocalAdjustment adjustment;
        adjustment.shape = BrushMask{masks.back()};
        adjustment.deltas.exposure = 1.0F;
        state = withLocalAdjustmentAdded(std::move(state), adjustment);
    }
    // Per mask: a miss through the cache (not retained), then packing the tiles it made.
    double drawnMs = 0.0;
    double packMs = 0.0;
    double areaEdgeSquared = 0.0;
    for (std::size_t i = 0; i < masks.size(); ++i) {
        BrushCoverageCache cache;
        BrushCoverageCache::Result result;
        const double ms = timed([&] { result = cache.coverage(masks[i], frame, false); });
        drawnMs += ms;
        areaEdgeSquared += masks[i]->budget().sweptArea * edge * edge;
        detail::PackedCoverage packed;
        packed.size = frame;
        packed.planes.assign(1, std::vector<std::uint8_t>(frame.pixelCount() * 4));
        const double pack = bestOfThree(
            [&] { detail::quantiseInto(*result.tiles, 0, static_cast<std::uint32_t>(i), packed); });
        packMs += pack;
        Line("B8.mask")("index", i)("drawnMs", ms)("sweptArea",
                                                   masks[i]->budget().sweptArea)("packMs", pack);
    }
    // All sixteen banded, outside the cache, in one call.
    const LocalPlan local = localPlanFor(state, frame);
    double bandedMs = timed([&] { (void)detail::packBanded(local); });
    Line("B8.total")("drawnMsAll16", drawnMs)("bandedMsAll16", bandedMs)(
        "coverageCostNsPerAreaEdge2", drawnMs * 1e6 / areaEdgeSquared)(
        "packCostNsPerPixel", packMs * 1e6 / (pixels * 16.0))("bandedNsPerAreaEdge2",
                                                              bandedMs * 1e6 / areaEdgeSquared);
    finish("B8");
}
