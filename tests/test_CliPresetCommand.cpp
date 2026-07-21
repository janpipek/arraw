#include "cli/PresetCommand.h"
#include "develop/DevelopGroup.h"
#include "develop/DevelopPreset.h"
#include "io/XmpSidecar.h"
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

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

// An empty file with a supported extension: apply's preflight and
// XmpSidecar's sidecar-path derivation only look at the path, never decode
// pixels, so no real image content is needed.
QString touchImage(const QString& dir, const QString& fileName) {
    const QString path = QDir(dir).filePath(fileName);
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    return path;
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

TEST_CASE("show table marks an all-default active group as resetting to defaults") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Neutral", {DevelopGroup::Tone})); // default GlobalAdjustment: no change

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "Neutral", false, out, err) == 0);

    REQUIRE(outText.contains("Neutral"));
    REQUIRE(outText.contains("Tone"));
    REQUIRE(outText.contains("(resets to defaults)"));
}

TEST_CASE("show table lists each changed field for a group") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    DevelopPreset p = preset("Punchy", {DevelopGroup::Tone});
    p.values.exposure = 0.5f;
    store.save(p);

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "Punchy", false, out, err) == 0);

    // Reuse the same describer the command calls, rather than hardcoding its
    // label/formatting conventions here.
    for (const QString& line : describeGroupNonDefaults(DevelopGroup::Tone, p.values))
        REQUIRE(outText.contains(line));
    REQUIRE_FALSE(outText.contains("(resets to defaults)"));
}

TEST_CASE("show --json emits the preset's native, round-trippable JSON") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    DevelopPreset p = preset("Punchy", {DevelopGroup::Tone, DevelopGroup::BlackAndWhite});
    p.values.exposure = 0.5f;
    p.values.convertToGrayscale = true;
    store.save(p);

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "Punchy", true, out, err) == 0);

    bool ok = false;
    const DevelopPreset roundTripped = parseDevelopPreset(outText.toUtf8(), &ok);
    REQUIRE(ok);
    CHECK(roundTripped.name == "Punchy");
    CHECK(roundTripped.groups == p.groups);
    CHECK(roundTripped.values.exposure == 0.5f);
    CHECK(roundTripped.values.convertToGrayscale);
}

TEST_CASE("show matches an existing preset's name case-insensitively") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "punchy", false, out, err) == 0);
    REQUIRE(outText.contains("Punchy")); // the stored display name, not the lookup casing
}

TEST_CASE("show on an unknown name lists the available presets on stderr and exits 2") {
    QTemporaryDir dir;
    const PresetStore store(dir.path());
    store.save(preset("Alpha", {DevelopGroup::Tone}));
    store.save(preset("Beta", {DevelopGroup::Tone}));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "Ghost", false, out, err) == 2);

    REQUIRE(outText.isEmpty());
    REQUIRE(errText.contains("Ghost"));
    REQUIRE(errText.contains("Alpha"));
    REQUIRE(errText.contains("Beta"));
}

TEST_CASE("show on an empty store says so and exits 2") {
    const PresetStore store(QTemporaryDir().path());

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetShow(store, "Anything", false, out, err) == 2);
    REQUIRE(errText.contains("no presets saved"));
}

TEST_CASE("apply writes each file's sidecar with the preset's groups") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    DevelopPreset p = preset("Punchy", {DevelopGroup::Tone});
    p.values.exposure = 0.5f;
    store.save(p);

    QTemporaryDir imagesDir;
    const QString a = touchImage(imagesDir.path(), "a.arw");
    const QString b = touchImage(imagesDir.path(), "b.jpg");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {a, b}, false, out, err) == 0);

    CHECK(XmpSidecar::loadAdjustments(a).exposure == 0.5f);
    CHECK(XmpSidecar::loadAdjustments(b).exposure == 0.5f);
    CHECK(outText.contains("Applied: " + a));
    CHECK(outText.contains("Applied: " + b));
    CHECK(outText.contains("2 applied, 0 failed"));
    CHECK(errText.isEmpty());
}

TEST_CASE("apply preserves the target's other groups, replacing only the preset's") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    DevelopPreset p = preset("Punchy", {DevelopGroup::Tone});
    p.values.exposure = 0.5f;
    store.save(p);

    QTemporaryDir imagesDir;
    const QString a = touchImage(imagesDir.path(), "a.arw");
    GlobalAdjustment existing;
    existing.saturation = 42.0f; // Colour group: the preset never touches this
    REQUIRE(XmpSidecar::saveAdjustments(a, existing));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {a}, false, out, err) == 0);

    const GlobalAdjustment after = XmpSidecar::loadAdjustments(a);
    CHECK(after.exposure == 0.5f);    // overwritten by the preset's Tone group
    CHECK(after.saturation == 42.0f); // untouched: Colour wasn't in the preset
}

TEST_CASE("apply --json emits preset, applied, and failed") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QTemporaryDir imagesDir;
    const QString a = touchImage(imagesDir.path(), "a.arw");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {a}, true, out, err) == 0);

    QJsonParseError jsonErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(outText.toUtf8(), &jsonErr);
    REQUIRE(jsonErr.error == QJsonParseError::NoError);
    const QJsonObject root = doc.object();
    CHECK(root["preset"].toString() == "Punchy");
    CHECK(root["applied"].toArray() == QJsonArray{a});
    CHECK(root["failed"].toArray().isEmpty());
}

TEST_CASE("apply on an unknown preset name touches nothing and exits 2") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Alpha", {DevelopGroup::Tone}));

    QTemporaryDir imagesDir;
    const QString a = touchImage(imagesDir.path(), "a.arw");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Ghost", {a}, false, out, err) == 2);

    REQUIRE(errText.contains("Ghost"));
    REQUIRE(errText.contains("Alpha"));
    REQUIRE_FALSE(QFile::exists(XmpSidecar::pathFor(a))); // nothing was written
}

TEST_CASE("apply refuses the whole run on a missing path, writing nothing") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QTemporaryDir imagesDir;
    const QString good = touchImage(imagesDir.path(), "good.arw");
    const QString missing = QDir(imagesDir.path()).filePath("missing.arw");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {good, missing}, false, out, err) == 2);

    REQUIRE(errText.contains("missing.arw"));
    REQUIRE_FALSE(QFile::exists(XmpSidecar::pathFor(good))); // preflight ran before any write
}

TEST_CASE("apply refuses a directory path") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QTemporaryDir imagesDir;
    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {imagesDir.path()}, false, out, err) == 2);
    REQUIRE(errText.contains("directory"));
}

TEST_CASE("apply refuses an unsupported extension") {
    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QTemporaryDir imagesDir;
    const QString notes = touchImage(imagesDir.path(), "notes.txt");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runPresetApply(store, "Punchy", {notes}, false, out, err) == 2);
    REQUIRE(errText.contains("notes.txt"));
}

TEST_CASE("apply continues past a per-file write failure and exits 1") {
#ifdef Q_OS_WIN
    SKIP("read-only directory does not reliably block sidecar writes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root bypasses directory write permissions");

    QTemporaryDir storeDir;
    const PresetStore store(storeDir.path());
    store.save(preset("Punchy", {DevelopGroup::Tone}));

    QTemporaryDir lockedDir;
    const QString locked = touchImage(lockedDir.path(), "locked.arw");
    QTemporaryDir writableDir;
    const QString writable = touchImage(writableDir.path(), "writable.arw");

    REQUIRE(QFile::setPermissions(lockedDir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    const int code = cli::runPresetApply(store, "Punchy", {locked, writable}, false, out, err);
    QFile::setPermissions(
        lockedDir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    CHECK(code == 1);
    CHECK(outText.contains("Applied: " + writable));
    CHECK(outText.contains("1 applied, 1 failed"));
    CHECK(errText.contains("locked.arw"));
#endif
}
