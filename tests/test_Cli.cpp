#include "Cli.h"
#include "Command.h"
#include "ExportCommand.h"

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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
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

TEST_CASE("The highlight roll-off reaches the exported pixels", "[cli]") {
    /// On by default and turned off by asking for none, which is the one thing
    /// about it a photographer can get wrong from the command line.
    const test::TempDir directory;
    const auto raw = test::fixture("linear-32x24-neutral.dng").string();
    const auto exported = directory.file("linear-32x24-neutral.png").string();

    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--bit-depth", "16", "--exposure", "2"})
                .code == cli::Success);
    const QImage rolled(QString::fromStdString(exported));

    REQUIRE(
        invoke({"export", raw, "-o", directory.path().string(), "--format", "png", "--bit-depth",
                "16", "--exposure", "2", "--filmic-highlights", "0", "--overwrite"})
            .code == cli::Success);
    const QImage clipped(QString::fromStdString(exported));

    REQUIRE_FALSE(rolled.isNull());
    REQUIRE_FALSE(clipped.isNull());

    /// Two stops up puts the upper half of the ramp above white. Clipped, they
    /// are all the same white; rolled, they are still telling apart.
    const auto clippedLeft = clipped.pixelColor(20, 12);
    const auto clippedRight = clipped.pixelColor(31, 12);
    REQUIRE(clippedLeft.greenF() == 1.0F);
    REQUIRE(clippedRight.greenF() == 1.0F);

    const auto rolledLeft = rolled.pixelColor(20, 12);
    const auto rolledRight = rolled.pixelColor(31, 12);
    REQUIRE(rolledLeft.greenF() < 1.0F);
    REQUIRE(rolledLeft.greenF() < rolledRight.greenF());
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
    const auto* flag = GENERATE("--exposure", "--temperature", "--tint", "--filmic-highlights");

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

TEST_CASE("Geometry options are documented and export successfully", "[cli]") {
    const test::TempDir directory;
    const auto options = GENERATE(
        std::vector<std::string>{"--rotate", "90"}, std::vector<std::string>{"--rotate", "0"},
        std::vector<std::string>{"--rotate", "180"}, std::vector<std::string>{"--rotate", "270"},
        std::vector<std::string>{"--rotate", "45"}, std::vector<std::string>{"--rotate", "90.5"},
        std::vector<std::string>{"--rotate", "-20"}, std::vector<std::string>{"--rotate", "730"},
        std::vector<std::string>{"--rotate", "1e300"},
        std::vector<std::string>{"--flip-horizontal"}, std::vector<std::string>{"--flip-vertical"},
        std::vector<std::string>{"--rotate", "-45"},
        std::vector<std::string>{"--crop", "0.1,0.2,0.8,0.9"},
        std::vector<std::string>{"--crop", "auto"},
        std::vector<std::string>{"--crop-aspect", "free"},
        std::vector<std::string>{"--crop-aspect", "original"},
        std::vector<std::string>{"--crop-aspect", "3:2"},
        std::vector<std::string>{"--crop-aspect", "2:3"});
    CAPTURE(options);
    const auto help = invoke({"export", "--help"});
    REQUIRE_THAT(help.out, ContainsSubstring(options.front()));

    std::vector<std::string> arguments{"export", test::fixture(card).string(), "-o",
                                       directory.path().string()};
    arguments.insert(arguments.end(), options.begin(), options.end());
    const auto result = invoke(arguments);
    REQUIRE(result.code == cli::Success);
    REQUIRE(result.out.empty());
    const QImage image(QString::fromStdString(directory.file("testcard-61x41-srgb8.jpg").string()));
    REQUIRE_FALSE(image.isNull());
}

TEST_CASE("Rotation angles split into equivalent quarter-turns and bounded straighten", "[cli]") {
    struct Example {
        double angle;
        QuarterTurn rotation;
        double straighten;
    };
    const Example examples[]{
        {0.0, QuarterTurn::None, 0.0},
        {100.0, QuarterTurn::Clockwise90, 10.0},
        {90.5, QuarterTurn::Clockwise90, 0.5},
        {-20.0, QuarterTurn::None, -20.0},
        {-100.0, QuarterTurn::Clockwise270, -10.0},
        {45.0, QuarterTurn::Clockwise90, -45.0},
        {-45.0, QuarterTurn::Clockwise270, 45.0},
        {135.0, QuarterTurn::Clockwise180, -45.0},
        {315.0, QuarterTurn::None, -45.0},
        {360.0, QuarterTurn::None, 0.0},
        {-360.0, QuarterTurn::None, 0.0},
        {730.0, QuarterTurn::None, 10.0},
    };
    for (const auto& example : examples) {
        CAPTURE(example.angle);
        GeometrySettings geometry;
        geometry.flipHorizontal = true;
        geometry.crop.rectangle = UprightCropRect{0.1, 0.2, 0.8, 0.9};
        geometry.crop.aspect = CropRatio{1.5};
        const auto crop = geometry.crop;
        cli::setRotationAngle(geometry, example.angle);
        REQUIRE(geometry.rotation == example.rotation);
        REQUIRE(geometry.straighten == example.straighten);
        REQUIRE(geometry.flipHorizontal);
        REQUIRE(geometry.crop == crop);
    }
    for (const double angle : {1e300, -1e300, 44.999, 45.001, -44.999, -45.001}) {
        CAPTURE(angle);
        GeometrySettings geometry;
        cli::setRotationAngle(geometry, angle);
        REQUIRE(geometry.straighten >= minimumStraighten);
        REQUIRE(geometry.straighten <= maximumStraighten);
        const double resolved = static_cast<int>(geometry.rotation) * 90.0 + geometry.straighten;
        REQUIRE(std::abs(std::remainder(resolved - std::fmod(angle, 360.0), 360.0)) < 1e-10);
    }
}

TEST_CASE("Nonfinite rotation angles leave geometry untouched", "[cli]") {
    GeometrySettings geometry;
    cli::setRotationAngle(geometry, 100.0);
    const auto original = geometry;
    for (const double angle :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        REQUIRE_THROWS_AS(cli::setRotationAngle(geometry, angle), std::invalid_argument);
        REQUIRE(geometry == original);
    }
}

TEST_CASE("Malformed geometry is rejected before exporting any files", "[cli]") {
    const test::TempDir directory;
    const auto options = GENERATE(std::vector<std::string>{"--rotate", "nan"},
                                  std::vector<std::string>{"--rotate", "inf"},
                                  std::vector<std::string>{"--rotate", "1e999"},
                                  std::vector<std::string>{"--rotate", "clockwise"},
                                  std::vector<std::string>{"--crop", "0,0,1"},
                                  std::vector<std::string>{"--crop", "0,0,1,1,1"},
                                  std::vector<std::string>{"--crop", "0,0,0,1"},
                                  std::vector<std::string>{"--crop", "0,0.9,1,0.1"},
                                  std::vector<std::string>{"--crop", "-0.1,0,1,1"},
                                  std::vector<std::string>{"--crop", "0,0,1.1,1"},
                                  std::vector<std::string>{"--crop", "0,,1,1"},
                                  std::vector<std::string>{"--crop", "nan,0,1,1"},
                                  std::vector<std::string>{"--crop-aspect", "3:0"},
                                  std::vector<std::string>{"--crop-aspect", "-3:2"},
                                  std::vector<std::string>{"--crop-aspect", "nan:2"},
                                  std::vector<std::string>{"--crop-aspect", "3:inf"},
                                  std::vector<std::string>{"--crop-aspect", "1e308:1e-308"},
                                  std::vector<std::string>{"--crop-aspect", "1e-308:1e308"},
                                  std::vector<std::string>{"--crop-aspect", "3:2:1"},
                                  std::vector<std::string>{"--crop-aspect", "square"});
    CAPTURE(options);
    std::vector<std::string> arguments{"export", test::fixture(card).string(), "-o",
                                       directory.path().string()};
    arguments.insert(arguments.end(), options.begin(), options.end());
    const auto result = invoke(arguments);
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring(options.front()));
    REQUIRE_THAT(result.err, !ContainsSubstring("not implemented yet"));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("Explicit white-balance modes preserve the existing defaults", "[cli]") {
    const test::TempDir directory;
    const auto mode = GENERATE(std::string{"as-shot"}, std::string{"custom"});
    const auto raw = test::fixture("linear-32x24-neutral.dng").string();
    const auto output = directory.file("linear-32x24-neutral.png");
    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png"}).code ==
            cli::Success);
    const QImage baseline(QString::fromStdString(output.string()));
    REQUIRE_FALSE(baseline.isNull());
    REQUIRE(invoke({"export", raw, "-o", directory.path().string(), "--format", "png",
                    "--white-balance", mode, "--overwrite"})
                .code == cli::Success);
    const QImage explicitMode(QString::fromStdString(output.string()));
    REQUIRE(explicitMode == baseline);
}

TEST_CASE("Conflicting or unknown white-balance modes are usage errors", "[cli]") {
    const test::TempDir directory;
    const auto options =
        GENERATE(std::vector<std::string>{"--white-balance", "daylight"},
                 std::vector<std::string>{"--white-balance", "as-shot", "--temperature", "5500"},
                 std::vector<std::string>{"--tint", "10", "--white-balance", "as-shot"});
    std::vector<std::string> arguments{"export", test::fixture(card).string(), "-o",
                                       directory.path().string()};
    arguments.insert(arguments.end(), options.begin(), options.end());
    const auto result = invoke(arguments);
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("--white-balance"));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("Nonfinite develop numbers are usage errors", "[cli]") {
    const test::TempDir directory;
    const auto* flag = GENERATE("--exposure", "--contrast", "--shadows", "--highlights", "--blacks",
                                "--whites", "--temperature", "--tint", "--filmic-highlights");
    const auto* value = GENERATE("nan", "inf", "-inf");
    CAPTURE(flag, value);
    const auto result = invoke(
        {"export", test::fixture(card).string(), "-o", directory.path().string(), flag, value});
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring(flag));
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
        const auto object = parsed.object();
        REQUIRE(object.contains("notice"));
        // A diagnostic about no particular photograph names none: the summary
        // has no file, every other line has one.
        REQUIRE(object.contains("file") == (object["notice"] != "batch_finished"));
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
