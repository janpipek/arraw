#include "cli/InfoCommand.h"
#include "core/ImageMetadata.h"
#include "develop/DevelopGroup.h"
#include "develop/LocalAdjustment.h"
#include "develop/Spot.h"
#include "io/XmpSidecar.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <libraw/libraw.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

// The generated DNG fixture (tests/fixtures/make_test_dng.py): the only real
// RAW in the tree, so the one file whose EXIF LibRaw can actually read.
const QString kDng = QStringLiteral(ARRAW_FIXTURE_DIR "/gradient-32x24.dng");

// A copy of the fixture in a writable dir, so a test can give it a sidecar
// without dirtying the checked-in tree.
QString dngCopy(const QString& dir, const QString& fileName) {
    const QString path = QDir(dir).filePath(fileName);
    REQUIRE(QFile::copy(kDng, path));
    return path;
}

// A sidecar that exists but is not parseable XMP.
void writeBrokenSidecar(const QString& imagePath) {
    QFile f(XmpSidecar::pathFor(imagePath));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("<x:xmpmeta><this never closes");
}

} // namespace

TEST_CASE("info table heads each file's block with its path") {
    QString outText, errText;
    QTextStream out(&outText), err(&errText);

    REQUIRE(cli::runInfo({kDng}, false, out, err) == 0);
    REQUIRE(outText.contains(kDng));
    REQUIRE(errText.isEmpty());
}

TEST_CASE("info refuses the whole run on a missing path, reading nothing") {
    QTemporaryDir tmp;
    const QString missing = QDir(tmp.path()).filePath("missing.arw");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({kDng, missing}, false, out, err) == 2);

    REQUIRE(outText.isEmpty()); // pre-flight ran before the readable file was touched
    REQUIRE(errText.contains("missing.arw"));
}

TEST_CASE("info refuses a directory: no folder mode in v1") {
    QTemporaryDir tmp;

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({tmp.path()}, false, out, err) == 2);

    REQUIRE(outText.isEmpty());
    REQUIRE(errText.contains("directory"));
}

TEST_CASE("info refuses a path the loaders don't recognise as an image") {
    QTemporaryDir tmp;
    const QString notes = QDir(tmp.path()).filePath("notes.txt");
    {
        QFile f(notes);
        REQUIRE(f.open(QIODevice::WriteOnly));
    }

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({notes}, false, out, err) == 2);

    REQUIRE(outText.isEmpty());
    REQUIRE(errText.contains("notes.txt"));
}

TEST_CASE("info table reports the camera EXIF the GUI's Info panel shows") {
    QString outText, errText;
    QTextStream out(&outText), err(&errText);

    REQUIRE(cli::runInfo({kDng}, false, out, err) == 0);

    // Assert against the same extractor the Info panel renders, rather than
    // hardcoding the fixture's camera strings: the contract is "info shows
    // what arraw knows", not "info shows these literals".
    LibRaw raw;
    REQUIRE(raw.open_file(kDng.toLocal8Bit().constData()) == LIBRAW_SUCCESS);
    const ImageMetadata expected = extractMetadata(raw);
    REQUIRE_FALSE(expected.empty());
    for (const auto& [label, value] : expected.rows)
        REQUIRE(outText.contains(label + ": " + value));
}

TEST_CASE("info table distinguishes a file with no sidecar from an edited one") {
    QTemporaryDir tmp;
    const QString bare = dngCopy(tmp.path(), "bare.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({bare}, false, out, err) == 0);

    // A missing sidecar is not an error: it reads as all-default, and only the
    // explicit flag separates that from "edited back to default" (docs/adr/0053).
    REQUIRE(outText.contains("Sidecar: none"));
    REQUIRE(errText.isEmpty());
}

TEST_CASE("info table reports the sidecar's User Metadata") {
    QTemporaryDir tmp;
    const QString rated = dngCopy(tmp.path(), "rated.dng");
    UserMetadata meta;
    meta.rating = 4;
    meta.label = ColourLabel::Green;
    meta.title = "Harbour at dawn";
    meta.creator = "Jan Pipek";
    meta.keywords = QStringList{"boats", "sunrise"};
    REQUIRE(XmpSidecar::saveMetadata(rated, meta));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({rated}, false, out, err) == 0);

    REQUIRE(outText.contains("Sidecar: present"));
    REQUIRE(outText.contains("Rating: 4"));
    REQUIRE(outText.contains("Colour label: Green"));
    REQUIRE(outText.contains("Title: Harbour at dawn"));
    REQUIRE(outText.contains("Creator: Jan Pipek"));
    REQUIRE(outText.contains("Keywords: boats, sunrise"));
}

TEST_CASE("info table names each non-default develop group and what it changed") {
    QTemporaryDir tmp;
    const QString edited = dngCopy(tmp.path(), "edited.dng");
    GlobalAdjustment adjustments;
    adjustments.exposure = 0.5f;    // Tone
    adjustments.saturation = 20.0f; // Colour
    REQUIRE(XmpSidecar::saveAdjustments(edited, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({edited}, false, out, err) == 0);

    // Reuse the describer the GUI and `preset show` render, rather than
    // restating its label/formatting conventions here.
    REQUIRE(outText.contains(developGroupLabel(DevelopGroup::Tone)));
    REQUIRE(outText.contains(developGroupLabel(DevelopGroup::Colour)));
    for (const QString& line : describeGroupNonDefaults(DevelopGroup::Tone, adjustments))
        REQUIRE(outText.contains(line));
    for (const QString& line : describeGroupNonDefaults(DevelopGroup::Colour, adjustments))
        REQUIRE(outText.contains(line));

    // Untouched groups stay out of the report entirely.
    REQUIRE_FALSE(outText.contains(developGroupLabel(DevelopGroup::Effects)));
}

TEST_CASE("info table says so when a file has no develop edits at all") {
    QTemporaryDir tmp;
    const QString bare = dngCopy(tmp.path(), "bare.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({bare}, false, out, err) == 0);

    REQUIRE(outText.contains("Develop: no edits"));
}

TEST_CASE("info --json emits one array element per input path") {
    QTemporaryDir tmp;
    const QString a = dngCopy(tmp.path(), "a.dng");
    const QString b = dngCopy(tmp.path(), "b.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({a, b}, true, out, err) == 0);

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(outText.toUtf8(), &parseErr);
    REQUIRE(parseErr.error == QJsonParseError::NoError);
    REQUIRE(doc.isArray()); // a listing of independent reports, not a batch-operation shape
    const QJsonArray arr = doc.array();
    REQUIRE(arr.size() == 2);
    CHECK(arr.at(0).toObject()["path"].toString() == a);
    CHECK(arr.at(1).toObject()["path"].toString() == b);
}

TEST_CASE("info --json types EXIF numbers as numbers, not display strings") {
    QTemporaryDir tmp;
    const QString path = dngCopy(tmp.path(), "a.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({path}, true, out, err) == 0);

    const QJsonObject exif
        = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject()["exif"].toObject();

    // A script filtering `iso > 1600` must not have to parse "f/2.8" back into
    // 2.8 (docs/adr/0053), so every numeric key is a JSON number.
    LibRaw raw;
    REQUIRE(raw.open_file(path.toLocal8Bit().constData()) == LIBRAW_SUCCESS);
    const ExifData expected = extractExifData(raw);
    CHECK(exif["width"].toInt() == expected.width);
    CHECK(exif["height"].toInt() == expected.height);
    CHECK(exif["make"].toString() == expected.make);
    for (const QString& key : {QStringLiteral("width"), QStringLiteral("height")})
        CHECK(exif[key].isDouble());
}

TEST_CASE("info --json keys develop groups by their stable machine keys") {
    QTemporaryDir tmp;
    const QString edited = dngCopy(tmp.path(), "edited.dng");
    GlobalAdjustment adjustments;
    adjustments.exposure = 0.5f;
    REQUIRE(XmpSidecar::saveAdjustments(edited, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({edited}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK(report["hasSidecar"].toBool());
    CHECK(report["rating"].toInt() == 0);

    // The same native group serialization `preset show --json` already exposes,
    // keyed by developGroupKey — never a localised label (docs/adr/0050).
    const QJsonObject groups = report["developGroups"].toObject();
    REQUIRE(groups.contains(developGroupKey(DevelopGroup::Tone)));
    CHECK(groups[developGroupKey(DevelopGroup::Tone)].toObject()["exposure"].toDouble() == 0.5);
    CHECK_FALSE(groups.contains(developGroupKey(DevelopGroup::Effects)));
}

TEST_CASE("info --json reports a colour label by its canonical on-disk string") {
    QTemporaryDir tmp;
    const QString labelled = dngCopy(tmp.path(), "labelled.dng");
    UserMetadata meta;
    meta.label = ColourLabel::Purple;
    REQUIRE(XmpSidecar::saveMetadata(labelled, meta));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({labelled}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK(report["colourLabel"].toString() == colourLabelToString(ColourLabel::Purple));
}

TEST_CASE("info --json marks a file with no sidecar as unedited, not as an error") {
    QTemporaryDir tmp;
    const QString bare = dngCopy(tmp.path(), "bare.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({bare}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK_FALSE(report["hasSidecar"].toBool());
    CHECK(report["developGroups"].toObject().isEmpty());
    CHECK_FALSE(report.contains("error"));
    CHECK(errText.isEmpty());
}

TEST_CASE("info fails only the corrupt file and still reports the others, exiting 1") {
    QTemporaryDir tmp;
    const QString good = dngCopy(tmp.path(), "good.dng");
    const QString corrupt = QDir(tmp.path()).filePath("corrupt.dng");
    {
        QFile f(corrupt); // right extension, contents LibRaw cannot open
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a raw file");
    }

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    // A batch-1 tier, not a refusal: files are independent, so one bad file
    // must not hide the report on the other 299 (docs/adr/0053).
    REQUIRE(cli::runInfo({corrupt, good}, false, out, err) == 1);

    REQUIRE(outText.contains(good));
    REQUIRE(errText.contains("corrupt.dng"));
}

TEST_CASE("info --json reports a failed file inline as well as on stderr") {
    QTemporaryDir tmp;
    const QString corrupt = QDir(tmp.path()).filePath("corrupt.dng");
    {
        QFile f(corrupt);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a raw file");
    }

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({corrupt}, true, out, err) == 1);

    const QJsonArray arr = QJsonDocument::fromJson(outText.toUtf8()).array();
    REQUIRE(arr.size() == 1);
    const QJsonObject report = arr.at(0).toObject();
    CHECK(report["path"].toString() == corrupt);
    CHECK_FALSE(report["error"].toString().isEmpty());
    CHECK(errText.contains("corrupt.dng"));
}

TEST_CASE("info treats a standard image's absent EXIF as a gap, not a failure") {
    QTemporaryDir tmp;
    const QString jpeg = QDir(tmp.path()).filePath("shot.jpg");
    {
        QFile f(jpeg);
        REQUIRE(f.open(QIODevice::WriteOnly));
    }

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    // StandardImageLoader extracts no EXIF today; that pre-existing gap is not
    // this command's to close, and must not read as a per-file error.
    REQUIRE(cli::runInfo({jpeg}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK(report["exif"].toObject().isEmpty());
    CHECK_FALSE(report.contains("error"));
    CHECK(errText.isEmpty());
}

TEST_CASE("info --json omits colourLabel entirely when the photo carries none") {
    QTemporaryDir tmp;
    const QString unlabelled = dngCopy(tmp.path(), "unlabelled.dng");
    UserMetadata meta;
    meta.rating = 3; // rated, but no colour label
    REQUIRE(XmpSidecar::saveMetadata(unlabelled, meta));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({unlabelled}, true, out, err) == 0);

    // Absence is a missing key, the same as every other optional field —
    // never an empty string a script has to treat as a third state.
    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK(report["rating"].toInt() == 3);
    CHECK_FALSE(report.contains("colourLabel"));
}

TEST_CASE("info reports an unreadable sidecar rather than calling it unedited") {
    QTemporaryDir tmp;
    const QString broken = dngCopy(tmp.path(), "broken.dng");
    writeBrokenSidecar(broken);

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    // A sidecar that exists but won't parse means the edit state below is
    // defaults standing in for something real. Reporting it as "no edits"
    // would be a silent wrong answer, so it joins the per-file failure tier —
    // the same tier the GUI surfaces as "Sidecar unreadable; defaults applied".
    REQUIRE(cli::runInfo({broken}, false, out, err) == 1);

    REQUIRE(outText.contains("Sidecar: unreadable"));
    REQUIRE_FALSE(outText.contains("Sidecar: present"));
    REQUIRE(errText.contains("sidecar unreadable"));
}

TEST_CASE("info --json flags an unreadable sidecar with its own key") {
    QTemporaryDir tmp;
    const QString broken = dngCopy(tmp.path(), "broken.dng");
    writeBrokenSidecar(broken);

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({broken}, true, out, err) == 1);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK(report["hasSidecar"].toBool());        // the file is there
    CHECK(report["sidecarUnreadable"].toBool()); // ...but says nothing usable
}

TEST_CASE("info --json omits sidecarUnreadable when the sidecar parses") {
    QTemporaryDir tmp;
    const QString edited = dngCopy(tmp.path(), "edited.dng");
    GlobalAdjustment adjustments;
    adjustments.exposure = 0.5f;
    REQUIRE(XmpSidecar::saveAdjustments(edited, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({edited}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK_FALSE(report.contains("sidecarUnreadable"));
}

TEST_CASE("a file whose EXIF fails still reports the edit state its sidecar carries") {
    QTemporaryDir tmp;
    const QString corrupt = QDir(tmp.path()).filePath("corrupt.dng");
    {
        QFile f(corrupt); // right extension, contents LibRaw cannot open
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a raw file");
    }
    UserMetadata meta;
    meta.rating = 5;
    REQUIRE(XmpSidecar::saveMetadata(corrupt, meta));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    // EXIF and sidecar are independent halves: losing the one LibRaw owns is
    // no reason to drop the rating that parsed perfectly well.
    REQUIRE(cli::runInfo({corrupt}, true, out, err) == 1);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    CHECK_FALSE(report["error"].toString().isEmpty());
    CHECK_FALSE(report.contains("exif")); // the half that genuinely failed
    CHECK(report["hasSidecar"].toBool());
    CHECK(report["rating"].toInt() == 5);
}

TEST_CASE("info table gives a failed file a block with its error, not silence on stdout") {
    QTemporaryDir tmp;
    const QString corrupt = QDir(tmp.path()).filePath("corrupt.dng");
    {
        QFile f(corrupt);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a raw file");
    }

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({corrupt}, false, out, err) == 1);

    REQUIRE(outText.contains(corrupt)); // a reader piping stdout still sees it
    REQUIRE(outText.contains("Error: cannot read image"));
}

TEST_CASE("info separates consecutive file blocks with a blank line") {
    QTemporaryDir tmp;
    const QString a = dngCopy(tmp.path(), "a.dng");
    const QString b = dngCopy(tmp.path(), "b.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({a, b}, false, out, err) == 0);

    REQUIRE_FALSE(outText.startsWith("\n")); // ...but never before the first
    REQUIRE(outText.contains("\n\n" + b));
}

TEST_CASE("info never writes: no sidecar appears, and an existing one is untouched") {
    QTemporaryDir tmp;
    const QString bare = dngCopy(tmp.path(), "bare.dng");
    const QString edited = dngCopy(tmp.path(), "edited.dng");
    GlobalAdjustment adjustments;
    adjustments.exposure = 0.5f;
    REQUIRE(XmpSidecar::saveAdjustments(edited, adjustments));
    const QByteArray before = [&] {
        QFile f(XmpSidecar::pathFor(edited));
        REQUIRE(f.open(QIODevice::ReadOnly));
        return f.readAll();
    }();

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({bare, edited}, false, out, err) == 0);

    // The command's headline promise (docs/adr/0053): reading a file must
    // never be what creates or rewrites its sidecar.
    CHECK_FALSE(QFile::exists(XmpSidecar::pathFor(bare)));
    QFile after(XmpSidecar::pathFor(edited));
    REQUIRE(after.open(QIODevice::ReadOnly));
    CHECK(after.readAll() == before);
}

TEST_CASE("info colours the table only when asked, and never the JSON") {
    QTemporaryDir tmp;
    const QString rated = dngCopy(tmp.path(), "rated.dng");
    UserMetadata meta;
    meta.rating = 4;
    meta.label = ColourLabel::Green;
    REQUIRE(XmpSidecar::saveMetadata(rated, meta));

    const QString esc = QStringLiteral("\033[");

    QString plainText, errText;
    QTextStream plain(&plainText), err(&errText);
    REQUIRE(cli::runInfo({rated}, false, plain, err) == 0);
    CHECK_FALSE(plainText.contains(esc)); // the default every pipe gets

    QString colourText;
    QTextStream colour(&colourText);
    REQUIRE(cli::runInfo({rated}, false, colour, err, cli::TextStyle(true)) == 0);
    CHECK(colourText.contains(esc));
    CHECK(colourText.contains("\033[1m" + rated + "\033[0m")); // path in bold
    CHECK(colourText.contains("\033[32mGreen\033[0m"));        // a green label, in green

    QString jsonText;
    QTextStream json(&jsonText);
    REQUIRE(cli::runInfo({rated}, true, json, err, cli::TextStyle(true)) == 0);
    CHECK_FALSE(jsonText.contains(esc)); // machine output is never seasoned
}

TEST_CASE("info lists the local masks a photo carries, with what each one changes") {
    QTemporaryDir tmp;
    const QString masked = dngCopy(tmp.path(), "masked.dng");
    GlobalAdjustment adjustments;
    LocalAdjustment linear; // default-constructed Mask is Linear
    linear.exposure = 0.5f;
    linear.shadows = -30.0f;
    LocalAdjustment radial;
    radial.mask = RadialMask{};
    radial.saturation = 20.0f;
    adjustments.localAdjustments = {linear, radial};
    REQUIRE(XmpSidecar::saveAdjustments(masked, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({masked}, false, out, err) == 0);

    // Local Adjustments sit outside the DevelopGroup enum a preset carries
    // (docs/adr/0023), so nothing in the Develop section would ever mention
    // them — yet they are most of what was done to a masked photo.
    REQUIRE(outText.contains("Masks:"));
    REQUIRE(outText.contains(maskDisplayName(adjustments.localAdjustments, 0))); // "Linear 1"
    REQUIRE(outText.contains(maskDisplayName(adjustments.localAdjustments, 1))); // "Radial 1"
    // Reuse the describer rather than restating its formatting here.
    for (const QString& delta : describeLocalNonDefaults(linear))
        REQUIRE(outText.contains(delta));
    REQUIRE(outText.contains(describeLocalNonDefaults(radial).join(", ")));
}

TEST_CASE("info says so when a mask is drawn but adjusts nothing") {
    QTemporaryDir tmp;
    const QString masked = dngCopy(tmp.path(), "masked.dng");
    GlobalAdjustment adjustments;
    adjustments.localAdjustments = {LocalAdjustment{}}; // drawn, all deltas zero
    REQUIRE(XmpSidecar::saveAdjustments(masked, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({masked}, false, out, err) == 0);

    REQUIRE(outText.contains("Linear 1 — no adjustments"));
}

TEST_CASE("info omits the Masks section entirely for a photo with none") {
    QTemporaryDir tmp;
    const QString plain = dngCopy(tmp.path(), "plain.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({plain}, false, out, err) == 0);

    // Absence is absence: an unmasked file shouldn't grow a line saying so.
    REQUIRE_FALSE(outText.contains("Masks:"));
}

TEST_CASE("info --json keys each mask by kind and reports only its changed deltas") {
    QTemporaryDir tmp;
    const QString masked = dngCopy(tmp.path(), "masked.dng");
    GlobalAdjustment adjustments;
    LocalAdjustment brush;
    brush.mask = BrushMask{};
    brush.exposure = -0.25f;
    brush.temperature = -40.0f; // a relative shift here, never Kelvin
    adjustments.localAdjustments = {brush};
    REQUIRE(XmpSidecar::saveAdjustments(masked, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({masked}, true, out, err) == 0);

    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    const QJsonArray masks = report["masks"].toArray();
    REQUIRE(masks.size() == 1);
    const QJsonObject m = masks.at(0).toObject();
    CHECK(m["type"].toString() == QString::fromLatin1(maskKindName(brush.mask)));
    CHECK(m["exposure"].toDouble() == Catch::Approx(-0.25));
    CHECK(m["temperature"].toDouble() == Catch::Approx(-40.0));
    CHECK_FALSE(m.contains("vibrance")); // untouched deltas stay out
    CHECK_FALSE(m.contains("mask"));     // geometry is not part of the report
}

TEST_CASE("info --json always carries a masks array, empty when there are none") {
    QTemporaryDir tmp;
    const QString plain = dngCopy(tmp.path(), "plain.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({plain}, true, out, err) == 0);

    // A listing a script iterates: present and empty, never a missing key it
    // has to test for first.
    const QJsonObject report = QJsonDocument::fromJson(outText.toUtf8()).array().at(0).toObject();
    REQUIRE(report.contains("masks"));
    CHECK(report["masks"].toArray().isEmpty());
}

TEST_CASE("info counts the spots a photo carries") {
    QTemporaryDir tmp;
    const QString retouched = dngCopy(tmp.path(), "retouched.dng");
    GlobalAdjustment adjustments;
    adjustments.spots
        = {Spot{.destination = {10, 10}, .source = {20, 20}, .radius = 5.0},
           Spot{.destination = {30, 30}, .source = {40, 40}, .radius = 8.0}};
    REQUIRE(XmpSidecar::saveAdjustments(retouched, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({retouched}, false, out, err) == 0);

    // A count is the whole story: every field of a Spot is geometry, and the
    // Develop groups can't mention them at all (docs/adr/0017, docs/adr/0023).
    REQUIRE(outText.contains("Spots: 2"));
}

TEST_CASE("info omits the spot count for a photo that has none") {
    QTemporaryDir tmp;
    const QString plain = dngCopy(tmp.path(), "plain.dng");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({plain}, false, out, err) == 0);

    REQUIRE_FALSE(outText.contains("Spots:"));
}

TEST_CASE("info --json always reports a spot count, zero included") {
    QTemporaryDir tmp;
    const QString retouched = dngCopy(tmp.path(), "retouched.dng");
    const QString plain = dngCopy(tmp.path(), "plain.dng");
    GlobalAdjustment adjustments;
    adjustments.spots = {Spot{.destination = {10, 10}, .source = {20, 20}, .radius = 5.0}};
    REQUIRE(XmpSidecar::saveAdjustments(retouched, adjustments));

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    REQUIRE(cli::runInfo({retouched, plain}, true, out, err) == 0);

    const QJsonArray arr = QJsonDocument::fromJson(outText.toUtf8()).array();
    CHECK(arr.at(0).toObject()["spots"].toInt() == 1);
    // Present and zero rather than absent, so a script never tests for the key.
    REQUIRE(arr.at(1).toObject().contains("spots"));
    CHECK(arr.at(1).toObject()["spots"].toInt() == 0);
}
