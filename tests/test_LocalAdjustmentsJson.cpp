// The local adjustments in the state JSON document (ADR 044, section 9).

#include "support/LocalAdjustmentStates.h"

#include <DevelopState.h>
#include <Diagnostics.h>
#include <LocalAdjustmentEdits.h>
#include <SettingsJson.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

namespace {

/// A radial mask the way the document spells it, with extras spliced in.
std::string radialJson(int id, const std::string& geometry = "", const std::string& extra = "") {
    return R"({"id": )" + std::to_string(id) +
           R"(, "type": "radial", "name": "", "enabled": true,)" +
           R"( "opacity": 1, "invert": false,)" + extra + R"( "geometry": {)" +
           (geometry.empty() ? R"("centre": [0.5, 0.5], "radius": [0.2, 0.2], "angle": 0,)"
                               R"( "feather": 0.5)"
                             : geometry) +
           R"(}, "deltas": {"exposure": 1}})";
}

std::string linearJson(int id, const std::string& geometry = "") {
    return R"({"id": )" + std::to_string(id) + R"(, "type": "linear", "geometry": {)" +
           (geometry.empty() ? R"("from": [0.5, 0.2], "to": [0.5, 0.8])" : geometry) + "}}";
}

/// A state document holding the given masks (already JSON), with extra members of the list.
std::string document(const std::vector<std::string>& masks, const std::string& listExtra = "") {
    std::string list;
    for (const std::string& mask : masks) {
        list += (list.empty() ? "" : ", ") + mask;
    }
    return R"({"arraw": 1, "settings": {}, "localAdjustments": {)" + listExtra + R"("masks": [)" +
           list + "]}}";
}

/// A document applied onto a base, with what was said about it.
struct Applied {
    DevelopState state;
    CollectedDiagnostics log;

    Applied(const std::string& json, const DevelopState& base) {
        state = applyStateJson(json, base, log);
    }
};

Applied applyDoc(const std::string& json, const DevelopState& base = {}) {
    return Applied(json, base);
}

std::size_t count(const CollectedDiagnostics& log, Notice notice) {
    std::size_t total = 0;
    for (const Diagnostic& entry : log.entries()) {
        total += entry.notice == notice ? 1 : 0;
    }
    return total;
}

/// The one warning of a kind; fails when there is not exactly one.
Diagnostic only(const CollectedDiagnostics& log, Notice notice) {
    REQUIRE(count(log, notice) == 1);
    for (const Diagnostic& entry : log.entries()) {
        if (entry.notice == notice) {
            return entry;
        }
    }
    return {.notice = notice};
}

std::vector<std::uint32_t> idsOf(const DevelopState& state) {
    std::vector<std::uint32_t> ids;
    for (const LocalAdjustment& adjustment : state.localAdjustments) {
        ids.push_back(adjustment.id.value);
    }
    return ids;
}

/// A state of sixteen masks with awkward floats.
DevelopState sixteenMasks() {
    DevelopState state;
    for (std::size_t i = 0; i < maximumLocalAdjustments; ++i) {
        LocalAdjustment adjustment;
        adjustment.name = i % 3 == 0 ? "" : "m" + std::to_string(i);
        adjustment.opacity = 0.1F * static_cast<float>(i) / 1.7F;
        adjustment.invert = i % 2 == 1;
        adjustment.enabled = i % 4 != 3;
        adjustment.deltas.exposure = 0.1F * static_cast<float>(i);
        adjustment.deltas.texture = -0.3F * static_cast<float>(i);
        if (i % 2 == 0) {
            adjustment.shape = LinearMask{.from = {0.1F + 0.01F * static_cast<float>(i), 0.3F},
                                          .to = {0.7F, 1.0F / 3.0F + 0.5F}};
        } else {
            adjustment.shape = RadialMask{.centre = {0.1F * static_cast<float>(i), 0.37F},
                                          .radiusX = 0.123456789F,
                                          .radiusY = 0.2F,
                                          .angle = -179.99F + static_cast<float>(i),
                                          .feather = 0.01F * static_cast<float>(i)};
        }
        state = withLocalAdjustmentAdded(state, adjustment);
    }
    return state;
}

} // namespace

TEST_CASE("A state document is the settings document plus the list", "[local][json]") {
    DevelopState state;
    state.nextLocalAdjustmentId = LocalAdjustmentId{4};
    LocalAdjustment mask;
    mask.id = LocalAdjustmentId{3};
    mask.shape = RadialMask{
        .centre = {0.5F, 0.4F}, .radiusX = 0.2F, .radiusY = 0.1F, .angle = 30.0F, .feather = 0.5F};
    mask.deltas.exposure = 0.5F;
    mask.deltas.dehaze = 20.0F;
    state.localAdjustments = {mask};
    state.settings.tone.exposure = 0.25F;

    const std::string text = stateToJson(state);
    CHECK_THAT(text,
               ContainsSubstring(R"("localAdjustments": {"version": 1, "nextId": 4, "masks": [)"));
    CHECK_THAT(text,
               ContainsSubstring(
                   R"({"id": 3, "type": "radial", "name": "", "enabled": true, "opacity": 1, )"
                   R"("invert": false, "geometry": {"centre": [0.5, 0.4], "radius": [0.2, 0.1], )"
                   R"("angle": 30, "feather": 0.5}, "deltas": {"exposure": 0.5, "dehaze": 20}})"));
    CHECK_THAT(text, ContainsSubstring(R"("exposure": 0.25)"));
    // The settings document is the same text up to the closing of its object.
    const std::string settingsOnly = settingsToJson(state.settings);
    CHECK(text.starts_with(settingsOnly.substr(0, settingsOnly.size() - 3)));
    CHECK_THAT(settingsToJson(state.settings), !ContainsSubstring("localAdjustments"));

    CHECK(localAdjustmentsToJson(state) ==
          R"({"version": 1, "nextId": 4, "masks": [{"id": 3, "type": "radial", "name": "", )"
          R"("enabled": true, "opacity": 1, "invert": false, "geometry": {"centre": [0.5, 0.4], )"
          R"("radius": [0.2, 0.1], "angle": 30, "feather": 0.5}, "deltas": {"exposure": 0.5, )"
          R"("dehaze": 20}}]})");
}

TEST_CASE("A linear mask writes its ends, and an empty list is still written", "[local][json]") {
    const DevelopState one = withLocalAdjustmentAdded(
        DevelopState{}, LinearMask{.from = {0.25F, 0.125F}, .to = {0.75F, 0.875F}});
    CHECK_THAT(stateToJson(one),
               ContainsSubstring(R"("geometry": {"from": [0.25, 0.125], "to": [0.75, 0.875]}, )"
                                 R"("deltas": {}})"));
    CHECK(localAdjustmentsToJson(DevelopState{}) == R"({"version": 1, "nextId": 1, "masks": []})");
    CHECK_THAT(
        stateToJson(DevelopState{}),
        ContainsSubstring(R"("localAdjustments": {"version": 1, "nextId": 1, "masks": []})"));
}

TEST_CASE("A state that is not valid is not written", "[local][json]") {
    DevelopState state = test::stateWithMasks();
    state.localAdjustments[0].opacity = 3.0F;
    CHECK_THROWS_AS(stateToJson(state), std::invalid_argument);
    CHECK_THROWS_AS(localAdjustmentsToJson(state), std::invalid_argument);
}

TEST_CASE("Every kind of mask survives a round trip through JSON", "[local][json]") {
    const std::vector<DevelopState> states = {
        DevelopState{},
        test::stateWithMasks(),
        test::stateWithEveryDelta(),
        sixteenMasks(),
        // An empty list with a raised counter.
        withLocalAdjustmentRemoved(withLocalAdjustmentAdded(DevelopState{}, LinearMask{}), {1}),
    };
    for (const DevelopState& state : states) {
        const std::string text = stateToJson(state);
        INFO(text);
        CHECK(applyStateJson(text, DevelopState{}) == state);
        // Written onto any base, the state comes back whole: the list replaces the base's.
        CHECK(applyStateJson(text, test::stateWithEveryDelta()) == state);
        CollectedDiagnostics log;
        (void)applyStateJson(text, DevelopState{}, log);
        CHECK(log.entries().empty());
    }
    const DevelopState counted = states.back();
    REQUIRE(counted.localAdjustments.empty());
    CHECK(counted.nextLocalAdjustmentId == LocalAdjustmentId{2});
}

TEST_CASE("Names, flags and awkward numbers survive", "[local][json]") {
    DevelopState state = test::stateWithMasks();
    state = withLocalAdjustmentRenamed(state, state.localAdjustments[2].id,
                                       "Zon\xC3\xA9 \\ \"quoted\" \xF0\x9F\x8C\x85 </tag>");
    state = withLocalDelta(state, state.localAdjustments[2].id, "exposure", 0.1);
    state = withLocalOpacity(state, state.localAdjustments[2].id, 0.3F);
    const DevelopState back = applyStateJson(stateToJson(state), DevelopState{});
    CHECK(back == state);
    CHECK(back.localAdjustments[0].enabled == false);
    CHECK(back.localAdjustments[1].invert == true);
    CHECK(back.localAdjustments[2].deltas.exposure == 0.1F);
    CHECK(back.localAdjustments[2].opacity == 0.3F);
}

TEST_CASE("A document without the list leaves the base's masks and counter", "[local][json]") {
    const DevelopState base = test::stateWithMasks();
    const auto [state, log] = applyDoc(R"({"arraw": 1, "settings": {"exposure": 1.5}})", base);
    CHECK(state.localAdjustments == base.localAdjustments);
    CHECK(state.nextLocalAdjustmentId == base.nextLocalAdjustmentId);
    CHECK(state.settings.tone.exposure == 1.5F);
    CHECK(log.entries().empty());
    // The settings-only document of a state is the same case.
    CHECK(applyStateJson(settingsToJson(DevelopSettings{}), base).localAdjustments ==
          base.localAdjustments);
    // And the settings-only calls ignore the key altogether.
    CHECK(applySettingsJson(stateToJson(base), DevelopSettings{}) == base.settings);
}

TEST_CASE("A list that is present replaces the base's whole", "[local][json]") {
    const DevelopState base = test::stateWithMasks();
    const auto [empty, log] = applyDoc(document({}), base);
    CHECK(empty.localAdjustments.empty());
    CHECK(empty.nextLocalAdjustmentId == LocalAdjustmentId{1});
    const auto [one, log2] = applyDoc(document({linearJson(7)}), base);
    CHECK(idsOf(one) == std::vector<std::uint32_t>{7});
    CHECK(one.nextLocalAdjustmentId == LocalAdjustmentId{8});
    CHECK(one.settings == base.settings);
}

TEST_CASE("A mask is read with its defaults for what it leaves out", "[local][json]") {
    const auto [state, log] = applyDoc(document({linearJson(2)}));
    REQUIRE(state.localAdjustments.size() == 1);
    const LocalAdjustment& mask = state.localAdjustments[0];
    CHECK(mask.id == LocalAdjustmentId{2});
    CHECK(mask.name.empty());
    CHECK(mask.enabled);
    CHECK(mask.opacity == 1.0F);
    CHECK_FALSE(mask.invert);
    CHECK(mask.deltas.isZero());
    CHECK(std::get<LinearMask>(mask.shape) == LinearMask{.from = {0.5F, 0.2F}, .to = {0.5F, 0.8F}});
    CHECK(log.entries().empty());
}

TEST_CASE("An unknown mask type is dropped with a warning, the others kept", "[local][json]") {
    const std::string brush =
        R"({"id": 2, "type": "brush", "geometry": {"rasteriser": 1, "strokes": []}})";
    const auto [state, log] = applyDoc(document({linearJson(1), brush, radialJson(3)}));
    CHECK(idsOf(state) == std::vector<std::uint32_t>{1, 3});
    const Diagnostic warning = only(log, Notice::LocalAdjustmentDropped);
    CHECK(warning.severity == Severity::Warning);
    CHECK(warning.values.size() == 4);
    CHECK(std::get<double>(warning.values[0]) == 2.0);
    CHECK(std::get<std::string>(warning.values[1]) == "2");
    CHECK(std::get<std::string>(warning.values[2]) == "brush");
    CHECK_THAT(describe(warning), ContainsSubstring("mask 2"));
    CHECK_THAT(describe(warning), ContainsSubstring("brush"));
    // The dropped mask's id is not handed out again by the counter it leaves: only kept ids count.
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{4});
}

TEST_CASE("Malformed entries are dropped with a warning naming them", "[local][json]") {
    const std::vector<std::string> bad = {
        "5",                                                                 // not an object
        R"({"type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})", // no id
        R"({"id": 0, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": -3, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 1.5, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": "4", "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 5000000000, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 6, "geometry": {"from": [0, 0], "to": [1, 1]}})", // no type
        R"({"id": 7, "type": 3, "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 8, "type": "linear"})", // no geometry
        R"({"id": 9, "type": "linear", "geometry": 4})",
        R"({"id": 10, "type": "linear", "geometry": {"from": [0, 0]}})", // no end
        R"({"id": 11, "type": "linear", "geometry": {"from": [0], "to": [1, 1]}})",
        R"({"id": 12, "type": "linear", "geometry": {"from": [0, "x"], "to": [1, 1]}})",
        R"({"id": 13, "type": "radial", "geometry": {"centre": [0, 0], "radius": [1, 1]}})",
        R"({"id": 14, "type": "linear", "enabled": "yes", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 15, "type": "linear", "name": 4, "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 16, "type": "linear", "opacity": "half", "geometry": {"from": [0, 0], "to": [1, 1]}})",
        R"({"id": 17, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}, "deltas": 4})",
        R"({"id": 18, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}, "deltas": {"exposure": "lots"}})",
        R"({"id": 19, "type": "linear", "geometry": {"from": [0, 0], "to": [1, 1]}, "deltas": {"exposure": null}})",
    };
    for (std::size_t i = 0; i < bad.size(); ++i) {
        INFO(bad[i]);
        const auto [state, log] = applyDoc(document({linearJson(1), bad[i]}));
        CHECK(idsOf(state) == std::vector<std::uint32_t>{1});
        const Diagnostic warning = only(log, Notice::LocalAdjustmentDropped);
        CHECK(std::get<double>(warning.values[0]) == 2.0);
        CHECK_FALSE(std::get<std::string>(warning.values[3]).empty());
    }
}

TEST_CASE("Degenerate geometry is dropped", "[local][json]") {
    const std::vector<std::string> bad = {
        linearJson(2, R"("from": [0.5, 0.5], "to": [0.5, 0.5])"),
        linearJson(2, R"("from": [0.5, 0.5], "to": [0.5, 0.5005])"),
        // Clamped onto one point, it is one point.
        linearJson(2, R"("from": [4, 0], "to": [9, 0])"),
        radialJson(2, R"("centre": [0.5, 0.5], "radius": [0, 0.2], "angle": 0, "feather": 0.5)"),
        radialJson(2, R"("centre": [0.5, 0.5], "radius": [0.2, 0.0005], "angle": 0, "feather": 0)"),
        radialJson(2, R"("centre": [0.5, 0.5], "radius": [-0.2, 0.2], "angle": 0, "feather": 0)"),
    };
    for (const std::string& mask : bad) {
        INFO(mask);
        const auto [state, log] = applyDoc(document({linearJson(1), mask}));
        CHECK(idsOf(state) == std::vector<std::uint32_t>{1});
        CHECK(count(log, Notice::LocalAdjustmentDropped) == 1);
    }
    // Just enough is kept.
    const auto [state, log] = applyDoc(
        document({linearJson(1, R"("from": [0.5, 0.5], "to": [0.5, 0.5015])"),
                  radialJson(2, R"("centre": [0.5, 0.5], "radius": [0.001, 0.001], "angle": 0,)"
                                R"( "feather": 0)")}));
    CHECK(idsOf(state) == std::vector<std::uint32_t>{1, 2});
    CHECK(log.entries().empty());
}

TEST_CASE("A duplicate id drops the later mask", "[local][json]") {
    const auto [state, log] = applyDoc(document({linearJson(4), radialJson(5), radialJson(4)}));
    CHECK(idsOf(state) == std::vector<std::uint32_t>{4, 5});
    CHECK(std::holds_alternative<LinearMask>(state.localAdjustments[0].shape));
    const Diagnostic warning = only(log, Notice::LocalAdjustmentDropped);
    CHECK(std::get<double>(warning.values[0]) == 3.0);
    CHECK_THAT(describe(warning), ContainsSubstring("used"));
}

TEST_CASE("Entries past sixteen are dropped, each with a warning", "[local][json]") {
    std::vector<std::string> masks;
    for (int id = 1; id <= 19; ++id) {
        masks.push_back(id % 2 == 0 ? radialJson(id) : linearJson(id));
    }
    const auto [state, log] = applyDoc(document(masks));
    CHECK(state.localAdjustments.size() == 16);
    CHECK(idsOf(state).back() == 16);
    CHECK(count(log, Notice::LocalAdjustmentDropped) == 3);
    CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{17});
    CHECK_NOTHROW(validate(state));
    // Exactly sixteen is fine.
    masks.resize(16);
    CHECK(count(applyDoc(document(masks)).log, Notice::LocalAdjustmentDropped) == 0);
}

TEST_CASE("Unknown fields are ignored with a warning and the mask stays", "[local][json]") {
    const std::string mask =
        R"({"id": 2, "type": "linear", "colour": "red", "geometry": {"from": [0.5, 0.2],)"
        R"( "to": [0.5, 0.8], "centreX": 1, "twist": 2}, "deltas": {"exposure": 0.5,)"
        R"( "filmicHighlights": 10, "sparkle": 3}})";
    const auto [state, log] = applyDoc(document({mask}));
    REQUIRE(state.localAdjustments.size() == 1);
    CHECK(state.localAdjustments[0].deltas.exposure == 0.5F);
    CHECK(count(log, Notice::LocalAdjustmentFieldIgnored) == 5);
    CHECK(count(log, Notice::LocalAdjustmentDropped) == 0);
    std::vector<std::string> keys;
    for (const Diagnostic& entry : log.entries()) {
        REQUIRE(entry.notice == Notice::LocalAdjustmentFieldIgnored);
        keys.push_back(std::get<std::string>(entry.values[2]));
    }
    std::ranges::sort(keys);
    CHECK(keys ==
          std::vector<std::string>{"centreX", "colour", "filmicHighlights", "sparkle", "twist"});
}

TEST_CASE("Numbers out of range are clamped with a warning", "[local][json]") {
    const std::string mask =
        R"({"id": 2, "type": "radial", "opacity": 2.5, "geometry": {"centre": [9, -9],)"
        R"( "radius": [9, 0.2], "angle": 190, "feather": -1}, "deltas": {"exposure": 9,)"
        R"( "dehaze": -500, "vibrance": 100}})";
    const auto [state, log] = applyDoc(document({mask}));
    REQUIRE(state.localAdjustments.size() == 1);
    const LocalAdjustment& adjustment = state.localAdjustments[0];
    CHECK(adjustment.opacity == 1.0F);
    CHECK(adjustment.deltas.exposure == 4.0F);
    CHECK(adjustment.deltas.dehaze == -100.0F);
    CHECK(adjustment.deltas.vibrance == 100.0F);
    const auto& radial = std::get<RadialMask>(adjustment.shape);
    CHECK(radial.centre == CorrectedPoint{3.0F, -2.0F});
    CHECK(radial.radiusX == 4.0F);
    CHECK(radial.angle == -170.0F);
    CHECK(radial.feather == 0.0F);
    // opacity, centreX, centreY, radiusX, angle, feather, exposure, dehaze: not vibrance.
    CHECK(count(log, Notice::SettingClamped) == 8);
    CHECK_NOTHROW(validate(state));
    bool named = false;
    for (const Diagnostic& entry : log.entries()) {
        named = named || std::get<std::string>(entry.values[0]) == "localAdjustments[1].opacity";
    }
    CHECK(named);
}

TEST_CASE("An angle is wrapped, not clamped, and the half-turn becomes minus 180",
          "[local][json]") {
    const auto angleOf = [](const std::string& angle) {
        const auto [state, log] = applyDoc(
            document({radialJson(1, R"("centre": [0.5, 0.5], "radius": [0.2, 0.2], "angle": )" +
                                        angle + R"(, "feather": 0.5)")}));
        REQUIRE(state.localAdjustments.size() == 1);
        return std::get<RadialMask>(state.localAdjustments[0].shape).angle;
    };
    CHECK(angleOf("180") == -180.0F);
    CHECK(angleOf("-180") == -180.0F);
    CHECK(angleOf("179.5") == 179.5F);
    CHECK(angleOf("540") == -180.0F);
    CHECK(angleOf("-181") == 179.0F);
    CHECK(angleOf("1000000") == -80.0F);
}

TEST_CASE("The counter is repaired to sit above every id", "[local][json]") {
    const auto nextOf = [](const std::string& extra) {
        return applyDoc(document({linearJson(2), linearJson(5)}, extra))
            .state.nextLocalAdjustmentId;
    };
    CHECK(nextOf("") == LocalAdjustmentId{6});
    CHECK(nextOf(R"("nextId": 3, )") == LocalAdjustmentId{6});
    CHECK(nextOf(R"("nextId": 6, )") == LocalAdjustmentId{6});
    CHECK(nextOf(R"("nextId": 40, )") == LocalAdjustmentId{40});
    CHECK(nextOf(R"("nextId": 99999999999, )") ==
          LocalAdjustmentId{std::numeric_limits<std::uint32_t>::max()});

    for (const char* bad : {R"("nextId": "7", )", R"("nextId": 0, )", R"("nextId": 2.5, )",
                            R"("nextId": -4, )", R"("nextId": null, )"}) {
        INFO(bad);
        const auto [state, log] = applyDoc(document({linearJson(2), linearJson(5)}, bad));
        CHECK(state.nextLocalAdjustmentId == LocalAdjustmentId{6});
        CHECK(count(log, Notice::SettingMalformed) == 1);
        CHECK(state.localAdjustments.size() == 2);
    }
    // An empty list keeps a stored counter, and starts at 1 without one.
    CHECK(applyDoc(document({}, R"("nextId": 9, )")).state.nextLocalAdjustmentId ==
          LocalAdjustmentId{9});
    CHECK(applyDoc(document({})).state.nextLocalAdjustmentId == LocalAdjustmentId{1});
}

TEST_CASE("A newer list version is read as far as it is understood", "[local][json]") {
    const auto [state, log] =
        applyDoc(document({linearJson(1)}, R"("version": 2, "futureThing": 1, )"));
    CHECK(state.localAdjustments.size() == 1);
    const Diagnostic warning = only(log, Notice::NewerLocalAdjustmentsVersion);
    CHECK(std::get<double>(warning.values[0]) == 2.0);
    CHECK(std::get<double>(warning.values[1]) == 1.0);
    CHECK_THAT(describe(warning), ContainsSubstring("version 2"));

    for (const char* bad : {R"("version": "one", )", R"("version": 0, )", R"("version": 1.5, )"}) {
        INFO(bad);
        const auto [again, badLog] = applyDoc(document({linearJson(1)}, bad));
        CHECK(again.localAdjustments.size() == 1);
        CHECK(count(badLog, Notice::SettingMalformed) == 1);
    }
    CHECK(count(applyDoc(document({linearJson(1)}, R"("version": 1, )")).log,
                Notice::SettingMalformed) == 0);
}

TEST_CASE("A list that cannot be read at all leaves the base's alone", "[local][json]") {
    const DevelopState base = test::stateWithMasks();
    for (const char* text :
         {R"({"arraw": 1, "settings": {}, "localAdjustments": 5})",
          R"({"arraw": 1, "settings": {}, "localAdjustments": []})",
          R"({"arraw": 1, "settings": {}, "localAdjustments": {"masks": {}}})",
          R"({"arraw": 1, "settings": {}, "localAdjustments": {"version": 1}})"}) {
        INFO(text);
        const auto [state, log] = applyDoc(text, base);
        CHECK(state.localAdjustments == base.localAdjustments);
        CHECK(state.nextLocalAdjustmentId == base.nextLocalAdjustmentId);
        CHECK(count(log, Notice::SettingMalformed) == 1);
    }
}

TEST_CASE("Control characters in a name are removed", "[local][json]") {
    const auto [state, log] = applyDoc(document(
        {R"({"id": 1, "type": "linear", "name": "a\u0001b\nc\td", "geometry": {"from": [0, 0],)"
         R"( "to": [1, 1]}})"}));
    REQUIRE(state.localAdjustments.size() == 1);
    CHECK(state.localAdjustments[0].name == "abcd");
    CHECK_NOTHROW(validate(state));
}

TEST_CASE("Characters XML cannot hold are removed from a name, and refused in an edit",
          "[local][json]") {
    const auto [state, log] = applyDoc(document(
        {R"({"id": 1, "type": "linear", "name": "a\ufffeb\uffffc\u007fd\u00e9", "geometry": )"
         R"({"from": [0, 0], "to": [1, 1]}})"}));
    REQUIRE(state.localAdjustments.size() == 1);
    CHECK(state.localAdjustments[0].name == "abcd\xC3\xA9");
    CHECK_NOTHROW(validate(state));

    const DevelopState one = withLocalAdjustmentAdded(DevelopState{}, LinearMask{});
    const LocalAdjustmentId id = one.localAdjustments[0].id;
    for (const char* bad : {"x\xEF\xBF\xBEy", "x\xEF\xBF\xBFy", "x\xFFy", "x\xC3", "x\xC0\x80y",
                            "x\xED\xA0\x80y", "x\xF4\x90\x80\x80y", "x\ty"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(withLocalAdjustmentRenamed(one, id, bad), std::invalid_argument);
        DevelopState direct = one;
        direct.localAdjustments[0].name = bad;
        CHECK_THROWS_AS(validate(direct), std::invalid_argument);
        CHECK_THROWS_AS(stateToJson(direct), std::invalid_argument);
    }
    for (const char* good :
         {"\xEF\xBF\xBD", "\xF0\x9F\x8C\x85", "\xEE\x80\x80", "\xF4\x8F\xBF\xBF"}) {
        CAPTURE(good);
        CHECK(withLocalAdjustmentRenamed(one, id, good).localAdjustments[0].name == good);
    }
    CHECK(storableMaskName("a\xFF"
                           "b\xEF\xBF\xBE"
                           "c\xC3") == "abc");
}

TEST_CASE("A document that is not a state document is refused as a settings document is",
          "[local][json]") {
    CHECK_THROWS_AS(applyStateJson("nope", DevelopState{}), std::invalid_argument);
    CHECK_THROWS_AS(applyStateJson(R"({"arraw": 1})", DevelopState{}), std::invalid_argument);
    CHECK_THROWS_AS(applyStateJson(R"({"settings": {}})", DevelopState{}), std::invalid_argument);
}
