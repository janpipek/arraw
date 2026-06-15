#include "FieldSpec.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Exposure: slider ±500 ticks = ±5.00 EV, 2 decimals, " EV" suffix, signed.
static const FieldSpec kExposure{-500, 500, 0, 0.01f, 0.01f, 2, " EV", true};

// HSL Hue: param stays ±100 (Lightroom-compatible) but reads ±30.0° — the
// display scale (0.3) deliberately differs from the param scale (1.0).
static const FieldSpec kHue{-100, 100, 0, 1.0f, 0.3f, 1, QString::fromUtf8("°"), true};

// Temperature: unipolar Kelvin, no leading '+', " K" suffix.
static const FieldSpec kTemperature{2000, 12000, 5500, 1.0f, 1.0f, 0, " K", false};

TEST_CASE("format renders raw tick in display units with sign and suffix", "[fieldspec]") {
    REQUIRE(kExposure.format(150) == "+1.50 EV");
}

TEST_CASE(
    "parse reads a display string back to a raw tick, "
    "tolerating sign, suffix and whitespace",
    "[fieldspec]") {
    bool ok = false;
    REQUIRE(kExposure.parse("+1.50 EV", &ok) == 150);
    REQUIRE(ok);
    REQUIRE(kExposure.parse("1.5", &ok) == 150);
    REQUIRE(ok);
    REQUIRE(kExposure.parse("  -2.00 EV  ", &ok) == -200);
    REQUIRE(ok);
}

TEST_CASE("parse clamps to range and rejects non-numeric input", "[fieldspec]") {
    bool ok = false;
    REQUIRE(kExposure.parse("10 EV", &ok) == 500); // 1000 ticks -> clamped
    REQUIRE(ok);
    REQUIRE(kExposure.parse("-99", &ok) == -500);
    REQUIRE(ok);

    REQUIRE(kExposure.parse("abc", &ok) == kExposure.def);
    REQUIRE_FALSE(ok);
    REQUIRE(kExposure.parse("", &ok) == kExposure.def);
    REQUIRE_FALSE(ok);
}

TEST_CASE("raw <-> param round-trips through the param scale, clamped", "[fieldspec]") {
    REQUIRE(kExposure.toParam(150) == Catch::Approx(1.5f));
    REQUIRE(kExposure.fromParam(1.5f) == 150);
    REQUIRE(kExposure.fromParam(-2.0f) == -200);
    REQUIRE(kExposure.fromParam(99.0f) == 500); // out of range -> clamped
}

TEST_CASE("display scale is independent of param scale (Hue reads degrees)", "[fieldspec]") {
    REQUIRE(kHue.format(50) == QString::fromUtf8("+15.0°"));
    REQUIRE(kHue.format(100) == QString::fromUtf8("+30.0°"));
    REQUIRE(kHue.toParam(50) == Catch::Approx(50.0f)); // param stays ±100
    REQUIRE(kHue.parse(QString::fromUtf8("15°")) == 50);
}

TEST_CASE("unipolar field omits the leading plus sign", "[fieldspec]") {
    REQUIRE(kTemperature.format(5500) == "5500 K");
    REQUIRE(kTemperature.format(6500) == "6500 K");
}
