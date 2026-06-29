#include "core/Orientation.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/DevelopGroup.h"
#include "develop/GlobalAdjustment.h"
#include "develop/LocalAdjustment.h"
#include "develop/Spot.h"

#include <catch2/catch_test_macros.hpp>

#include <QRectF>

// A GlobalAdjustment with a distinct non-default value in *every* global field,
// so a copy that misses a field is detectable. localAdjustments is left empty
// here and exercised separately.
static GlobalAdjustment fullyEdited() {
    GlobalAdjustment g;
    // White Balance
    g.temperature = 7200.0f;
    g.tint = 12.0f;
    // Tone (SharedAdjustment)
    g.exposure = 1.5f;
    g.contrast = 20.0f;
    g.highlights = -30.0f;
    g.shadows = 40.0f;
    g.whites = 10.0f;
    g.blacks = -15.0f;
    g.filmicHighlights = 60.0f;
    // Colour
    g.saturation = 25.0f;
    g.vibrance = -8.0f;
    // Tone Curve
    g.curveLuma.points = {{0.0, 0.0}, {0.4, 0.5}, {1.0, 1.0}};
    g.curveR.points = {{0.0, 0.1}, {1.0, 1.0}};
    g.curveG.points = {{0.0, 0.0}, {1.0, 0.9}};
    g.curveB.points = {{0.0, 0.05}, {1.0, 0.95}};
    // HSL
    g.hslHue = {1, 2, 3, 4, 5, 6, 7, 8};
    g.hslSat = {-1, -2, -3, -4, -5, -6, -7, -8};
    g.hslLum = {9, 8, 7, 6, 5, 4, 3, 2};
    // Detail
    g.demosaicAlgorithm = DemosaicAlgorithm::VNG;
    g.texture = 18.0f;
    g.clarity = 22.0f;
    g.dehaze = -12.0f;
    g.sharpening = 55.0f;
    // Geometry
    g.orientation = orient::Orientation{3, true};
    g.rotation = 7.5f;
    g.cropRect = {0.1, 0.2, 0.7, 0.6};
    g.cropConstrained = true;
    // Lens Corrections
    g.lensCorrectDistortion = true;
    g.lensCorrectVignetting = true;
    g.lensCorrectCA = true;
    // Effects (the seed is deliberately per-image, not part of the group)
    g.postCropVignetteAmount = -35.0f;
    g.postCropVignetteMidpoint = 62.0f;
    g.postCropVignetteFeather = 78.0f;
    g.grainAmount = 24.0f;
    g.grainSize = 40.0f;
    g.grainRoughness = 71.0f;
    g.grainSeed = 12345;
    return g;
}

static GroupSelection only(DevelopGroup g) {
    GroupSelection s;
    s.set(static_cast<size_t>(g));
    return s;
}

TEST_CASE("the default copy selection checks every group except Geometry", "[developgroup]") {
    const GroupSelection sel = defaultCopySelection();
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const DevelopGroup g = static_cast<DevelopGroup>(i);
        if (g == DevelopGroup::Geometry)
            CHECK_FALSE(hasGroup(sel, g));
        else
            CHECK(hasGroup(sel, g));
    }
}

TEST_CASE("Empty selection leaves the target unchanged", "[developgroup]") {
    const GlobalAdjustment target; // defaults
    const GlobalAdjustment source = fullyEdited();

    REQUIRE(applyGroups(target, source, GroupSelection{}) == target);
}

TEST_CASE("Selecting one group copies only that group's fields", "[developgroup]") {
    const GlobalAdjustment target; // defaults
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::Tone));

    // The Tone fields came across...
    CHECK(result.exposure == source.exposure);
    CHECK(result.contrast == source.contrast);
    CHECK(result.highlights == source.highlights);
    CHECK(result.shadows == source.shadows);
    CHECK(result.whites == source.whites);
    CHECK(result.blacks == source.blacks);
    CHECK(result.filmicHighlights == source.filmicHighlights);

    // ...and nothing outside Tone moved.
    CHECK(result.temperature == target.temperature);
    CHECK(result.tint == target.tint);
    CHECK(result.saturation == target.saturation);
    CHECK(result.vibrance == target.vibrance);
    CHECK(result.sharpening == target.sharpening);
    CHECK(result.rotation == target.rotation);
    CHECK(result.curveLuma == target.curveLuma);
}

TEST_CASE("Replace is wholesale: pasting a default group resets the target", "[developgroup]") {
    const GlobalAdjustment source;           // unedited Tone (all defaults)
    GlobalAdjustment target = fullyEdited(); // target has heavy Tone edits

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::Tone));

    // Pasting an unedited Tone group resets the target's Tone to defaults.
    CHECK(result.exposure == 0.0f);
    CHECK(result.contrast == 0.0f);
    CHECK(result.blacks == 0.0f);
    // Untouched groups survive.
    CHECK(result.saturation == target.saturation);
}

TEST_CASE("Geometry moves rotation, crop, and the aspect-lock flag together", "[developgroup]") {
    const GlobalAdjustment target; // defaults: no rotation, full-frame crop
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::Geometry));

    CHECK(result.orientation == source.orientation);
    CHECK(result.rotation == source.rotation);
    CHECK(result.cropRect == source.cropRect);
    CHECK(result.cropConstrained == source.cropConstrained);
    // Geometry must not drag tonal fields along.
    CHECK(result.exposure == target.exposure);
}

TEST_CASE("Detail carries the demosaic algorithm; reset returns AHD", "[developgroup]") {
    const GlobalAdjustment target; // defaults: demosaicAlgorithm == AHD
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::Detail));
    CHECK(result.demosaicAlgorithm == source.demosaicAlgorithm);
    CHECK(result.texture == source.texture);
    CHECK(result.clarity == source.clarity);
    CHECK(result.dehaze == source.dehaze);
    CHECK(result.sharpening == source.sharpening);

    // Pasting an unedited Detail group (Replace semantics) resets it back to AHD.
    const GlobalAdjustment reset
        = applyGroups(source, GlobalAdjustment{}, only(DevelopGroup::Detail));
    CHECK(reset.demosaicAlgorithm == DemosaicAlgorithm::AHD);
    CHECK(reset.texture == 0.0f);
    CHECK(reset.clarity == 0.0f);
    CHECK(reset.dehaze == 0.0f);
}

TEST_CASE("Lens Corrections moves only its enable toggles", "[developgroup]") {
    const GlobalAdjustment target; // defaults: all toggles off
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::LensCorrections));

    CHECK(result.lensCorrectDistortion == source.lensCorrectDistortion);
    CHECK(result.lensCorrectVignetting == source.lensCorrectVignetting);
    CHECK(result.lensCorrectCA == source.lensCorrectCA);
    // Must not drag the post-crop vignette (a different group) along.
    CHECK(result.postCropVignetteAmount == target.postCropVignetteAmount);
}

TEST_CASE("White Balance carries temperature and tint", "[developgroup]") {
    const GlobalAdjustment target;
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::WhiteBalance));

    CHECK(result.temperature == source.temperature);
    CHECK(result.tint == source.tint);
    CHECK(result.saturation == target.saturation); // tint is WB, sat is Colour
}

TEST_CASE("Effects carries visible controls but preserves the target Grain seed", "[developgroup]") {
    GlobalAdjustment target;
    target.grainSeed = 99;
    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, only(DevelopGroup::Effects));

    CHECK(result.postCropVignetteAmount == source.postCropVignetteAmount);
    CHECK(result.postCropVignetteMidpoint == source.postCropVignetteMidpoint);
    CHECK(result.postCropVignetteFeather == source.postCropVignetteFeather);
    CHECK(result.grainAmount == source.grainAmount);
    CHECK(result.grainSize == source.grainSize);
    CHECK(result.grainRoughness == source.grainRoughness);
    CHECK(result.grainSeed == target.grainSeed);
}

TEST_CASE(
    "All groups reproduce the source's visible globals but never per-image state",
    "[developgroup]") {
    // Exhaustiveness guard: if any global field belongs to no group, it stays at
    // the target's value and this fails — enforcing "no field silently fails to
    // copy" as a test, not a hope.
    GlobalAdjustment target; // defaults...
    LocalAdjustment keep;    // ...plus per-image edits to protect
    keep.exposure = 0.3f;
    target.localAdjustments = {keep};
    Spot spot;
    spot.destination = {12.0, 20.0};
    spot.source = {18.0, 20.0};
    spot.radius = 4.0;
    target.spots = {spot};
    target.grainSeed = 9876;

    const GlobalAdjustment source = fullyEdited();

    const GlobalAdjustment result = applyGroups(target, source, allGroups());

    // Every global field now matches the source...
    GlobalAdjustment expected = source;
    expected.localAdjustments = target.localAdjustments;
    expected.spots = target.spots;
    expected.grainSeed = target.grainSeed;
    CHECK(result == expected);
}
