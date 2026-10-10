// The two candidate encodings of a stroke (ADR 044, section 7 and 9).

#include "StrokeCodec.h"
#include "support/BrushGenerators.h"

#include <catch2/catch_test_macros.hpp>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
#include <string>

using namespace arraw;

namespace {

bool sameBits(const Stroke& a, const Stroke& b) {
    const auto bits = [](float x) {
        std::uint32_t out = 0;
        std::memcpy(&out, &x, 4);
        return out;
    };
    if (bits(a.radius) != bits(b.radius) || bits(a.hardness) != bits(b.hardness) ||
        bits(a.flow) != bits(b.flow) || a.erase != b.erase || a.points.size() != b.points.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (bits(a.points[i].u) != bits(b.points[i].u) ||
            bits(a.points[i].v) != bits(b.points[i].v)) {
            return false;
        }
    }
    return true;
}

Stroke awkward() {
    Stroke stroke{0.02F, 0.5F, 1.0F, true, {}};
    const float values[] = {0.0F,
                            -0.0F,
                            1e-45F,
                            FLT_MIN,
                            FLT_MAX,
                            0.1F,
                            -2.0F,
                            3.0F,
                            std::nextafter(0.5F, 0.0F),
                            std::nextafter(0.5F, 1.0F)};
    for (std::size_t i = 0; i < std::size(values); ++i) {
        stroke.points.push_back({values[i], values[(i + 3) % std::size(values)]});
    }
    return stroke;
}

Stroke tenThousand() {
    std::mt19937_64 g(20261009);
    Stroke stroke = test::zigzagStroke(maximumStrokePoints, 40, 0.01F, 0.3F, 0.7F);
    for (SensorPoint& point : stroke.points) {
        point.u += static_cast<float>(test::unitDouble(g)) * 1e-3F;
    }
    return stroke;
}

} // namespace

TEST_CASE("Both encodings round-trip a stroke exactly", "[brush][codec]") {
    for (const Stroke& stroke : {awkward(), tenThousand()}) {
        const DecodedStroke text = strokeFromText(strokeText(stroke));
        CHECK(sameBits(text.stroke, stroke));
        CHECK(text.pointsDropped == 0);
        const DecodedStroke binary = strokeFromBase64(strokeBase64(stroke));
        CHECK(sameBits(binary.stroke, stroke));
        CHECK(binary.pointsDropped == 0);
    }
}

TEST_CASE("The text form is the one the ADR gives", "[brush][codec]") {
    Stroke stroke{0.02F, 0.5F, 1.0F, false, {{0.25F, 0.5F}, {0.2501F, 0.5003F}}};
    CHECK(strokeText(stroke) == "0.02 0.5 1 0;0.25,0.5 0.2501,0.5003");
    const DecodedStroke spaced =
        strokeFromText("\n 0.02\t0.5  1 0 ;\n0.25,0.5\r\n0.2501,0.5003 \n");
    CHECK(sameBits(spaced.stroke, stroke));
}

TEST_CASE("Malformed input is refused", "[brush][codec]") {
    for (const char* text :
         {"", "0.02 0.5 1 0;", "0.02 0.5 1 2;0.1,0.1", "0.02 0.5 1 0;0.1",
          "0.02 0.5 1 0;0.1,0.1,0.5", "0.02 0.5 1 0;nan,0.1", "0.02 0.5 1 0;0.1,inf",
          "0.02 +0.5 1 0;0.1,0.1", "0.02 0.5 1 0;+0.1,0.1", "0.02 0.5 1 0;0.1,0.1 junk",
          "0.02 0.5 1 0;0.1,0.1x", "0.02 0.5 1;0.1,0.1", "0.02 0.5 1 0 0.1,0.1",
          "0.02 0.5 1 0;0.1,", "0.02 0.5 1e999 0;0.1,0.1", "0.02 0.5 1 0;0.1,0.1;"}) {
        INFO(text);
        CHECK_THROWS_AS(strokeFromText(text), std::invalid_argument);
    }

    const Stroke stroke{0.02F, 0.5F, 1.0F, true, {{0.25F, 0.5F}, {0.3F, 0.4F}}};
    const std::string good = strokeBase64(stroke);
    CHECK_NOTHROW(strokeFromBase64(good));
    CHECK_NOTHROW(strokeFromBase64(good.substr(0, 8) + "\n " + good.substr(8)));
    CHECK_THROWS_AS(strokeFromBase64(""), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(good.substr(1)), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64("!" + good.substr(1)), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64("==" + good.substr(2)), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(good + good), std::invalid_argument);

    // Bytes the decoder must refuse, spelled out and encoded by hand.
    const auto base64 = [](const std::vector<std::uint8_t>& bytes) {
        static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const std::size_t left = bytes.size() - i;
            const unsigned chunk = (bytes[i] << 16) | (left > 1 ? bytes[i + 1] << 8 : 0) |
                                   (left > 2 ? bytes[i + 2] : 0);
            out += alphabet[(chunk >> 18) & 63];
            out += alphabet[(chunk >> 12) & 63];
            out += left > 1 ? alphabet[(chunk >> 6) & 63] : '=';
            out += left > 2 ? alphabet[chunk & 63] : '=';
        }
        return out;
    };
    // format 1, flags 0, radius/hardness/flow 0.5 (0x3F000000), then the tail.
    const auto with = [](std::uint8_t format, std::uint8_t flags, std::vector<std::uint8_t> tail) {
        std::vector<std::uint8_t> bytes{format, flags};
        for (int i = 0; i < 3; ++i) {
            bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x3F});
        }
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        return bytes;
    };
    CHECK_NOTHROW(strokeFromBase64(base64(with(1, 0, {1, 0x80, 0x80, 0x80, 0x80, 0x08, 0x02}))));
    // One number has one spelling: 0 as 0x80 0x00 is refused.
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {1, 0x80, 0x00, 2}))),
                    std::invalid_argument);
    CHECK_NOTHROW(strokeFromBase64(base64(with(1, 0, {1, 0x80, 0x01, 2}))));
    CHECK_THROWS_AS(strokeFromBase64(base64(with(2, 0, {1, 2, 2}))), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 2, {1, 2, 2}))), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {1, 0x80}))), std::invalid_argument);
    CHECK_THROWS_AS(
        strokeFromBase64(base64(with(1, 0, {1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 2}))),
        std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {0, 2, 2}))), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {2, 2, 2}))), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {1, 2, 2, 0}))), std::invalid_argument);
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 2, 2}))),
                    std::invalid_argument);
    // A point that lands past 32 bits: zigzag of +2^32.
    CHECK_THROWS_AS(strokeFromBase64(base64(with(1, 0, {1, 0x80, 0x80, 0x80, 0x80, 0x20, 2}))),
                    std::invalid_argument);
    // Non-finite radius (infinity) in the style.
    std::vector<std::uint8_t> infinite = with(1, 0, {1, 2, 2});
    infinite[4] = 0x80;
    infinite[5] = 0x7F;
    CHECK_THROWS_AS(strokeFromBase64(base64(infinite)), std::invalid_argument);
}

TEST_CASE("A point past the limit is cut and counted", "[brush][codec]") {
    Stroke stroke = test::zigzagStroke(12'000, 40, 0.01F, 0.3F, 0.7F);
    for (const DecodedStroke& decoded :
         {strokeFromText(strokeText(stroke)), strokeFromBase64(strokeBase64(stroke))}) {
        CHECK(decoded.stroke.points.size() == 10'000);
        CHECK(decoded.pointsDropped == 2000);
        CHECK(decoded.stroke.points.front() == stroke.points.front());
        CHECK(decoded.stroke.points.back() == stroke.points[9999]);
    }
}
