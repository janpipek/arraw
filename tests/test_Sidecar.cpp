#include "SettingCodec.h"
#include "support/Fixtures.h"
#include "support/Sentinels.h"
#include "support/TempDir.h"

#include <Photo.h>
#include <SettingDescriptors.h>
#include <Sidecar.h>

#include <QDomDocument>
#include <QFile>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;

/// XMP sidecars (Sidecar.h): the settings and marks of a photograph beside it,
/// which arraw edits without disturbing what other programs keep there.

namespace {

namespace fs = std::filesystem;

constexpr std::string_view rawFixture = "linear-32x24-neutral.dng";
constexpr std::string_view arrawNs = "http://ns.arraw.org/develop/1.0/";
constexpr std::string_view xmpNs = "http://ns.adobe.com/xap/1.0/";

std::string slurp(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void spit(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

/// Copies a RAW fixture into a directory under a name, so a sidecar can sit beside it.
fs::path copyRaw(const test::TempDir& directory, const std::string& name) {
    const fs::path path = directory.file(name);
    fs::copy_file(test::fixture(rawFixture), path);
    return path;
}

/// A photograph over a real file, developed and marked as given.
Photo photoOf(const fs::path& path, DevelopSettings settings = {}, PhotoMarks marks = {}) {
    return openPhoto(path).with(DevelopState{settings}).with(marks);
}

DevelopSettings allNonDefault() {
    DevelopSettings settings;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        test::setNonDefault(descriptor, settings);
    }
    return settings;
}

QDomDocument parsed(const fs::path& path) {
    QDomDocument document;
    const QByteArray bytes = QByteArray::fromStdString(slurp(path));
    const auto result =
        document.setContent(bytes, QDomDocument::ParseOption::UseNamespaceProcessing);
    REQUIRE(bool(result));
    return document;
}

/// Everything a document says, as `{namespace}local` -> value, for comparing two documents by
/// meaning. Attributes are keyed by the description's position; elements by their path.
void collect(const QDomElement& element, const std::string& path,
             std::map<std::string, std::string>& into) {
    const QDomNamedNodeMap attributes = element.attributes();
    for (int i = 0; i < attributes.size(); ++i) {
        const QDomAttr attribute = attributes.item(i).toAttr();
        if (attribute.name().startsWith("xmlns")) {
            continue;
        }
        into[path + "@{" + attribute.namespaceURI().toStdString() + "}" +
             attribute.localName().toStdString()] = attribute.value().toStdString();
    }
    std::map<std::string, int> seen;
    for (QDomElement child = element.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        const std::string name =
            "{" + child.namespaceURI().toStdString() + "}" + child.localName().toStdString();
        const std::string here = path + "/" + name + "#" + std::to_string(seen[name]++);
        if (child.firstChildElement().isNull()) {
            into[here] = child.text().toStdString();
        }
        collect(child, here, into);
    }
}

std::map<std::string, std::string> meaning(const fs::path& path) {
    std::map<std::string, std::string> into;
    collect(parsed(path).documentElement(), "", into);
    return into;
}

std::size_t count(const CollectedDiagnostics& log, Notice notice) {
    std::size_t total = 0;
    for (const Diagnostic& entry : log.entries()) {
        total += entry.notice == notice ? 1 : 0;
    }
    return total;
}

} // namespace

TEST_CASE("Every settings row survives a write and a read", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        DevelopSettings settings;
        test::setNonDefault(descriptor, settings);
        writeSidecar(photoOf(path, settings));
        CollectedDiagnostics log;
        const auto contents = readSidecar(path, log);
        REQUIRE(contents);
        REQUIRE(contents->state.settings == settings);
        REQUIRE(log.entries().empty());
    }
    const DevelopSettings everything = allNonDefault();
    writeSidecar(photoOf(path, everything));
    REQUIRE(readSidecar(path)->state.settings == everything);
    // Back to defaults: the unset optionals leave no attribute, and read back as unset.
    writeSidecar(photoOf(path));
    REQUIRE(readSidecar(path)->state.settings == DevelopSettings{});
}

TEST_CASE("Marks survive a write and a read", "[sidecar][marks]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    for (const int rating : {-1, 0, 1, 5}) {
        for (const auto& [label, name] : colorLabelNames) {
            const PhotoMarks marks{.rating = rating, .label = label};
            writeSidecar(photoOf(path, {}, marks));
            REQUIRE(readSidecar(path)->marks == marks);
        }
    }
    writeSidecar(photoOf(path, {}, {.rating = 2, .label = ColorLabel::Green}));
    writeSidecar(photoOf(path, {}, {.rating = 2}));
    REQUIRE(readSidecar(path)->marks == PhotoMarks{.rating = 2});
    REQUIRE(slurp(sidecarPath(path)).find("Label") == std::string::npos);
}

TEST_CASE("A new sidecar is an XMP packet holding the settings as attributes", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    DevelopSettings settings;
    settings.tone.exposure = 0.1F;
    settings.color.temperature = 5500.0F;
    settings.geometry.flipVertical = true;
    settings.geometry.crop.rectangle =
        UprightCropRect{.left = 0.25, .top = 0.0, .right = 1.0, .bottom = 0.5};
    settings.geometry.crop.aspect = CropRatio{1.5};
    writeSidecar(photoOf(path, settings, {.rating = 4, .label = ColorLabel::Red}));

    const std::string text = slurp(directory.file("IMG_1.xmp"));
    REQUIRE(text.starts_with("<?xpacket begin="));
    REQUIRE(text.find("<?xpacket end=") != std::string::npos);
    for (const char* piece :
         {"arraw:version=\"1\"", "arraw:exposure=\"0.1\"", "arraw:temperature=\"5500\"",
          "arraw:flipVertical=\"True\"", "arraw:flipHorizontal=\"False\"",
          "arraw:whiteBalance=\"asShot\"", "arraw:cropRectangle=\"0.25,0,1,0.5\"",
          "arraw:cropAspect=\"1.5\"", "xmp:Rating=\"4\"", "xmp:Label=\"Red\""}) {
        INFO(piece);
        REQUIRE(text.find(piece) != std::string::npos);
    }
    // Unset optionals have no attribute.
    REQUIRE(text.find("arraw:tint") == std::string::npos);
}

TEST_CASE("Reading names the other tools that wrote in a sidecar", "[sidecar][others]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), directory.file("IMG_1.xmp"));

    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->creatorTool == "Adobe Lightroom Classic 13.0 (Macintosh)");
    // crs: on attributes, acme: on elements, then dc: in the second description;
    // arraw:, xmp:, rdf: and x: are not counted.
    const std::vector<ForeignNamespace> expected = {
        {"http://ns.adobe.com/camera-raw-settings/1.0/", "crs", 4},
        {"http://ns.example.com/acme/1.0/", "acme", 2},
        {"http://purl.org/dc/elements/1.1/", "dc", 1},
    };
    REQUIRE(contents->others == expected);
    REQUIRE(xmpNamespaceOwner(expected[0].uri) == "Adobe Camera Raw / Lightroom develop settings");
    REQUIRE(xmpNamespaceOwner(expected[2].uri) == "Dublin Core");
    REQUIRE_FALSE(xmpNamespaceOwner(expected[1].uri));
}

TEST_CASE("The creator tool is read as an element too", "[sidecar][others]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    spit(
        directory.file("IMG_1.xmp"),
        R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
 <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xml:lang="en"><xmp:CreatorTool>darktable 4.6</xmp:CreatorTool></rdf:Description>
</rdf:RDF></x:xmpmeta>)");
    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->creatorTool == "darktable 4.6");
    REQUIRE(contents->others.empty());
}

TEST_CASE("A sidecar arraw wrote has no other tools in it", "[sidecar][others]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    writeSidecar(photoOf(path, allNonDefault(), {.rating = 2, .label = ColorLabel::Red}));
    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE_FALSE(contents->creatorTool);
    REQUIRE(contents->others.empty());
}

TEST_CASE("Writing over a foreign sidecar keeps everything it does not own", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), sidecar);
    const auto before = meaning(sidecar);

    DevelopSettings settings;
    settings.tone.exposure = -0.75F;
    writeSidecar(photoOf(path, settings, {.rating = 5, .label = ColorLabel::Purple}));
    const auto after = meaning(sidecar);

    // Everything foreign is still there, with the same value.
    std::size_t kept = 0;
    for (const auto& [key, value] : before) {
        const bool owned = key.find("{" + std::string(arrawNs) + "}") != std::string::npos ||
                           key.find("{" + std::string(xmpNs) + "}") != std::string::npos;
        const bool unknownArraw = key.ends_with("}futureKnob");
        if (owned && !unknownArraw) {
            continue;
        }
        INFO(key);
        REQUIRE(after.contains(key));
        REQUIRE(after.at(key) == value);
        ++kept;
    }
    REQUIRE(kept >= 10);
    const auto valueOf = [](const std::map<std::string, std::string>& document,
                            const std::string& suffix) {
        for (const auto& [key, value] : document) {
            if (key.ends_with(suffix)) {
                return value;
            }
        }
        return std::string("<absent>");
    };
    REQUIRE(valueOf(after, "@{http://ns.adobe.com/camera-raw-settings/1.0/}Exposure2012") ==
            "+0.50");
    REQUIRE(valueOf(after, "@{http://ns.arraw.org/develop/1.0/}futureKnob") == "7");
    REQUIRE(valueOf(after,
                    "{http://purl.org/dc/elements/1.1/}creator#0/{http://www.w3.org/1999/02/"
                    "22-rdf-syntax-ns#}Seq#0/{http://www.w3.org/1999/02/22-rdf-syntax-ns#}li#0") ==
            "A. Photographer");

    // What it owns has moved to the new values.
    const auto contents = readSidecar(path);
    REQUIRE(contents->state.settings == settings);
    REQUIRE(contents->marks == PhotoMarks{.rating = 5, .label = ColorLabel::Purple});
    // The packet wrapper is still there.
    REQUIRE(slurp(sidecar).find("<?xpacket end=") != std::string::npos);
}

TEST_CASE("A child-element key is read, then replaced by an attribute", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), sidecar);

    CollectedDiagnostics log;
    const auto contents = readSidecar(path, log);
    REQUIRE(contents->state.settings.tone.exposure == 1.25F);
    REQUIRE(contents->state.settings.tone.contrast == 0.5F);
    REQUIRE(contents->marks == PhotoMarks{.rating = 3, .label = ColorLabel::Blue});
    // The one key this arraw does not know is reported, naming the file.
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingUnknown);
    REQUIRE(log.entries().front().subject == path);

    writeSidecar(photoOf(path, contents->state.settings, contents->marks));
    QDomDocument document = parsed(sidecar);
    REQUIRE(document.elementsByTagNameNS(QString::fromUtf8(arrawNs), "contrast").isEmpty());
    const auto after = meaning(sidecar);
    bool asAttribute = false;
    for (const auto& [key, value] : after) {
        asAttribute =
            asAttribute ||
            (key.ends_with("@{http://ns.arraw.org/develop/1.0/}contrast") && value == "0.5");
    }
    REQUIRE(asAttribute);
    REQUIRE(readSidecar(path)->state.settings == contents->state.settings);
}

TEST_CASE("A sidecar is named by the photograph's stem, unless a RAW shares it", "[sidecar]") {
    const test::TempDir directory;
    const fs::path raw = copyRaw(directory, "IMG_1.dng");

    SECTION("a RAW takes the stem") {
        REQUIRE(sidecarPath(raw) == directory.file("IMG_1.xmp"));
    }
    SECTION("a JPEG beside its RAW takes its whole name") {
        const fs::path jpeg = directory.file("IMG_1.JPG");
        spit(jpeg, "not really");
        REQUIRE(sidecarPath(jpeg) == directory.file("IMG_1.JPG.xmp"));
        REQUIRE(sidecarPath(raw) == directory.file("IMG_1.xmp"));
    }
    SECTION("a RAW LibRaw opens but the decoder does not claim by name is still a RAW") {
        const fs::path nrw = directory.file("IMG_5.NRW");
        const fs::path jpeg = directory.file("IMG_5.JPG");
        spit(nrw, "not really");
        spit(jpeg, "not really");
        REQUIRE(sidecarPath(nrw) == directory.file("IMG_5.xmp"));
        REQUIRE(sidecarPath(jpeg) == directory.file("IMG_5.JPG.xmp"));
    }
    SECTION("two RAWs of one stem each take their whole name") {
        const fs::path nrw = directory.file("IMG_1.nrw");
        spit(nrw, "not really");
        REQUIRE(sidecarPath(raw) == directory.file("IMG_1.dng.xmp"));
        REQUIRE(sidecarPath(nrw) == directory.file("IMG_1.nrw.xmp"));
    }
    SECTION("two images that are not RAWs each take their whole name") {
        const fs::path heic = directory.file("IMG_6.HEIC");
        const fs::path jpeg = directory.file("IMG_6.jpg");
        spit(heic, "not really");
        spit(jpeg, "not really");
        REQUIRE(sidecarPath(heic) == directory.file("IMG_6.HEIC.xmp"));
        REQUIRE(sidecarPath(jpeg) == directory.file("IMG_6.jpg.xmp"));
    }
    SECTION("a lone JPEG takes the stem") {
        const fs::path jpeg = directory.file("IMG_2.JPG");
        spit(jpeg, "not really");
        REQUIRE(sidecarPath(jpeg) == directory.file("IMG_2.xmp"));
    }
    SECTION("a file that does not exist is named all the same") {
        REQUIRE(sidecarPath(directory.file("nothing.png")) == directory.file("nothing.xmp"));
    }
    SECTION("a sidecar that differs in case is the one used") {
        spit(directory.file("IMG_1.XMP"), "<x/>");
        REQUIRE(sidecarPath(raw) == directory.file("IMG_1.XMP"));
    }
}

TEST_CASE("A pair with the same stem keeps two sidecars", "[sidecar]") {
    const test::TempDir directory;
    const fs::path raw = copyRaw(directory, "IMG_1.dng");
    const fs::path jpeg = directory.file("IMG_1.png");
    fs::copy_file(test::fixture("testcard-61x41-srgb8.png"), jpeg);

    writeSidecar(photoOf(raw, {.tone = {.exposure = 1.0F}}));
    writeSidecar(photoOf(jpeg, {.tone = {.exposure = -1.0F}}));
    REQUIRE(fs::exists(directory.file("IMG_1.xmp")));
    REQUIRE(fs::exists(directory.file("IMG_1.png.xmp")));
    REQUIRE(readSidecar(raw)->state.settings.tone.exposure == 1.0F);
    REQUIRE(readSidecar(jpeg)->state.settings.tone.exposure == -1.0F);
}

TEST_CASE("Writing edits a sidecar whose name differs only in case", "[sidecar]") {
    const test::TempDir directory;
    const fs::path raw = copyRaw(directory, "IMG_1.dng");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), directory.file("IMG_1.XMP"));
    writeSidecar(photoOf(raw, {}, {.rating = 1}));
    REQUIRE_FALSE(fs::exists(directory.file("IMG_1.xmp")));
    REQUIRE(readSidecar(raw)->marks.rating == 1);
}

TEST_CASE("Opening a photograph picks up its sidecar", "[sidecar][photo]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    spit(sidecar, R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" arraw:exposure="9" arraw:flipHorizontal="True"
    xmp:Rating="-1" xmp:Label="Yellow"/>
 </rdf:RDF>
</x:xmpmeta>)");

    CollectedDiagnostics log;
    const Photo photo = openPhoto(path, log);
    REQUIRE(photo.state().settings.tone.exposure == brightestExposure);
    REQUIRE(photo.state().settings.geometry.flipHorizontal);
    REQUIRE(photo.marks() == PhotoMarks{.rating = -1, .label = ColorLabel::Yellow});
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingClamped);
    REQUIRE(log.entries().front().subject == path);
}

TEST_CASE("A photograph without a sidecar opens as before", "[sidecar][photo]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    REQUIRE_FALSE(readSidecar(path));
    const Photo photo = openPhoto(path);
    REQUIRE(photo.state() == DevelopState{});
    REQUIRE(photo.marks() == PhotoMarks{});
    REQUIRE_FALSE(fs::exists(directory.file("IMG_1.xmp")));
}

TEST_CASE("Values a sidecar gets wrong are reported and repaired", "[sidecar][diagnostics]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const auto with = [&](const std::string& attributes) {
        spit(sidecar, R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" )" +
                          attributes + "/></rdf:RDF></x:xmpmeta>");
    };
    CollectedDiagnostics log;

    SECTION("a rating out of range is clamped") {
        with(R"(xmp:Rating="9")");
        REQUIRE(readSidecar(path, log)->marks.rating == 5);
        REQUIRE(count(log, Notice::SettingClamped) == 1);
        REQUIRE(log.entries().front().subject == path);
        with(R"(xmp:Rating="-4")");
        REQUIRE(readSidecar(path, log)->marks.rating == -1);
    }
    SECTION("a rating that is not an integer is ignored") {
        with(R"(xmp:Rating="lots")");
        REQUIRE(readSidecar(path, log)->marks.rating == 0);
        REQUIRE(count(log, Notice::SettingMalformed) == 1);
    }
    SECTION("an unknown label is ignored") {
        with(R"(xmp:Label="Mauve")");
        REQUIRE_FALSE(readSidecar(path, log)->marks.label);
        REQUIRE(count(log, Notice::SettingMalformed) == 1);
        REQUIRE(log.entries().front().subject == path);
    }
    SECTION("an empty label is no label") {
        with(R"(xmp:Label="")");
        REQUIRE_FALSE(readSidecar(path, log)->marks.label);
        REQUIRE(log.entries().empty());
    }
    SECTION("a malformed setting is skipped") {
        with(R"(arraw:exposure="bright" arraw:cropRectangle="1,2,3" arraw:contrast="0.25")");
        const auto contents = readSidecar(path, log);
        REQUIRE(contents->state.settings.tone.exposure == 0.0F);
        REQUIRE(contents->state.settings.tone.contrast == 0.25F);
        REQUIRE(count(log, Notice::SettingMalformed) == 2);
        REQUIRE(log.entries().front().subject == path);
    }
    SECTION("a newer version is read with a warning") {
        with(R"(arraw:version="2" arraw:exposure="0.5")");
        REQUIRE(readSidecar(path, log)->state.settings.tone.exposure == 0.5F);
        REQUIRE(count(log, Notice::NewerSettingsVersion) == 1);
        REQUIRE(log.entries().front().subject == path);
    }
}

TEST_CASE("A sidecar that is not XML is refused, and never overwritten", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const std::string junk = "<x:xmpmeta><unclosed>";
    spit(sidecar, junk);

    REQUIRE_THROWS_WITH(readSidecar(path), Catch::Matchers::ContainsSubstring("IMG_1.xmp"));
    REQUIRE_THROWS_AS(writeSidecar(Photo(path, readImageMetadata(path))), std::runtime_error);
    REQUIRE(slurp(sidecar) == junk);
}

TEST_CASE("A sidecar that is XML but not XMP is left alone", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const std::string other = "<notes>keep me</notes>";
    spit(sidecar, other);

    REQUIRE(readSidecar(path)->state.settings == DevelopSettings{});
    REQUIRE_THROWS_AS(writeSidecar(Photo(path, readImageMetadata(path))), std::runtime_error);
    REQUIRE(slurp(sidecar) == other);
}

TEST_CASE("A photograph refuses a rating outside -1 to 5", "[sidecar][marks]") {
    const Photo photo = openPhoto(test::fixture(rawFixture));
    REQUIRE_THROWS_AS(photo.with(PhotoMarks{.rating = 6}), std::invalid_argument);
    REQUIRE_THROWS_AS(photo.with(PhotoMarks{.rating = -2}), std::invalid_argument);
    REQUIRE(photo.with(PhotoMarks{.rating = 5}).marks().rating == 5);
    // Developing differently keeps the marks, and marking keeps the development.
    const Photo marked = photo.with(PhotoMarks{.rating = 2, .label = ColorLabel::Red});
    REQUIRE(marked.with(DevelopState{.settings = {.tone = {.exposure = 1.0F}}}).marks() ==
            marked.marks());
    REQUIRE(marked.state() == photo.state());
}

namespace {

constexpr std::string_view packetHead = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" )";

/// A sidecar holding one description with the given attributes.
std::string sidecarWith(const std::string& attributes) {
    return std::string(packetHead) + attributes + "/></rdf:RDF></x:xmpmeta>";
}

} // namespace

TEST_CASE("A mark that is not changed keeps the file's own text", "[sidecar][marks]") {
    /// Marks are standard XMP that other tools write too; a write touches one
    /// only when it differs from what reading the file gave.
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");

    SECTION("a localised label and an out-of-range rating come back as they were") {
        spit(sidecar, sidecarWith(R"(xmp:Label="Rot" xmp:Rating="9")"));
        const Photo photo = openPhoto(path);
        REQUIRE_FALSE(photo.marks().label);
        REQUIRE(photo.marks().rating == 5);
        writeSidecar(photo.with(DevelopState{.settings = {.tone = {.exposure = 1.0F}}}));
        const std::string text = slurp(sidecar);
        REQUIRE(text.find("xmp:Label=\"Rot\"") != std::string::npos);
        REQUIRE(text.find("xmp:Rating=\"9\"") != std::string::npos);
        // The document holds what it held; reading it again says the same.
        REQUIRE(openPhoto(path).marks() == photo.marks());
    }
    SECTION("a changed mark replaces the file's text") {
        spit(sidecar, sidecarWith(R"(xmp:Label="Rot" xmp:Rating="9")"));
        writeSidecar(photoOf(path, {}, {.rating = 2, .label = ColorLabel::Green}));
        const std::string text = slurp(sidecar);
        REQUIRE(text.find("xmp:Label=\"Green\"") != std::string::npos);
        REQUIRE(text.find("xmp:Rating=\"2\"") != std::string::npos);
        REQUIRE(text.find("Rot") == std::string::npos);
        REQUIRE(readSidecar(path)->marks == PhotoMarks{.rating = 2, .label = ColorLabel::Green});
    }
    SECTION("a label arraw can show replaces one it cannot, and clearing it removes it") {
        spit(sidecar, sidecarWith(R"(xmp:Label="Select")"));
        writeSidecar(photoOf(path, {}, {.label = ColorLabel::Red}));
        REQUIRE(slurp(sidecar).find("xmp:Label=\"Red\"") != std::string::npos);
        writeSidecar(photoOf(path, {}, {}));
        REQUIRE(slurp(sidecar).find("Label") == std::string::npos);
    }
    SECTION("a rating that is not a number is kept") {
        spit(sidecar, sidecarWith(R"(xmp:Rating="lots")"));
        writeSidecar(openPhoto(path));
        REQUIRE(slurp(sidecar).find("xmp:Rating=\"lots\"") != std::string::npos);
    }
    SECTION("default marks add nothing to a file that has none") {
        spit(sidecar, sidecarWith(R"(arraw:exposure="0.5")"));
        writeSidecar(openPhoto(path));
        const std::string text = slurp(sidecar);
        REQUIRE(text.find("Rating") == std::string::npos);
        REQUIRE(text.find("Label") == std::string::npos);
    }
    SECTION("a rating spelled as a real number is a rating") {
        spit(sidecar, sidecarWith(R"(xmp:Rating="3.0")"));
        CollectedDiagnostics log;
        REQUIRE(readSidecar(path, log)->marks == PhotoMarks{.rating = 3});
        REQUIRE(log.entries().empty());
        spit(sidecar, sidecarWith(R"(xmp:Rating="+2")"));
        REQUIRE(readSidecar(path, log)->marks == PhotoMarks{.rating = 2});
        REQUIRE(log.entries().empty());
        spit(sidecar, sidecarWith(R"(xmp:Rating="3.5")"));
        REQUIRE(readSidecar(path, log)->marks.rating == 0);
        REQUIRE(count(log, Notice::SettingMalformed) == 1);
    }
}

TEST_CASE("A signed number is read", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    spit(directory.file("IMG_1.xmp"), sidecarWith(R"(arraw:exposure="+0.5")"));
    CollectedDiagnostics log;
    REQUIRE(readSidecar(path, log)->state.settings.tone.exposure == 0.5F);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A sidecar of a newer version is read but never written over", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const std::string newer = sidecarWith(R"(arraw:version="2" arraw:exposure="99")");
    spit(sidecar, newer);

    CollectedDiagnostics log;
    const Photo photo = openPhoto(path, log);
    REQUIRE(count(log, Notice::NewerSettingsVersion) == 1);
    REQUIRE_THROWS_WITH(writeSidecar(photo), Catch::Matchers::ContainsSubstring("IMG_1.xmp"));
    REQUIRE(slurp(sidecar) == newer);
}

TEST_CASE("A sidecar in another encoding is written back as UTF-8, declared", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const std::string body = std::string(packetHead) +
                             "><dc:title xmlns:dc=\"http://purl.org/dc/"
                             "elements/1.1/\">%TITLE%</dc:title></rdf:Description></rdf:RDF>"
                             "</x:xmpmeta>";
    const auto withTitle = [&](const std::string& title) {
        std::string text = body;
        text.replace(text.find("%TITLE%"), 7, title);
        return text;
    };
    const std::string utf8Title = "\xC3\x96lm\xC3\xBChle";

    SECTION("Latin-1") {
        spit(sidecar,
             "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n" + withTitle("\xD6lm\xFChle"));
        writeSidecar(photoOf(path, {.tone = {.exposure = 1.0F}}));
    }
    SECTION("UTF-16") {
        const std::string declared =
            "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n" + withTitle("XXXXXXX");
        std::u16string wide = u"\uFEFF";
        for (const char character : declared) {
            wide += static_cast<char16_t>(character);
        }
        wide.replace(wide.find(u"XXXXXXX"), 7, u"\u00D6lm\u00FChle");
        std::ofstream(sidecar, std::ios::binary)
            .write(reinterpret_cast<const char*>(wide.data()),
                   static_cast<std::streamsize>(wide.size() * 2));
        writeSidecar(photoOf(path, {.tone = {.exposure = 1.0F}}));
    }
    const std::string text = slurp(sidecar);
    REQUIRE(text.find(utf8Title) != std::string::npos);
    REQUIRE(text.find("ISO-8859-1") == std::string::npos);
    REQUIRE(text.find("UTF-16") == std::string::npos);
    REQUIRE(text.find("UTF-8") != std::string::npos);
    REQUIRE(readSidecar(path)->state.settings.tone.exposure == 1.0F);
}

TEST_CASE("A sidecar that cannot be parsed does not stop a photograph opening",
          "[sidecar][photo]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    const std::string junk = "<x:xmpmeta><unclosed>";
    spit(sidecar, junk);

    CollectedDiagnostics log;
    const Photo photo = openPhoto(path, log);
    REQUIRE(photo.state() == DevelopState{});
    REQUIRE(photo.marks() == PhotoMarks{});
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SidecarUnreadable);
    REQUIRE(log.entries().front().severity == Severity::Error);
    REQUIRE(log.entries().front().subject == path);
    REQUIRE_THAT(describe(log.entries().front()), Catch::Matchers::ContainsSubstring("IMG_1.xmp"));
    REQUIRE_THROWS_AS(readSidecar(path), std::runtime_error);
    REQUIRE(slurp(sidecar) == junk);
}

TEST_CASE("A failed write leaves the old sidecar byte for byte", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), sidecar);
    const std::string before = slurp(sidecar);
    const Photo photo =
        openPhoto(path).with(DevelopState{.settings = {.tone = {.exposure = 1.0F}}});

    // A directory nobody can add to: the replacement cannot be made.
    fs::permissions(directory.path(), fs::perms::owner_read | fs::perms::owner_exec);
    struct Restore {
        fs::path directory;
        ~Restore() {
            fs::permissions(directory, fs::perms::owner_all);
        }
    } restore{directory.path()};
    {
        std::ofstream probe(directory.file("probe"));
        if (probe.is_open()) {
            SKIP("permissions are not enforced here, so a write cannot be made to fail");
        }
    }
    REQUIRE_THROWS_WITH(writeSidecar(photo), Catch::Matchers::ContainsSubstring("IMG_1.xmp"));
    REQUIRE(slurp(sidecar) == before);
}

TEST_CASE("A prefix already bound to the namespace is reused", "[sidecar]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    const fs::path sidecar = directory.file("IMG_1.xmp");
    spit(sidecar, R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:a="http://ns.arraw.org/develop/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" a:contrast="0.25"/>
 </rdf:RDF>
</x:xmpmeta>)");
    writeSidecar(photoOf(path, {.tone = {.exposure = 1.0F}}));
    const std::string text = slurp(sidecar);
    REQUIRE(text.find("xmlns:arraw") == std::string::npos);
    REQUIRE(text.find("a:exposure=") != std::string::npos);
    REQUIRE(readSidecar(path)->state.settings.tone.exposure == 1.0F);
}

TEST_CASE("A row that encodes to a compound is read through the shapes the codec names",
          "[sidecar][codec]") {
    std::size_t compounds = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        DevelopSettings settings;
        test::setNonDefault(descriptor, settings);
        const Encoded encoded = encode(descriptor, settings);
        const auto shapes = compoundShapes(descriptor);
        if (const auto* compound = std::get_if<Compound>(&encoded)) {
            INFO(descriptor.key);
            ++compounds;
            std::vector<std::string> names;
            for (const auto& [name, value] : *compound) {
                names.push_back(name);
            }
            REQUIRE(std::ranges::find(shapes, names) != shapes.end());
        }
    }
    REQUIRE(compounds >= 1);
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.key == "cropRectangle") {
            REQUIRE(compoundShapes(descriptor) ==
                    std::vector<std::vector<std::string>>{{"left", "top", "right", "bottom"}});
        } else if (descriptor.key == "cropAspect") {
            REQUIRE(compoundShapes(descriptor) == std::vector<std::vector<std::string>>{{"ratio"}});
        } else if (descriptor.key == "exposure") {
            REQUIRE(compoundShapes(descriptor).empty());
        }
    }
}

TEST_CASE("Writing only the marks keeps the settings and what is foreign", "[sidecar][marks]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    DevelopSettings settings;
    settings.tone.exposure = 0.5F;
    const fs::path sidecar = directory.file("IMG_1.xmp");
    fs::copy_file(test::fixture("sidecar-foreign.xmp"), sidecar);
    writeSidecar(photoOf(path, settings));
    const auto before = meaning(sidecar);

    writeSidecarMarks(path, {.rating = 4, .label = ColorLabel::Blue});

    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->marks == PhotoMarks{.rating = 4, .label = ColorLabel::Blue});
    REQUIRE(contents->state.settings == settings);
    const auto after = meaning(sidecar);
    for (const auto& [key, value] : before) {
        if (key.find("}Rating") != std::string::npos || key.find("}Label") != std::string::npos) {
            continue;
        }
        INFO(key);
        REQUIRE(after.contains(key));
        REQUIRE(after.at(key) == value);
    }
}

TEST_CASE("Writing only the marks creates a sidecar with default settings", "[sidecar][marks]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    REQUIRE_FALSE(fs::exists(directory.file("IMG_1.xmp")));
    writeSidecarMarks(path, {.rating = -1});
    const auto contents = readSidecar(path);
    REQUIRE(contents);
    REQUIRE(contents->marks == PhotoMarks{.rating = -1});
    REQUIRE(contents->state.settings == DevelopSettings{});
}

TEST_CASE("Writing only the marks refuses what writeSidecar refuses", "[sidecar][marks]") {
    const test::TempDir directory;
    const fs::path path = copyRaw(directory, "IMG_1.dng");
    spit(directory.file("IMG_1.xmp"), "this is not XML");
    REQUIRE_THROWS_AS(writeSidecarMarks(path, {.rating = 3}), std::runtime_error);
    REQUIRE(slurp(directory.file("IMG_1.xmp")) == "this is not XML");
    REQUIRE_THROWS_AS(writeSidecarMarks(path, {.rating = 6}), std::invalid_argument);
    REQUIRE_THROWS_AS(writeSidecarMarks(path, {.rating = -2}), std::invalid_argument);
}
