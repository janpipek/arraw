#include "develop/DevelopGroup.h"
#include "io/PresetStore.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <QTemporaryDir>

namespace {

GroupSelection groups(std::initializer_list<DevelopGroup> gs) {
    GroupSelection s;
    for (DevelopGroup g : gs)
        s.set(static_cast<size_t>(g));
    return s;
}

DevelopPreset preset(const QString& name) {
    DevelopPreset p;
    p.name = name;
    p.groups = groups({DevelopGroup::Tone});
    p.values.exposure = 0.5f;
    return p;
}

} // namespace

TEST_CASE("presetFileName sanitises unsafe characters", "[presetstore]") {
    CHECK(presetFileName("Punchy") == "Punchy.json");
    CHECK(presetFileName("a/b") == "a_b.json");
    CHECK(presetFileName("Cool: Tone") == "Cool_ Tone.json");
}

TEST_CASE("save then loadAll round-trips a preset", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());

    REQUIRE(store.save(preset("Punchy")));

    const auto loaded = store.loadAll();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "Punchy");
    CHECK(hasGroup(loaded[0].groups, DevelopGroup::Tone));
    CHECK(loaded[0].values.exposure == 0.5f);
}

TEST_CASE("loadAll returns presets sorted case-insensitively by name", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("zebra"));
    store.save(preset("Apple"));
    store.save(preset("mango"));

    const auto loaded = store.loadAll();
    REQUIRE(loaded.size() == 3);
    CHECK(loaded[0].name == "Apple");
    CHECK(loaded[1].name == "mango");
    CHECK(loaded[2].name == "zebra");
}

TEST_CASE("loadAll on a missing directory is empty, not fatal", "[presetstore]") {
    const PresetStore store("/no/such/arraw/presets/dir");
    CHECK(store.loadAll().empty());
}

TEST_CASE("loadAll skips malformed files but keeps the good ones", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Good"));

    QFile junk(dir.path() + "/broken.json");
    REQUIRE(junk.open(QIODevice::WriteOnly));
    junk.write("not json at all");
    junk.close();

    const auto loaded = store.loadAll();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "Good");
}

TEST_CASE("exists is false when no preset by that name is stored", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    CHECK_FALSE(store.exists("Punchy"));
}

TEST_CASE("exists is true for an exact name match", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy"));
    CHECK(store.exists("Punchy"));
}

TEST_CASE("exists is case-insensitive", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy"));
    CHECK(store.exists("punchy"));
    CHECK(store.exists("PUNCHY"));
}

TEST_CASE("exists treats a sanitisation collision as taken", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("a/b")); // sanitises to "a_b.json"
    CHECK(store.exists("a:b")); // also sanitises to "a_b.json" — a distinct name, same file
}

TEST_CASE("rename moves a preset to a new name, keeping its content", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy"));

    CHECK(store.rename("Punchy", "Vivid"));

    const auto loaded = store.loadAll();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "Vivid");
    CHECK(hasGroup(loaded[0].groups, DevelopGroup::Tone));
    CHECK(loaded[0].values.exposure == 0.5f);
}

TEST_CASE("rename returns false for a preset that doesn't exist", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    CHECK_FALSE(store.rename("Ghost", "Whatever"));
}

TEST_CASE("remove deletes the backing file", "[presetstore]") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy"));
    REQUIRE(store.loadAll().size() == 1);

    CHECK(store.remove("Punchy"));
    CHECK(store.loadAll().empty());
    CHECK_FALSE(store.remove("Punchy")); // already gone
}
