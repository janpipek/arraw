#include "ExportPresetsJson.h"
#include "support/Fixtures.h"

#include <Diagnostics.h>
#include <ExportPresets.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace arraw;
namespace fs = std::filesystem;

namespace {

/// @brief Reads a whole file.
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Wraps entries in a version 1 file.
std::string fileOf(const std::string& entries) {
    return R"({"arraw": 1, "exportPresets": [)" + entries + "]}";
}

/// @brief Reads text with a collecting log.
ParsedPresets parse(const std::string& text, CollectedDiagnostics& log) {
    return readExportPresets(text, log, fs::path("export-presets.json"));
}

/// @brief Counts the log's entries of a notice.
std::size_t count(const CollectedDiagnostics& log, Notice notice) {
    return static_cast<std::size_t>(std::ranges::count_if(
        log.entries(), [&](const Diagnostic& d) { return d.notice == notice; }));
}

} // namespace

TEST_CASE("A catalog written and read back is the same, in name order", "[presets][json]") {
    ParsedPresets parsed;
    const ExportPreset web{"web", {.resize = true, .width = 2048, .height = 2048, .quality = 85}};
    const ExportPreset print{
        "Print",
        {.format = ImageFileFormat::Tiff, .encoding = NamedEncoding::AdobeRgb, .sixteenBit = true}};
    const ExportPreset apple{"apple", {.format = ImageFileFormat::Png, .location = true}};
    for (const ExportPreset* preset : {&web, &print, &apple}) {
        parsed.entries.push_back(presetEntry(*preset, nullptr));
    }

    const std::string text = writeExportPresets(parsed);
    CollectedDiagnostics log;
    const ExportPresetCatalog catalog = parse(text, log).catalog();

    CHECK(log.entries().empty());
    REQUIRE(catalog.presets.size() == 3);
    CHECK(catalog.presets[0] == apple);
    CHECK(catalog.presets[1] == print);
    CHECK(catalog.presets[2] == web);
    CHECK(catalog.state == ExportPresetFileState::Read);
    CHECK(catalog.version == exportPresetsVersion);
}

TEST_CASE("Formats and encodings are written as tokens", "[presets][json]") {
    ParsedPresets parsed;
    parsed.entries.push_back(presetEntry(
        ExportPreset{"p", {.format = ImageFileFormat::Tiff, .encoding = NamedEncoding::DisplayP3}},
        nullptr));
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(writeExportPresets(parsed)));
    const QJsonObject entry = document.object().value("exportPresets").toArray().at(0).toObject();
    CHECK(entry.value("format") == "tiff");
    CHECK(entry.value("encoding") == "display-p3");
    CHECK(entry.value("bitDepth") == 8);
    CHECK(entry.value("size").isNull());
    CHECK(document.object().value("arraw") == exportPresetsVersion);

    const std::string alone = exportPresetToJson(ExportPreset{"p", {}});
    CHECK_THAT(alone, Catch::Matchers::ContainsSubstring("\"format\": \"jpeg\""));
    CHECK(alone.back() == '\n');
}

TEST_CASE("An absent key is the version 1 default", "[presets][json]") {
    CollectedDiagnostics log;
    const auto catalog = parse(fileOf(R"({"name": "Bare"})"), log).catalog();
    CHECK(log.entries().empty());
    REQUIRE(catalog.presets.size() == 1);
    CHECK(catalog.presets[0].settings == ExportSettings{});
    // Frozen: these numbers are version 1's, whatever the struct's defaults later become.
    CHECK(catalog.presets[0].settings.quality == 90);
    CHECK(catalog.presets[0].settings.sharpening == 0);
    CHECK(catalog.presets[0].settings.captureInfo);
    CHECK_FALSE(catalog.presets[0].settings.location);
    CHECK(catalog.presets[0].settings.descriptive);
    CHECK(catalog.presets[0].settings.format == ImageFileFormat::Jpeg);
    CHECK(catalog.presets[0].settings.encoding == NamedEncoding::Srgb);
}

TEST_CASE("A number out of range is clamped with a warning", "[presets][json]") {
    CollectedDiagnostics log;
    const auto catalog = parse(fileOf(R"({"name": "Wild", "quality": 120, "sharpening": -5,
                         "size": {"width": 0, "height": 200000}})"),
                               log)
                             .catalog();
    REQUIRE(catalog.presets.size() == 1);
    const ExportSettings& s = catalog.presets[0].settings;
    CHECK(s.quality == 100);
    CHECK(s.sharpening == 0);
    CHECK(s.resize);
    CHECK(s.width == 1);
    CHECK(s.height == 99999);
    CHECK(count(log, Notice::SettingClamped) == 4);
    CHECK(log.entries()[0].subject == fs::path("export-presets.json"));
    CHECK_NOTHROW(validate(s));
}

TEST_CASE("A value of the wrong type takes the default with a warning", "[presets][json]") {
    CollectedDiagnostics log;
    const auto catalog = parse(fileOf(R"({"name": "Odd", "quality": "high", "allowEnlarging": 1,
                         "metadata": {"location": "yes"}, "format": 3})"),
                               log)
                             .catalog();
    REQUIRE(catalog.presets.size() == 1);
    CHECK(catalog.presets[0].settings == ExportSettings{});
    CHECK(count(log, Notice::SettingMalformed) == 4);
}

TEST_CASE("An unknown key is reported and kept on write", "[presets][json]") {
    CollectedDiagnostics log;
    ParsedPresets parsed = parse(
        R"({"arraw": 1, "future": {"a": 1}, "exportPresets": [
            {"name": "X", "watermark": "yes", "size": {"width": 10, "height": 20, "dpi": 300},
             "metadata": {"faces": true}}]})",
        log);
    CHECK(count(log, Notice::SettingUnknown) == 4);
    std::vector<std::string> keys;
    for (const Diagnostic& d : log.entries()) {
        keys.push_back(std::get<std::string>(d.values.at(0)));
    }
    CHECK(std::ranges::find(keys, "future") != keys.end());
    CHECK(std::ranges::find(keys, "X.watermark") != keys.end());
    CHECK(std::ranges::find(keys, "X.size.dpi") != keys.end());
    CHECK(std::ranges::find(keys, "X.metadata.faces") != keys.end());

    // Replace the preset through presetEntry, as the store does.
    parsed.entries[0] = presetEntry(ExportPreset{"X", {.quality = 50}}, &parsed.entries[0]);
    const QJsonObject root =
        QJsonDocument::fromJson(QByteArray::fromStdString(writeExportPresets(parsed))).object();
    CHECK(root.value("future").toObject().value("a") == 1);
    const QJsonObject entry = root.value("exportPresets").toArray().at(0).toObject();
    CHECK(entry.value("watermark") == "yes");
    CHECK(entry.value("quality") == 50);
    CHECK(entry.value("size").isNull());
    CHECK(entry.value("metadata").toObject().value("faces") == true);
}

TEST_CASE("An unknown format, bit depth or size shape makes the preset unusable, kept verbatim",
          "[presets][json]") {
    CollectedDiagnostics log;
    const std::string avif = R"({"name": "Future", "format": "avif", "extra": [1, 2]})";
    ParsedPresets parsed = parse(fileOf(avif + R"(, {"name": "Deep", "bitDepth": 32},
                {"name": "Shape", "size": {"longEdge": 2048}},
                {"name": "Shape2", "size": "big"}, {"name": "Colour", "encoding": "rec2020"},
                {"name": "Fine"})"),
                                 log);
    const auto catalog = parsed.catalog();
    REQUIRE(catalog.presets.size() == 1);
    CHECK(catalog.presets[0].name == "Fine");
    REQUIRE(catalog.unusable.size() == 5);
    CHECK(count(log, Notice::ExportPresetUnusable) == 5);
    for (const auto& unusable : catalog.unusable) {
        CHECK_FALSE(unusable.reason.empty());
    }

    const QJsonObject root =
        QJsonDocument::fromJson(QByteArray::fromStdString(writeExportPresets(parsed))).object();
    bool found = false;
    for (const QJsonValue value : root.value("exportPresets").toArray()) {
        if (value.toObject().value("name") == "Future") {
            found = true;
            CHECK(value.toObject().value("format") == "avif");
            CHECK(value.toObject().value("extra").toArray().size() == 2);
            CHECK(value.toObject().size() == 3);
        }
    }
    CHECK(found);
}

TEST_CASE("Names that fold alike: the first is used, the rest are unusable", "[presets][json]") {
    CollectedDiagnostics log;
    const auto catalog =
        parse(fileOf(R"({"name": "Web", "quality": 10}, {"name": "WEB", "quality": 20},
                        {"name": " web "})"),
              log)
            .catalog();
    REQUIRE(catalog.presets.size() == 1);
    CHECK(catalog.presets[0].name == "Web");
    CHECK(catalog.presets[0].settings.quality == 10);
    REQUIRE(catalog.unusable.size() == 2);
    CHECK(catalog.unusable[0].reason == "duplicate name");
}

TEST_CASE("An invalid name makes the entry unusable", "[presets][json]") {
    CollectedDiagnostics log;
    const std::string tooLong(65, 'x');
    const auto catalog = parse(fileOf(R"({"quality": 1}, {"name": ""}, {"name": "   "}, {"name": 5},
                        {"name": "a\u0007b"}, {"name": "a b"}, {"name": ")" +
                                      tooLong + R"("}, "text", {"name": "Fine"})"),
                               log)
                             .catalog();
    REQUIRE(catalog.presets.size() == 1);
    CHECK(catalog.unusable.size() == 8);
    CHECK(count(log, Notice::ExportPresetUnusable) == 8);

    CHECK(normalizedPresetName("  Web  ") == "Web");
    CHECK(normalizedPresetName(std::string(64, 'x')).has_value());
    CHECK_FALSE(normalizedPresetName(tooLong));
    CHECK_FALSE(normalizedPresetName(""));
    // NFD in, NFC out.
    CHECK(normalizedPresetName("Cafe\xCC\x81") == "Caf\xC3\xA9");
    CHECK(presetNamesEqual("Caf\xC3\xA9", "CAFE\xCC\x81"));
    CHECK(presetNamesEqual("Caf\xC3\xA9", "CAF\xC3\x89"));
    CHECK(presetNamesEqual("Web", "web"));
    CHECK_FALSE(presetNamesEqual("", ""));
    // Simple folding only: the sharp s and SS stay apart.
    CHECK_FALSE(presetNamesEqual("Stra\xC3\x9F"
                                 "e",
                                 "STRASSE"));
}

TEST_CASE("A newer file is read with a warning", "[presets][json]") {
    CollectedDiagnostics log;
    const ParsedPresets parsed =
        parse(R"({"arraw": 2, "exportPresets": [{"name": "A", "quality": 40}]})", log);
    CHECK(parsed.state == ExportPresetFileState::Newer);
    CHECK(parsed.version == 2);
    CHECK(count(log, Notice::NewerSettingsVersion) == 1);
    REQUIRE(parsed.catalog().presets.size() == 1);
    CHECK(parsed.catalog().presets[0].settings.quality == 40);
}

TEST_CASE("Not JSON, the wrong shape, another topic, or over 1 MiB is unreadable",
          "[presets][json]") {
    const std::string big = fileOf("") + std::string(maxPresetsFileSize, ' ') + "x";
    for (const std::string& text :
         {std::string("not json"), std::string("[]"), std::string(R"({"exportPresets": []})"),
          std::string(R"({"arraw": 0, "exportPresets": []})"),
          std::string(R"({"arraw": 1.5, "exportPresets": []})"),
          std::string(R"({"arraw": "1", "exportPresets": []})"),
          std::string(R"({"arraw": 1, "exportPresets": {}})"),
          std::string(R"({"arraw": 1, "developPresets": []})"), big}) {
        INFO(text.substr(0, 60));
        CollectedDiagnostics log;
        const ParsedPresets parsed = parse(text, log);
        CHECK(parsed.state == ExportPresetFileState::Unreadable);
        CHECK_FALSE(parsed.problem.empty());
        CHECK(parsed.catalog().presets.empty());
        CHECK(count(log, Notice::ExportPresetsUnreadable) == 1);
        CHECK(parsed.catalog().problem == parsed.problem);
    }
}

TEST_CASE("An empty or blank file is an empty catalog", "[presets][json]") {
    for (const std::string& text : {std::string(), std::string("  \n\t ")}) {
        CollectedDiagnostics log;
        const ParsedPresets parsed = parse(text, log);
        CHECK(parsed.state == ExportPresetFileState::Read);
        CHECK(parsed.entries.empty());
        CHECK(log.entries().empty());
    }
}

TEST_CASE("The presets notices have sentences", "[presets][json][diagnostics]") {
    const Diagnostic unusable{.notice = Notice::ExportPresetUnusable,
                              .values = {std::string("Future"), std::string("unknown format")}};
    CHECK_THAT(describe(unusable), Catch::Matchers::ContainsSubstring("'Future'"));
    CHECK_THAT(describe(unusable), Catch::Matchers::ContainsSubstring("unknown format"));
    const Diagnostic bad{.notice = Notice::ExportPresetsUnreadable,
                         .values = {std::string("it is not JSON")}};
    CHECK_THAT(describe(bad), Catch::Matchers::ContainsSubstring("it is not JSON"));
}

TEST_CASE("Every stored version of the presets file is still read, and written as the current one",
          "[presets][json][versions]") {
    // Fixtures of a version are frozen: a change that breaks reading one is a breaking change, so
    // the version is raised and a directory for it added (ADR 054), never a fixture edited.
    const fs::path root = test::fixture("config/export-presets");
    REQUIRE(fs::is_directory(root / ("v" + std::to_string(exportPresetsVersion))));

    std::size_t files = 0;
    for (const auto& versionDirectory : fs::directory_iterator(root)) {
        const std::string name = versionDirectory.path().filename().string();
        if (!versionDirectory.is_directory() || !name.starts_with('v')) {
            continue;
        }
        REQUIRE(std::stoi(name.substr(1)) <= exportPresetsVersion);
        for (const auto& stored : fs::directory_iterator(versionDirectory.path())) {
            if (stored.path().extension() != ".json") {
                continue;
            }
            ++files;
            INFO(stored.path().string());
            const std::string text = slurp(stored.path());
            CollectedDiagnostics log;
            const ParsedPresets parsed = parse(text, log);
            CHECK(log.entries().empty());
            CHECK(parsed.state == ExportPresetFileState::Read);
            REQUIRE_FALSE(parsed.catalog().presets.empty());
            CHECK(parsed.catalog().unusable.empty());

            const std::string rewritten = writeExportPresets(parsed);
            const QJsonObject written =
                QJsonDocument::fromJson(QByteArray::fromStdString(rewritten)).object();
            CHECK(written.value("arraw") == exportPresetsVersion);
            CollectedDiagnostics again;
            const auto reread = parse(rewritten, again);
            CHECK(again.entries().empty());
            CHECK(reread.catalog().presets == parsed.catalog().presets);
            if (stored.path().filename() == "complete.json") {
                // Every key is there, so nothing is added or lost.
                CHECK(written == QJsonDocument::fromJson(QByteArray::fromStdString(text)).object());
                CHECK(parsed.catalog().presets.size() == 3);
            }
        }
    }
    CHECK(files >= 2);
}
