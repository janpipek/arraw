#include "DevelopGroup.h"
#include "DevelopPreset.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

GroupSelection groups(std::initializer_list<DevelopGroup> gs) {
    GroupSelection s;
    for (DevelopGroup g : gs)
        s.set(static_cast<size_t>(g));
    return s;
}

// A preset carrying Tone + Colour, with values only in those groups (the
// invariant: untouched groups stay at default).
DevelopPreset samplePreset() {
    DevelopPreset p;
    p.name = "Punchy";
    p.groups = groups({DevelopGroup::Tone, DevelopGroup::Colour});
    p.values.exposure = 0.5f;
    p.values.contrast = 25.0f;
    p.values.highlights = -40.0f;
    p.values.shadows = 30.0f;
    p.values.whites = 10.0f;
    p.values.blacks = -10.0f;
    p.values.saturation = 15.0f;
    p.values.vibrance = 20.0f;
    return p;
}

} // namespace

TEST_CASE("A Geometry preset round-trips orientation through JSON", "[preset]") {
    DevelopPreset p;
    p.name = "Turned";
    p.groups = groups({DevelopGroup::Geometry});
    p.values.orientation = orient::Orientation{1, true};
    p.values.rotation = 3.0f;

    bool ok = false;
    const DevelopPreset loaded = parseDevelopPreset(serializeDevelopPreset(p), &ok);

    REQUIRE(ok);
    CHECK(loaded.values.orientation == p.values.orientation);
}

TEST_CASE("Develop preset round-trips through JSON", "[preset]") {
    const DevelopPreset p = samplePreset();

    bool ok = false;
    const DevelopPreset loaded = parseDevelopPreset(serializeDevelopPreset(p), &ok);

    REQUIRE(ok);
    CHECK(loaded.name == p.name);
    CHECK(loaded.groups == p.groups);
    // Compare what actually matters: the fields the active groups carry.
    CHECK(
        applyGroups(GlobalAdjustment{}, loaded.values, loaded.groups)
        == applyGroups(GlobalAdjustment{}, p.values, p.groups));
}

TEST_CASE("Serialisation is partial: only active groups appear in the file", "[preset]") {
    const QByteArray json = serializeDevelopPreset(samplePreset());

    const QJsonObject root = QJsonDocument::fromJson(json).object();
    REQUIRE(root.contains("groups"));
    const QJsonObject groupsObj = root["groups"].toObject();

    // Tone + Colour were chosen; nothing else is written.
    CHECK(groupsObj.contains("tone"));
    CHECK(groupsObj.contains("colour"));
    CHECK_FALSE(groupsObj.contains("whiteBalance"));
    CHECK_FALSE(groupsObj.contains("geometry"));
    CHECK(groupsObj.size() == 2);
}

TEST_CASE("A group absent from the file loads as inactive", "[preset]") {
    // Only whiteBalance present — every other group must come back unset.
    const QByteArray json
        = R"({"name":"WB only","groups":{"whiteBalance":{"temperature":7200,"tint":-5}}})";

    bool ok = false;
    const DevelopPreset p = parseDevelopPreset(json, &ok);

    REQUIRE(ok);
    CHECK(hasGroup(p.groups, DevelopGroup::WhiteBalance));
    CHECK_FALSE(hasGroup(p.groups, DevelopGroup::Tone));
    CHECK(p.groups.count() == 1);
    CHECK(p.values.temperature == 7200.0f);
    CHECK(p.values.tint == -5.0f);
}

TEST_CASE("Unknown keys are tolerated (forward compatibility)", "[preset]") {
    const QByteArray json
        = R"({"name":"X","futureField":42,"groups":{"tone":{"exposure":1.0,"newKnob":9}}})";

    bool ok = false;
    const DevelopPreset p = parseDevelopPreset(json, &ok);

    REQUIRE(ok);
    CHECK(hasGroup(p.groups, DevelopGroup::Tone));
    CHECK(p.values.exposure == 1.0f);
}

TEST_CASE("Malformed JSON fails cleanly", "[preset]") {
    bool ok = true;
    const DevelopPreset p = parseDevelopPreset("not json at all", &ok);
    CHECK_FALSE(ok);
    CHECK(p.groups.none());
}

TEST_CASE("Known-good fixture parses to the expected preset", "[preset]") {
    QFile f(QString(ARRAW_FIXTURE_DIR) + "/preset_punchy.json");
    REQUIRE(f.open(QIODevice::ReadOnly));

    bool ok = false;
    const DevelopPreset p = parseDevelopPreset(f.readAll(), &ok);

    REQUIRE(ok);
    CHECK(p.name == "Punchy");
    CHECK(p.groups == groups({DevelopGroup::Tone, DevelopGroup::Colour}));
    CHECK(p.values.exposure == 0.5f);
    CHECK(p.values.contrast == 25.0f);
    CHECK(p.values.saturation == 15.0f);
    CHECK(p.values.vibrance == 20.0f);
}
