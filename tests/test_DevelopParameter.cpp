#include "DevelopParameter.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

// A GlobalAdjustment with a distinct non-default value in *every* parameter's
// backing field, so a per-parameter diff that misses a field is detectable.
// (Mirrors test_DevelopGroup's helper — the grain seed is per-image, not a
// parameter, so it is irrelevant here.)
static GlobalAdjustment fullyEdited() {
    GlobalAdjustment g;
    g.temperature = 7200.0f;
    g.tint = 12.0f;
    g.exposure = 1.5f;
    g.contrast = 20.0f;
    g.highlights = -30.0f;
    g.shadows = 40.0f;
    g.whites = 10.0f;
    g.blacks = -15.0f;
    g.saturation = 25.0f;
    g.vibrance = -8.0f;
    g.curveLuma.points = {{0.0, 0.0}, {0.4, 0.5}, {1.0, 1.0}};
    g.curveR.points = {{0.0, 0.1}, {1.0, 1.0}};
    g.curveG.points = {{0.0, 0.0}, {1.0, 0.9}};
    g.curveB.points = {{0.0, 0.05}, {1.0, 0.95}};
    g.hslHue = {1, 2, 3, 4, 5, 6, 7, 8};
    g.hslSat = {-1, -2, -3, -4, -5, -6, -7, -8};
    g.hslLum = {9, 8, 7, 6, 5, 4, 3, 2};
    g.sharpening = 55.0f;
    g.colorNoiseReduction = 30.0f;
    g.colorNoiseReductionSmoothness = 70.0f;
    g.lensCorrectDistortion = true;
    g.lensCorrectVignetting = true;
    g.lensCorrectCA = true;
    g.orientation = orient::Orientation{3, true};
    g.rotation = 7.5f;
    g.cropRect = {0.1, 0.2, 0.7, 0.6};
    g.cropConstrained = true;
    g.postCropVignetteAmount = -35.0f;
    g.postCropVignetteMidpoint = 62.0f;
    g.postCropVignetteFeather = 78.0f;
    g.grainAmount = 24.0f;
    g.grainSize = 40.0f;
    g.grainRoughness = 71.0f;
    return g;
}

static GroupSelection only(DevelopGroup g) {
    GroupSelection s;
    s.set(static_cast<size_t>(g));
    return s;
}

TEST_CASE("every parameter has a unique non-empty key and label", "[developparameter]") {
    std::set<std::string> keys;
    std::set<QString> labels;
    for (int i = 0; i < kDevelopParameterCount; ++i) {
        const auto p = static_cast<DevelopParameter>(i);
        const std::string key = developParameterKey(p);
        const QString label = developParameterLabel(p);
        CHECK_FALSE(key.empty());
        CHECK_FALSE(label.isEmpty());
        CHECK(keys.insert(key).second);     // no duplicate keys
        CHECK(labels.insert(label).second); // no duplicate labels
    }
}

TEST_CASE("HSL parameters read as '<Band> <Component>'", "[developparameter]") {
    CHECK(developParameterLabel(DevelopParameter::HslRedHue) == "Red Hue");
    CHECK(developParameterLabel(DevelopParameter::HslBlueLuminance) == "Blue Luminance");
    CHECK(developParameterLabel(DevelopParameter::HslMagentaSaturation) == "Magenta Saturation");
}

TEST_CASE("a single field change is reported as exactly that parameter", "[developparameter]") {
    GlobalAdjustment a;
    GlobalAdjustment b;
    b.exposure = 1.0f;
    REQUIRE(changedParameters(a, b) == std::vector{DevelopParameter::Exposure});

    GlobalAdjustment c;
    c.hslLum[5] = 30.0f; // Blue luminance
    REQUIRE(changedParameters(a, c) == std::vector{DevelopParameter::HslBlueLuminance});

    GlobalAdjustment d;
    d.cropConstrained = true; // still part of Crop
    REQUIRE(changedParameters(a, d) == std::vector{DevelopParameter::Crop});
}

TEST_CASE("identical states report no changed parameters", "[developparameter]") {
    const GlobalAdjustment a = fullyEdited();
    CHECK(changedParameters(a, a).empty());
}

// The crux test: the parameter→group map (developParameterGroup) and the
// group→field map (applyGroups) describe the same partition. For every
// parameter, *its* group must carry the change and no other group may.
TEST_CASE("each parameter is carried by exactly its own group", "[developparameter]") {
    const GlobalAdjustment base; // defaults
    const GlobalAdjustment edited = fullyEdited();
    for (int i = 0; i < kDevelopParameterCount; ++i) {
        const auto p = static_cast<DevelopParameter>(i);
        const DevelopGroup g = developParameterGroup(p);

        INFO("parameter key: " << developParameterKey(p));
        // Its own group brings the change across...
        CHECK(developParameterDiffers(p, base, applyGroups(base, edited, only(g))));
        // ...and applying every *other* group does not.
        GroupSelection others = allGroups();
        others.reset(static_cast<size_t>(g));
        CHECK_FALSE(developParameterDiffers(p, base, applyGroups(base, edited, others)));
    }
}

TEST_CASE("developChangeLabel names a single edit by its parameter", "[developparameter]") {
    GlobalAdjustment a;
    GlobalAdjustment b;
    b.contrast = 15.0f;
    CHECK(developChangeLabel(a, b) == "Contrast");
}

TEST_CASE("developChangeLabel collapses a multi-field group edit to the group name",
    "[developparameter]") {
    GlobalAdjustment a;
    GlobalAdjustment b; // a white-balance pick moves temperature and tint together
    b.temperature = 6200.0f;
    b.tint = -5.0f;
    CHECK(developChangeLabel(a, b) == developGroupLabel(DevelopGroup::WhiteBalance));
}

TEST_CASE("developChangeLabel falls back to a generic verb across groups",
    "[developparameter]") {
    GlobalAdjustment a;
    GlobalAdjustment b;
    b.exposure = 1.0f;        // Tone
    b.cropRect = {0, 0, 1, 1};
    b.rotation = 3.0f;        // Geometry
    CHECK(developChangeLabel(a, b) == "Adjust");
}

TEST_CASE("an unchanged state yields the generic verb", "[developparameter]") {
    const GlobalAdjustment a = fullyEdited();
    CHECK(developChangeLabel(a, a) == "Adjust");
}
