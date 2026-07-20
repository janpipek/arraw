#include "cli/PresetCommand.h"
#include "develop/DevelopGroup.h"
#include <catch2/catch_test_macros.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

GroupSelection groups(std::initializer_list<DevelopGroup> gs) {
    GroupSelection s;
    for (DevelopGroup g : gs)
        s.set(static_cast<size_t>(g));
    return s;
}

DevelopPreset preset(const QString& name, std::initializer_list<DevelopGroup> gs) {
    DevelopPreset p;
    p.name = name;
    p.groups = groups(gs);
    return p;
}

} // namespace

TEST_CASE("list table on an empty store says so and exits 0") {
    const PresetStore store(QTemporaryDir().path());
    QString outText;
    QTextStream out(&outText);

    REQUIRE(cli::runPresetList(store, false, out) == 0);
    REQUIRE(outText == "No presets saved.\n");
}

TEST_CASE("list --json on an empty store is an empty array") {
    const PresetStore store(QTemporaryDir().path());
    QString outText;
    QTextStream out(&outText);

    REQUIRE(cli::runPresetList(store, true, out) == 0);
    REQUIRE(outText.trimmed() == "[]");
}

TEST_CASE("list table shows each preset's name and group labels") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy BW", {DevelopGroup::Tone, DevelopGroup::BlackAndWhite}));

    QString outText;
    QTextStream out(&outText);
    REQUIRE(cli::runPresetList(store, false, out) == 0);

    REQUIRE(outText.contains("Punchy BW"));
    REQUIRE(outText.contains("Tone, Black & White"));
}

TEST_CASE("list --json emits name and stable group keys, not labels") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy BW", {DevelopGroup::Tone, DevelopGroup::BlackAndWhite}));

    QString outText;
    QTextStream out(&outText);
    REQUIRE(cli::runPresetList(store, true, out) == 0);

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(outText.toUtf8(), &err);
    REQUIRE(err.error == QJsonParseError::NoError);
    REQUIRE(doc.isArray());
    const QJsonArray arr = doc.array();
    REQUIRE(arr.size() == 1);
    const QJsonObject o = arr.at(0).toObject();
    CHECK(o["name"].toString() == "Punchy BW");
    const QJsonArray groupsJson = o["groups"].toArray();
    CHECK(groupsJson.size() == 2);
    CHECK(groupsJson.contains(QJsonValue("tone")));
    CHECK(groupsJson.contains(QJsonValue("blackAndWhite")));
}

TEST_CASE("list sorts presets case-insensitively, same as the store") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("zebra", {DevelopGroup::Tone}));
    store.save(preset("Apple", {DevelopGroup::Tone}));

    QString outText;
    QTextStream out(&outText);
    REQUIRE(cli::runPresetList(store, false, out) == 0);

    REQUIRE(outText.indexOf("Apple") < outText.indexOf("zebra"));
}
