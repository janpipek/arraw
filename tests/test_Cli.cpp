#include "Cli.h"
#include "Command.h"

#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <QByteArray>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// Tests for the command line's contract: what it writes to which stream, and
/// what it returns. In-process rather than through the real binary, because
/// the seam under test is arguments-to-exit-code and cli::run is exactly that
/// seam; see ADR 006.

namespace {

/// @brief One invocation's observable result.
struct Invocation {
    int code = 0;
    std::string out;
    std::string err;
};

/// @brief Runs the command line with both streams captured.
Invocation invoke(const std::vector<std::string>& arguments) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run(arguments, out, err);
    return {code, out.str(), err.str()};
}

/// @brief Writes a file that is not an image, to fail a decode on purpose.
std::filesystem::path writeRubbish(const test::TempDir& directory, std::string_view name) {
    const auto path = directory.file(name);
    std::ofstream stream(path, std::ios::binary);
    stream << "not an image";
    return path;
}

constexpr std::string_view card = "testcard-61x41-srgb8.png";

} // namespace

TEST_CASE("The top-level help lists every command, on stdout", "[cli]") {
    const auto requested = GENERATE(std::string{"--help"}, std::string{"-h"}, std::string{"help"});
    const auto result = invoke({requested});

    /// Asked for outright, so `arraw-cli --help | less` must work. Usage
    /// printed *at* a mistake goes to stderr instead, which the cases below
    /// pin down.
    REQUIRE(result.code == cli::Success);
    REQUIRE(result.err.empty());

    /// Generated from the same table dispatch uses, so the help cannot
    /// advertise a command that does not exist nor hide one that does.
    for (const auto& command : cli::commands()) {
        CAPTURE(command.name);
        REQUIRE_THAT(result.out, ContainsSubstring(std::string(command.name)));
    }
    REQUIRE_THAT(result.out, ContainsSubstring("not implemented yet"));
    /// A command's own options belong to its own help, not to this one.
    REQUIRE_THAT(result.out, !ContainsSubstring("--overwrite"));
}

TEST_CASE("A command's help carries its own options", "[cli]") {
    const auto asked = GENERATE(std::vector<std::string>{"export", "--help"},
                                std::vector<std::string>{"help", "export"});
    CAPTURE(asked);
    const auto result = invoke(asked);

    /// `help <command>` and `<command> --help` are the same question, so they
    /// are answered by the same parser rather than by two texts that can drift.
    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("--overwrite"));
    REQUIRE_THAT(result.out, ContainsSubstring("export <input>..."));
    REQUIRE(result.err.empty());
}

TEST_CASE("A reserved command says it is coming, not that it is unknown", "[cli]") {
    const auto reserved = GENERATE(std::string{"info"}, std::string{"preset"});
    CAPTURE(reserved);

    const auto* command = cli::findCommand(reserved);
    REQUIRE(command != nullptr);
    REQUIRE(command->run == nullptr);

    const auto result = invoke({reserved, "photo.arw"});

    /// Named in desired-features.md, so someone typing it followed the
    /// documentation rather than mistyping, and deserves a different answer
    /// from the unknown-command case below.
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("not implemented yet"));
    REQUIRE_THAT(result.err, !ContainsSubstring("unknown command"));
    REQUIRE(result.out.empty());
}

TEST_CASE("An unknown command is named and the real ones listed", "[cli]") {
    const auto result = invoke({"develop", "photo.arw"});

    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("unknown command 'develop'"));
    REQUIRE_THAT(result.err, ContainsSubstring("export"));
    REQUIRE(result.out.empty());
}

TEST_CASE("The version carries the licence notice, on stdout", "[cli]") {
    const auto requested = GENERATE(std::string{"--version"}, std::string{"-v"});
    const auto result = invoke({requested});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("arraw-cli "));
    /// The GPL asks that a program be able to show this. README.md and LICENSE
    /// are authoritative; this is for a user holding only the binary.
    REQUIRE_THAT(result.out, ContainsSubstring("GPL-3.0-or-later"));
    REQUIRE_THAT(result.out, ContainsSubstring("NO WARRANTY"));
    REQUIRE(result.err.empty());
}

TEST_CASE("A command line that is wrong exits 2 and says so on stderr", "[cli]") {
    const test::TempDir directory;
    const auto image = test::fixture(card).string();
    const auto output = directory.path().string();

    const auto arguments = GENERATE_REF(
        std::vector<std::string>{}, std::vector<std::string>{"develop", image},
        std::vector<std::string>{"export"}, std::vector<std::string>{"export", image},
        std::vector<std::string>{"export", image, "-o", "/no/such/directory"},
        std::vector<std::string>{"export", image, "-o", image},
        std::vector<std::string>{"export", image, "-o", output, "--format", "gif"},
        std::vector<std::string>{"export", image, "-o", output, "--encoding", "rec2020"},
        std::vector<std::string>{"export", image, "-o", output, "--quality", "high"},
        std::vector<std::string>{"export", image, "-o", output, "--nonsense"});
    CAPTURE(arguments);

    const auto result = invoke(arguments);

    /// Usage errors are 2, never 1: a script that retries on a failed export
    /// would otherwise loop forever on a mistyped flag.
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_FALSE(result.err.empty());
    REQUIRE(result.out.empty());
}

TEST_CASE("An export writes the file and leaves stdout empty", "[cli]") {
    const test::TempDir directory;

    const auto result =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE(std::filesystem::exists(directory.file("testcard-61x41-srgb8.jpg")));
    /// The channel stays free for machine-readable output later. If progress
    /// went here now, that door would close.
    REQUIRE(result.out.empty());
    REQUIRE_THAT(result.err, ContainsSubstring("testcard-61x41-srgb8.jpg"));
}

TEST_CASE("A RAW exports through the same command", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture("linear-32x24-neutral.dng").string(), "-o",
                                directory.path().string(), "--format", "png", "--bit-depth", "16"});

    REQUIRE(result.code == cli::Success);
    REQUIRE(std::filesystem::file_size(directory.file("linear-32x24-neutral.png")) > 0);
}

TEST_CASE("Quiet suppresses the per-file report but not errors", "[cli]") {
    const test::TempDir directory;

    const auto quiet = invoke(
        {"export", test::fixture(card).string(), "-o", directory.path().string(), "--quiet"});
    REQUIRE(quiet.code == cli::Success);
    REQUIRE(quiet.err.empty());

    const auto broken = invoke({"export", writeRubbish(directory, "junk.png").string(), "-o",
                                directory.path().string(), "--quiet"});
    REQUIRE(broken.code == cli::Failed);
    REQUIRE_FALSE(broken.err.empty());
}

TEST_CASE("One bad input does not abandon the batch", "[cli]") {
    const test::TempDir inputs;
    const test::TempDir outputs;
    const auto rubbish = writeRubbish(inputs, "broken.png");

    const auto result = invoke(
        {"export", rubbish.string(), test::fixture(card).string(), "-o", outputs.path().string()});

    /// The failing input comes first on purpose: an overnight batch must not be
    /// abandoned by the frame that broke, and the good file behind it must
    /// still be written.
    REQUIRE(result.code == cli::Failed);
    REQUIRE(std::filesystem::exists(outputs.file("testcard-61x41-srgb8.jpg")));
    REQUIRE_THAT(result.err, ContainsSubstring("1 of 2 failed"));
}

TEST_CASE("An existing output is refused unless overwriting is asked for", "[cli]") {
    const test::TempDir directory;
    const auto destination = directory.file("testcard-61x41-srgb8.jpg");
    {
        std::ofstream stream(destination, std::ios::binary);
        stream << "an earlier export";
    }
    const auto original = std::filesystem::file_size(destination);

    const std::vector<std::string> command{"export", test::fixture(card).string(), "-o",
                                           directory.path().string()};

    /// exportImage replaces a destination without a word, which is right for a
    /// library and wrong for a batch aimed at a folder of someone's negatives.
    const auto refused = invoke(command);
    REQUIRE(refused.code == cli::Failed);
    REQUIRE(std::filesystem::file_size(destination) == original);
    REQUIRE_THAT(refused.err, ContainsSubstring("--overwrite"));

    auto overwriting = command;
    overwriting.emplace_back("--overwrite");
    const auto replaced = invoke(overwriting);
    REQUIRE(replaced.code == cli::Success);
    REQUIRE(std::filesystem::file_size(destination) != original);
}

TEST_CASE("Exposure reaches the exported pixels", "[cli]") {
    const test::TempDir directory;
    const auto raw = test::fixture("linear-32x24-neutral.dng").string();

    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--bit-depth", "16"})
                .code == cli::Success);
    const QImage flat(QString::fromStdString(directory.file("linear-32x24-neutral.png").string()));

    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--bit-depth", "16", "--exposure", "-1", "--overwrite"})
                .code == cli::Success);
    const QImage darker(
        QString::fromStdString(directory.file("linear-32x24-neutral.png").string()));

    REQUIRE_FALSE(flat.isNull());
    REQUIRE_FALSE(darker.isNull());

    /// A stop down is half the light. The output is encoded rather than linear,
    /// so the assertion is the direction and that it is a large move, not a
    /// ratio -- the ratio belongs to the tests that can see linear values.
    const auto before = flat.pixelColor(24, 12);
    const auto after = darker.pixelColor(24, 12);
    REQUIRE(after.greenF() < before.greenF() * 0.8F);
}

TEST_CASE("White balance reaches the exported pixels", "[cli]") {
    const test::TempDir directory;
    const auto raw = test::fixture("linear-32x24-neutral.dng").string();

    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--temperature", "9000"})
                .code == cli::Success);
    const QImage cool(QString::fromStdString(directory.file("linear-32x24-neutral.png").string()));

    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--temperature", "3000", "--overwrite"})
                .code == cli::Success);
    const QImage warm(QString::fromStdString(directory.file("linear-32x24-neutral.png").string()));

    /// Naming a warm light takes red back out, so the picture cools.
    const auto coolPixel = cool.pixelColor(24, 12);
    const auto warmPixel = warm.pixelColor(24, 12);
    REQUIRE(warmPixel.redF() < coolPixel.redF());
    REQUIRE(warmPixel.blueF() > coolPixel.blueF());
}

TEST_CASE("A develop setting outside its range is a usage error", "[cli]") {
    const test::TempDir directory;
    const auto raw = test::fixture("linear-32x24-neutral.dng").string();
    const auto* flag = GENERATE("--exposure", "--temperature", "--tint");

    const auto tooBig = invoke({"export", raw, "-o", directory.path().string(), flag, "1000000"});
    const auto notANumber =
        invoke({"export", raw, "-o", directory.path().string(), flag, "quite bright"});

    /// Refused before any file is touched, rather than clamped: the person who
    /// typed it is present to be told (ADR 008).
    REQUIRE(tooBig.code == cli::UsageError);
    REQUIRE(notANumber.code == cli::UsageError);
    REQUIRE_THAT(tooBig.err, ContainsSubstring(flag));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("A temperature on a photograph with no sensor fails that file", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture("testcard-61x41-srgb8.png").string(), "-o",
                                directory.path().string(), "--temperature", "5000"});

    /// A JPEG or PNG has no sensor to measure kelvin against, so the file is
    /// reported and the batch's exit status says something failed.
    REQUIRE(result.code == cli::Failed);
    REQUIRE_THAT(result.err, ContainsSubstring("sensor"));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("A substituted white balance is reported per file", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture("linear-32x24-nowb.dng").string(), "-o",
                                directory.path().string(), "--format", "png"});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.err, ContainsSubstring("warning:"));
    REQUIRE_THAT(result.err, ContainsSubstring("linear-32x24-nowb.dng"));
    REQUIRE_THAT(result.err, ContainsSubstring("no white balance"));
}

TEST_CASE("Quiet keeps the warnings and drops the commentary", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture("linear-32x24-nowb.dng").string(), "-o",
                                directory.path().string(), "--format", "png", "--quiet"});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.err, ContainsSubstring("warning:"));
    REQUIRE_THAT(result.err, !ContainsSubstring("written to"));
}

TEST_CASE("A JSON log is one object per line, and nothing else", "[cli]") {
    /// The point of the format: whatever reads an overnight batch afterwards
    /// should not have to skip a summary line that is not JSON.
    const test::TempDir directory;

    const auto result =
        invoke({"export", test::fixture("linear-32x24-nowb.dng").string(),
                (directory.path() / "absent.dng").string(), "-o", directory.path().string(),
                "--format", "png", "--log-format", "json"});

    REQUIRE(result.code == cli::Failed);
    std::istringstream lines(result.err);
    std::string line;
    std::size_t objects = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        REQUIRE(parsed.isObject());
        REQUIRE(parsed.object().contains("notice"));
        ++objects;
    }
    REQUIRE(objects == 4); // the warning, the export, the failure, the summary
}

TEST_CASE("An unknown log format is a usage error", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--log-format", "yaml"});

    REQUIRE(result.code == cli::UsageError);
}
