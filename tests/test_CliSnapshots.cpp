#include "Cli.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTimeZone>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// The command line's snapshot surfaces (ADR 056, decision 10): `info` lists them and
/// `export --snapshot ID` renders one.

namespace {

namespace fs = std::filesystem;

struct Invocation {
    int code = 0;
    std::string out;
    std::string err;
};

Invocation invoke(const std::vector<std::string>& arguments) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run(arguments, out, err, [](cli::ApplicationKind) {});
    return {code, out.str(), err.str()};
}

DevelopState exposed(float exposure) {
    DevelopState state;
    state.settings.tone.exposure = exposure;
    return state;
}

std::string bytesOf(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// A RAW with a sidecar of two snapshots: 1 "Warm look" (exposure +1) and 2 `Dark "quoted"`
/// (exposure -1, a child of 1); the saved look is a third state, the defaults, based on 2.
struct Rig {
    test::TempDir directory;
    fs::path photo = directory.file("IMG_1.dng");

    Rig() {
        fs::copy_file(test::fixture("linear-32x24-neutral.dng"), photo);
        EditSession session(openDocument(photo));
        session.setState(exposed(1.0F));
        session.createSnapshot("Warm look");
        session.setState(exposed(-1.0F));
        session.createSnapshot("Dark \"quoted\"");
        session.setState(DevelopState{});
        session.save();
    }

    /// Exports to a folder of its own and returns the written PNG's path, and the result.
    fs::path exportTo(const std::string& folder, std::vector<std::string> flags,
                      Invocation* result = nullptr) const {
        const fs::path out = directory.file(folder);
        fs::create_directories(out);
        std::vector<std::string> arguments{"export",   photo.string(), "-o",          out.string(),
                                           "--format", "png",          "--bit-depth", "16",
                                           "--device", "cpu",          "--quiet"};
        arguments.insert(arguments.end(), flags.begin(), flags.end());
        Invocation done = invoke(arguments);
        if (result != nullptr) {
            *result = done;
        } else {
            REQUIRE(done.code == cli::Success);
        }
        return out / "IMG_1.png";
    }
};

std::string isoTime(std::chrono::sys_seconds time) {
    return QDateTime::fromSecsSinceEpoch(time.time_since_epoch().count(), QTimeZone::UTC)
        .toString(Qt::ISODate)
        .toStdString();
}

QJsonObject firstFile(const std::string& json) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    REQUIRE(document.isObject());
    return document.object()["files"].toArray().at(0).toObject();
}

/// Inserts a snapshot entry no reader can use, beside the good ones.
void addUnusableEntry(const fs::path& photo) {
    QFile file(QString::fromStdU16String(sidecarPath(photo).u16string()));
    REQUIRE(file.open(QIODevice::ReadWrite));
    QByteArray text = file.readAll();
    const qsizetype list = text.indexOf("</arraw:snapshots>");
    REQUIRE(list > 0);
    const qsizetype seq = text.lastIndexOf("</rdf:Seq>", list);
    REQUIRE(seq > 0);
    text.insert(seq,
                "<rdf:li rdf:parseType=\"Resource\"><arraw:id>9</arraw:id>"
                "<arraw:name>Bad</arraw:name><arraw:created>2026-10-23T12:00:00Z</arraw:created>"
                "<arraw:state>{ not json</arraw:state><arraw:negative>{}</arraw:negative>"
                "</rdf:li>");
    file.seek(0);
    file.resize(0);
    file.write(text);
}

} // namespace

TEST_CASE("Info lists the snapshots and the saved base in text and JSON, in step",
          "[cli][info][snapshots]") {
    const Rig rig;
    const OpenDocument disk = openDocument(rig.photo);
    REQUIRE(disk.snapshots.entries.size() == 2);
    REQUIRE(disk.savedBase == SnapshotId{2});

    const auto text = invoke({"info", rig.photo.string()});
    REQUIRE(text.code == cli::Success);
    const auto json = invoke({"info", "--json", rig.photo.string()});
    REQUIRE(json.code == cli::Success);
    const QJsonObject file = firstFile(json.out);

    const QJsonArray listed = file["snapshots"].toArray();
    REQUIRE(listed.size() == 2);
    REQUIRE(file["savedBase"].toString() == "2");
    REQUIRE(file["unreadableSnapshots"].toInt() == 0);
    for (int i = 0; i < 2; ++i) {
        const Snapshot& snapshot = disk.snapshots.entries[static_cast<std::size_t>(i)];
        const QJsonObject entry = listed[i].toObject();
        CHECK(entry["id"].toString().toStdString() == std::to_string(snapshot.id.value));
        CHECK(entry["name"].toString().toStdString() == snapshot.name);
        CHECK(entry["created"].toString().toStdString() == isoTime(snapshot.created));
        if (snapshot.parent) {
            CHECK(entry["parent"].toString().toStdString() ==
                  std::to_string(snapshot.parent->value));
        } else {
            CHECK(entry["parent"].isNull());
        }
        // The same facts in the text.
        const std::string parent =
            snapshot.parent ? std::to_string(snapshot.parent->value) : "none";
        CHECK_THAT(text.out, ContainsSubstring(std::to_string(snapshot.id.value) + ": " +
                                               snapshot.name + " (parent " + parent + ", created " +
                                               isoTime(snapshot.created) + ")"));
    }
    CHECK_THAT(text.out, ContainsSubstring("saved base: 2"));
    std::size_t lines = 0;
    for (std::size_t at = text.out.find("(parent "); at != std::string::npos;
         at = text.out.find("(parent ", at + 1)) {
        ++lines;
    }
    CHECK(lines == static_cast<std::size_t>(listed.size()));
    CHECK_THAT(text.out, !ContainsSubstring("unreadable snapshot"));
    CHECK(listed[0].toObject()["parent"].isNull());
    CHECK(listed[1].toObject()["parent"].toString() == "1");
}

TEST_CASE("Info of snapshots with no saved base says none", "[cli][info][snapshots]") {
    const Rig rig;
    writeSidecar(openPhoto(rig.photo), std::nullopt);
    REQUIRE_FALSE(openDocument(rig.photo).savedBase);
    REQUIRE(openDocument(rig.photo).snapshots.entries.size() == 2);

    const auto text = invoke({"info", rig.photo.string()});
    REQUIRE(text.code == cli::Success);
    CHECK_THAT(text.out, ContainsSubstring("Warm look"));
    CHECK_THAT(text.out, !ContainsSubstring("saved base: 1"));
    CHECK_THAT(text.out, !ContainsSubstring("saved base: 2"));
    const QJsonObject file = firstFile(invoke({"info", "--json", rig.photo.string()}).out);
    CHECK(file["savedBase"].isNull());
    CHECK(file["snapshots"].toArray().size() == 2);
}

TEST_CASE("Info never prints thumbnail bytes", "[cli][info][snapshots]") {
    const Rig rig;
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    REQUIRE(image.save(&buffer, "JPEG"));
    std::vector<std::byte> jpeg(static_cast<std::size_t>(encoded.size()));
    std::memcpy(jpeg.data(), encoded.constData(), jpeg.size());
    REQUIRE(setSnapshotThumbnail(rig.photo, SnapshotId{1},
                                 SnapshotThumbnail{std::move(jpeg), "key-1"}, "key-1"));
    REQUIRE(openDocument(rig.photo).snapshots.find(SnapshotId{1})->thumbnail.has_value());

    for (const bool asJson : {false, true}) {
        const auto result = asJson ? invoke({"info", "--json", rig.photo.string()})
                                   : invoke({"info", rig.photo.string()});
        REQUIRE(result.code == cli::Success);
        CHECK_THAT(result.out, !ContainsSubstring("key-1"));
        CHECK_THAT(result.out, !ContainsSubstring("/9j/")); // base64 of a JPEG's start
        CHECK_THAT(result.out, !ContainsSubstring("thumbnail"));
    }
}

TEST_CASE("Info notes unreadable snapshot entries in text and JSON", "[cli][info][snapshots]") {
    const Rig rig;
    addUnusableEntry(rig.photo);

    const auto text = invoke({"info", rig.photo.string()});
    REQUIRE(text.code == cli::Success);
    CHECK_THAT(text.out, ContainsSubstring("unreadable snapshot entries: 1"));
    CHECK_THAT(text.out, ContainsSubstring("Warm look")); // the usable ones are still listed

    const auto json = invoke({"info", "--json", rig.photo.string()});
    REQUIRE(json.code == cli::Success);
    const QJsonObject file = firstFile(json.out);
    CHECK(file["unreadableSnapshots"].toInt() == 1);
    CHECK(file["snapshots"].toArray().size() == 2);
}

TEST_CASE("Info of a photograph without snapshots says so only in JSON", "[cli][info][snapshots]") {
    const test::TempDir directory;
    const fs::path photo = directory.file("plain.dng");
    fs::copy_file(test::fixture("linear-32x24-neutral.dng"), photo);

    const auto text = invoke({"info", photo.string()});
    REQUIRE(text.code == cli::Success);
    CHECK_THAT(text.out, !ContainsSubstring("snapshot"));
    CHECK_THAT(text.out, !ContainsSubstring("saved base"));

    const QJsonObject file = firstFile(invoke({"info", "--json", photo.string()}).out);
    CHECK(file["snapshots"].toArray().isEmpty());
    CHECK(file["savedBase"].isNull());
    CHECK(file["unreadableSnapshots"].toInt() == 0);
}

TEST_CASE("Info --no-sidecar shows no snapshots", "[cli][info][snapshots]") {
    const Rig rig;
    const auto text = invoke({"info", "--no-sidecar", rig.photo.string()});
    REQUIRE(text.code == cli::Success);
    CHECK_THAT(text.out, !ContainsSubstring("Warm look"));
    const QJsonObject file =
        firstFile(invoke({"info", "--json", "--no-sidecar", rig.photo.string()}).out);
    CHECK(file["snapshots"].toArray().isEmpty());
    CHECK(file["savedBase"].isNull());
}

TEST_CASE("Info escapes snapshot names in JSON", "[cli][info][snapshots]") {
    const Rig rig;
    const auto json = invoke({"info", "--json", rig.photo.string()});
    CHECK(firstFile(json.out)["snapshots"].toArray()[1].toObject()["name"].toString() ==
          "Dark \"quoted\"");
}

TEST_CASE("Export --snapshot renders the snapshot's look", "[cli][export][snapshots]") {
    const Rig rig;
    const QImage saved(QString::fromStdString(rig.exportTo("saved", {}).string()));
    const QImage warm(QString::fromStdString(rig.exportTo("warm", {"--snapshot", "1"}).string()));
    const QImage dark(QString::fromStdString(rig.exportTo("dark", {"--snapshot", "2"}).string()));
    REQUIRE_FALSE(saved.isNull());
    REQUIRE_FALSE(warm.isNull());
    CHECK(warm != dark);
    CHECK(warm != saved);
    CHECK(dark != saved);

    // A snapshot is the look an ordinary photograph with that state exports as.
    const test::TempDir other;
    const fs::path twin = other.file("IMG_1.dng");
    fs::copy_file(test::fixture("linear-32x24-neutral.dng"), twin);
    writeSidecar(openPhoto(twin).with(exposed(1.0F)));
    const fs::path out = other.file("out");
    fs::create_directories(out);
    REQUIRE(invoke({"export", twin.string(), "-o", out.string(), "--format", "png", "--bit-depth",
                    "16", "--device", "cpu", "--quiet"})
                .code == cli::Success);
    CHECK(QImage(QString::fromStdString((out / "IMG_1.png").string())) == warm);
}

TEST_CASE("Export flags apply on top of the snapshot", "[cli][export][snapshots]") {
    const Rig rig;
    const QImage plain(QString::fromStdString(rig.exportTo("plain", {"--snapshot", "1"}).string()));
    const QImage over(QString::fromStdString(
        rig.exportTo("over", {"--snapshot", "1", "--contrast", "20"}).string()));
    const QImage onSaved(
        QString::fromStdString(rig.exportTo("saved", {"--contrast", "20"}).string()));
    CHECK(over != plain);
    CHECK(over != onSaved); // the snapshot, not the saved look, is the base

    // The same as a photograph whose own recipe is snapshot 1's with contrast 20.
    const test::TempDir other;
    const fs::path twin = other.file("IMG_1.dng");
    fs::copy_file(test::fixture("linear-32x24-neutral.dng"), twin);
    DevelopState state = exposed(1.0F);
    state.settings.tone.contrast = 20.0F;
    writeSidecar(openPhoto(twin).with(state));
    const fs::path out = other.file("out");
    fs::create_directories(out);
    REQUIRE(invoke({"export", twin.string(), "-o", out.string(), "--format", "png", "--bit-depth",
                    "16", "--device", "cpu", "--quiet"})
                .code == cli::Success);
    CHECK(QImage(QString::fromStdString((out / "IMG_1.png").string())) == over);
}

TEST_CASE("Export --snapshot takes the snapshot's negative exactly", "[cli][export][snapshots]") {
    const test::TempDir directory;
    const fs::path photo = directory.file("IMG_1.dng");
    fs::copy_file(test::fixture("bayer-detail-32x24.dng"), photo);
    NegativeSettings vng = openPhoto(photo).negative();
    vng.demosaic = Demosaic::Vng;
    {
        EditSession session(openDocument(photo));
        session.setNegative(vng);
        session.createSnapshot("Vng");
        session.setNegative(NegativeSettings{});
        session.save();
    }
    const auto exportOf = [&](const std::string& folder, std::vector<std::string> flags) {
        const fs::path out = directory.file(folder);
        fs::create_directories(out);
        std::vector<std::string> arguments{"export",   photo.string(), "-o",          out.string(),
                                           "--format", "png",          "--bit-depth", "16",
                                           "--device", "cpu",          "--quiet"};
        arguments.insert(arguments.end(), flags.begin(), flags.end());
        REQUIRE(invoke(arguments).code == cli::Success);
        return QImage(QString::fromStdString((out / "IMG_1.png").string()));
    };
    const QImage snapshot = exportOf("snap", {"--snapshot", "1"});
    const QImage saved = exportOf("saved", {});
    REQUIRE(snapshot != saved); // the fixture shows the demosaic

    const test::TempDir other;
    const fs::path twin = other.file("IMG_1.dng");
    fs::copy_file(test::fixture("bayer-detail-32x24.dng"), twin);
    writeSidecar(openPhoto(twin).with(vng));
    const fs::path out = other.file("out");
    fs::create_directories(out);
    REQUIRE(invoke({"export", twin.string(), "-o", out.string(), "--format", "png", "--bit-depth",
                    "16", "--device", "cpu", "--quiet"})
                .code == cli::Success);
    CHECK(QImage(QString::fromStdString((out / "IMG_1.png").string())) == snapshot);
}

TEST_CASE("Export --snapshot in a batch fails only the photograph lacking the ID",
          "[cli][export][snapshots]") {
    const Rig rig;
    const fs::path bare = rig.directory.file("IMG_2.dng");
    fs::copy_file(test::fixture("linear-32x24-neutral.dng"), bare);
    const fs::path out = rig.directory.file("batch");
    fs::create_directories(out);
    const auto result = invoke({"export", rig.photo.string(), bare.string(), "-o", out.string(),
                                "--format", "png", "--device", "cpu", "--snapshot", "1"});
    CHECK(result.code == cli::Failed);
    CHECK(fs::exists(out / "IMG_1.png"));
    CHECK_FALSE(fs::exists(out / "IMG_2.png"));
    CHECK_THAT(result.err, ContainsSubstring("IMG_2.dng"));
    CHECK_THAT(result.err, ContainsSubstring("no snapshot 1"));
}

TEST_CASE("Export --snapshot accepts leading zeros and the largest ID",
          "[cli][export][snapshots]") {
    const Rig rig;
    const QImage one(QString::fromStdString(rig.exportTo("one", {"--snapshot", "1"}).string()));
    const QImage padded(
        QString::fromStdString(rig.exportTo("padded", {"--snapshot", "001"}).string()));
    CHECK(padded == one);

    Invocation result;
    rig.exportTo("largest", {"--snapshot", "18446744073709551615"}, &result);
    CHECK(result.code == cli::Failed); // a well-formed ID, but no such snapshot
    CHECK_THAT(result.err, ContainsSubstring("no snapshot 18446744073709551615"));
}

TEST_CASE("Export --snapshot given twice is a usage error", "[cli][export][snapshots]") {
    const Rig rig;
    Invocation result;
    const fs::path png = rig.exportTo("twice", {"--snapshot", "1", "--snapshot", "2"}, &result);
    CHECK(result.code == cli::UsageError);
    CHECK_THAT(result.err, ContainsSubstring("twice"));
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("Export --snapshot beside a damaged entry or a damaged sidecar",
          "[cli][export][snapshots]") {
    const Rig rig;
    const QImage clean(QString::fromStdString(rig.exportTo("clean", {"--snapshot", "1"}).string()));
    addUnusableEntry(rig.photo);
    const QImage damaged(
        QString::fromStdString(rig.exportTo("damaged", {"--snapshot", "1"}).string()));
    CHECK(damaged == clean);

    Invocation result;
    rig.exportTo("unknown", {"--snapshot", "5"}, &result);
    CHECK(result.code == cli::Failed);
    CHECK_THAT(result.err, ContainsSubstring("cannot read"));

    {
        std::ofstream broken(sidecarPath(rig.photo), std::ios::binary | std::ios::trunc);
        broken << "this is not xml";
    }
    const fs::path png = rig.exportTo("broken", {"--snapshot", "1"}, &result);
    CHECK(result.code == cli::Failed);
    CHECK_THAT(result.err, ContainsSubstring("sidecar"));
    CHECK_THAT(result.err, !ContainsSubstring("no snapshot"));
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("Export --snapshot repeats with distinct outputs and writes no sidecar",
          "[cli][export][snapshots]") {
    const Rig rig;
    const std::string before = bytesOf(sidecarPath(rig.photo));
    const OpenDocument opened = openDocument(rig.photo);

    const QImage first(QString::fromStdString(rig.exportTo("a", {"--snapshot", "1"}).string()));
    const QImage second(QString::fromStdString(rig.exportTo("b", {"--snapshot", "2"}).string()));
    const QImage third(QString::fromStdString(rig.exportTo("c", {"--snapshot", "1"}).string()));
    CHECK(first != second);
    CHECK(first == third);
    CHECK(bytesOf(sidecarPath(rig.photo)) == before);
    CHECK(openDocument(rig.photo).snapshots == opened.snapshots);
}

TEST_CASE("Export --snapshot with an unknown ID fails clearly", "[cli][export][snapshots]") {
    const Rig rig;
    Invocation result;
    const fs::path png = rig.exportTo("none", {"--snapshot", "99"}, &result);
    CHECK(result.code == cli::Failed);
    CHECK_THAT(result.err, ContainsSubstring("no snapshot 99"));
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("Export --snapshot of a photograph without a sidecar fails", "[cli][export][snapshots]") {
    const test::TempDir directory;
    const fs::path photo = directory.file("plain.dng");
    fs::copy_file(test::fixture("linear-32x24-neutral.dng"), photo);
    const fs::path out = directory.file("out");
    fs::create_directories(out);
    const auto result = invoke(
        {"export", photo.string(), "-o", out.string(), "--device", "cpu", "--snapshot", "1"});
    CHECK(result.code == cli::Failed);
    CHECK_THAT(result.err, ContainsSubstring("no snapshot 1"));
}

TEST_CASE("Export --snapshot conflicts with --no-sidecar", "[cli][export][snapshots]") {
    const Rig rig;
    Invocation result;
    const fs::path png = rig.exportTo("conflict", {"--snapshot", "1", "--no-sidecar"}, &result);
    CHECK(result.code == cli::UsageError);
    CHECK_THAT(result.err, ContainsSubstring("--snapshot"));
    CHECK_THAT(result.err, ContainsSubstring("--no-sidecar"));
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("Export --snapshot takes only plain decimal digits", "[cli][export][snapshots]") {
    const Rig rig;
    const std::string text = GENERATE("", "abc", "-1", "+1", "0", "1.0", "0x1", " 1", "1 ", "1e0",
                                      "18446744073709551616", "١");
    Invocation result;
    const fs::path png = rig.exportTo("bad", {"--snapshot", text}, &result);
    CAPTURE(text, result.err);
    CHECK(result.code == cli::UsageError);
    CHECK_THAT(result.err, ContainsSubstring("snapshot"));
    CHECK_FALSE(fs::exists(png));
}

TEST_CASE("withSnapshot copies the recipe exactly and keeps the file and marks",
          "[snapshots][cli]") {
    const Rig rig;
    OpenDocument document = openDocument(rig.photo);
    document.photo = document.photo.with(PhotoMarks{.rating = 3});
    NegativeSettings negative = document.photo.negative();
    negative.demosaic = Demosaic::Vng;
    Snapshot snapshot = *document.snapshots.find(SnapshotId{1});
    snapshot.negative = negative;

    const Photo made = withSnapshot(document.photo, snapshot);
    CHECK(made.path() == document.photo.path());
    CHECK(made.marks() == document.photo.marks());
    CHECK(made.state() == snapshot.state);
    CHECK(made.negative() == negative);
    CHECK(made.metadata() == document.photo.metadata());
}
