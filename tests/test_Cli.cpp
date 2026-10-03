#include "Cli.h"
#include "Command.h"
#include "DeviceChoice.h"
#include "ExportCommand.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <Photo.h>
#include <Sidecar.h>

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QtGlobal>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

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
    /// @brief Applications the command asked for, in order; the suite is
    /// already inside one, so none is started.
    std::vector<cli::ApplicationKind> started;
};

/// @brief Runs the command line with both streams captured.
Invocation invoke(const std::vector<std::string>& arguments) {
    std::ostringstream out;
    std::ostringstream err;
    std::vector<cli::ApplicationKind> started;
    const int code = cli::run(arguments, out, err,
                              [&started](cli::ApplicationKind kind) { started.push_back(kind); });
    return {code, out.str(), err.str(), std::move(started)};
}

/// @brief Writes a file that is not an image, to fail a decode on purpose.
std::filesystem::path writeRubbish(const test::TempDir& directory, std::string_view name) {
    const auto path = directory.file(name);
    std::ofstream stream(path, std::ios::binary);
    stream << "not an image";
    return path;
}

constexpr std::string_view card = "testcard-61x41-srgb8.png";

/// @brief One environment variable set or unset for a scope, and restored after.
///
/// The GPU switch is read from the environment, so the tests that pin it down
/// set it themselves rather than inherit whatever the runner has.
class ScopedEnvironment {
public:
    /// @brief Sets or unsets @p name until the scope ends.
    /// @param name Variable to change.
    /// @param value New value, or `nullptr` to unset it.
    ScopedEnvironment(const char* name, const char* value)
        : name_(name), wasSet_(qEnvironmentVariableIsSet(name)), previous_(qgetenv(name)) {
        if (value == nullptr) {
            qunsetenv(name_);
        } else {
            qputenv(name_, value);
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    ~ScopedEnvironment() {
        if (wasSet_) {
            qputenv(name_, previous_);
        } else {
            qunsetenv(name_);
        }
    }

private:
    /// @brief Variable changed.
    const char* name_;

    /// @brief Whether it was set before, if only to an empty value.
    bool wasSet_;

    /// @brief Value it had before.
    QByteArray previous_;
};

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

TEST_CASE("Every ranged float setting is an export option", "[cli]") {
    const auto result = invoke({"export", "--help"});
    REQUIRE(result.code == cli::Success);

    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!cli::isRangedFloatSetting(descriptor)) {
            continue;
        }
        std::string option = "--";
        for (const char c : std::string_view(descriptor.key)) {
            if (c >= 'A' && c <= 'Z') {
                option += '-';
                option += static_cast<char>(c - 'A' + 'a');
            } else {
                option += c;
            }
        }
        CAPTURE(descriptor.key, option);
        // A row without help wording in the export command gets no option.
        REQUIRE_THAT(result.out, ContainsSubstring(option + " "));
    }
}

TEST_CASE("The GPU probe is listed and carries its own help", "[cli][gpu]") {
    REQUIRE_THAT(invoke({"--help"}).out, ContainsSubstring("gpu-test"));

    const auto asked = GENERATE(std::vector<std::string>{"gpu-test", "--help"},
                                std::vector<std::string>{"gpu-test", "--help-all"},
                                std::vector<std::string>{"help", "gpu-test"});
    CAPTURE(asked);
    const auto result = invoke(asked);

    /// Help is text: it is answered without making a device, so it works under
    /// the suite's QCoreApplication as it does on a machine with no GPU.
    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("--backend"));
    REQUIRE_THAT(result.out, ContainsSubstring("--allow-software"));
    REQUIRE_THAT(result.out, ContainsSubstring("--size"));
    REQUIRE_THAT(result.out, ContainsSubstring("gpu-test"));
    REQUIRE(result.err.empty());
}

TEST_CASE("A wrong GPU probe command line exits 2 before any device is made", "[cli][gpu]") {
    const auto arguments = GENERATE(std::vector<std::string>{"gpu-test", "--backend", "nonsense"},
                                    std::vector<std::string>{"gpu-test", "--backend", "null"},
                                    std::vector<std::string>{"gpu-test", "--size", "0"},
                                    std::vector<std::string>{"gpu-test", "--size", "-1"},
                                    std::vector<std::string>{"gpu-test", "--size", "8193"},
                                    std::vector<std::string>{"gpu-test", "--size", "1.5"},
                                    std::vector<std::string>{"gpu-test", "--size", "huge"},
                                    std::vector<std::string>{"gpu-test", "photo.arw"},
                                    std::vector<std::string>{"gpu-test", "--nonsense"});
    CAPTURE(arguments);
    const auto result = invoke(arguments);

    /// Checked before a device is created, so the suite's QCoreApplication --
    /// which cannot make one -- would turn a missed check into exit 1, not 2.
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("gpu-test --help"));
    REQUIRE(result.out.empty());
    if (arguments[1] == "--size") {
        REQUIRE_THAT(result.err, ContainsSubstring("--size"));
    } else if (arguments[1] == "--backend") {
        REQUIRE_THAT(result.err, ContainsSubstring("'" + arguments[2] + "'"));
    }
}

TEST_CASE("An unknown backend is named and the real ones listed", "[cli][gpu]") {
    const auto result = invoke({"gpu-test", "--backend", "nonsense"});

    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("'nonsense'"));
    REQUIRE_THAT(result.err, ContainsSubstring("vulkan"));
    REQUIRE_THAT(result.err, ContainsSubstring("d3d11"));
}

TEST_CASE("Without a device the GPU probe fails rather than falls back", "[cli][gpu]") {
    /// The suite runs under a QCoreApplication, which has no platform plugin to
    /// make a device through: the honest answer is a failure that says why,
    /// never a probe that quietly passes on the CPU (ADR 015). Unset, empty and
    /// 0 all leave the GPU on, so each reaches the device and fails there.
    for (const char* value : {static_cast<const char*>(nullptr), "", "0"}) {
        CAPTURE(value == nullptr ? "(unset)" : value);
        const ScopedEnvironment enabled(cli::disableGpuVariable, value);
        const auto result = invoke({"gpu-test", "--size", "1"});

        REQUIRE(result.code == cli::Failed);
        REQUIRE_THAT(result.err, ContainsSubstring("error:"));
        REQUIRE_THAT(result.err, ContainsSubstring("QGuiApplication"));
        REQUIRE(result.out.empty());
    }
}

TEST_CASE("A GPU turned off by ARRAW_DISABLE_GPU fails the probe and says so", "[cli][gpu]") {
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    const auto result = invoke({"gpu-test", "--size", "1"});

    /// Refused before a device is attempted, so the error names the switch
    /// rather than the missing QGuiApplication it implies.
    REQUIRE(result.code == cli::Failed);
    REQUIRE_THAT(result.err, ContainsSubstring("error:"));
    REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
    REQUIRE_THAT(result.err, !ContainsSubstring("QGuiApplication"));
    REQUIRE(result.out.empty());

    /// The switch turns off the device, not the command line: help is still
    /// text, and a wrong flag is still a usage error.
    REQUIRE(invoke({"gpu-test", "--help"}).code == cli::Success);
    REQUIRE(invoke({"gpu-test", "--size", "0"}).code == cli::UsageError);
}

TEST_CASE("Any value of ARRAW_DISABLE_GPU but empty or 0 turns the GPU off", "[cli][gpu]") {
    REQUIRE(std::string_view(cli::disableGpuVariable) == "ARRAW_DISABLE_GPU");

    REQUIRE_FALSE(cli::disablesGpu(nullptr));
    REQUIRE_FALSE(cli::disablesGpu(""));
    REQUIRE_FALSE(cli::disablesGpu("0"));

    /// No guessing at what a word meant: "false" and "00" are values too.
    for (const char* value : {"1", "yes", "true", "false", "off", "00", " 0", "0 "}) {
        CAPTURE(value);
        REQUIRE(cli::disablesGpu(value));
    }
}

TEST_CASE("The top-level help mentions the GPU switch", "[cli][gpu]") {
    REQUIRE_THAT(invoke({"--help"}).out, ContainsSubstring("ARRAW_DISABLE_GPU"));
    REQUIRE_THAT(invoke({"gpu-test", "--help"}).out, ContainsSubstring("ARRAW_DISABLE_GPU"));
}

TEST_CASE("A reserved command says it is coming, not that it is unknown", "[cli]") {
    const auto reserved = GENERATE(std::string{"preset"});
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
        std::vector<std::string>{"export", image, "-o", output, "--sharpen", "strong"},
        std::vector<std::string>{"export", image, "-o", output, "--sharpen", "101"},
        std::vector<std::string>{"export", image, "-o", output, "--sharpen", "-1"},
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

    const auto quiet = invoke({"export", test::fixture(card).string(), "-o",
                               directory.path().string(), "--quiet", "--device", "cpu"});
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

TEST_CASE("The sharpen flag is documented and sharpens the export", "[cli]") {
    REQUIRE_THAT(invoke({"export", "--help"}).out, ContainsSubstring("--sharpen"));

    const test::TempDir directory;
    const auto image = test::fixture(card).string();
    const auto destination = directory.file("testcard-61x41-srgb8.png");
    const auto base = std::vector<std::string>{
        "export", image, "-o", directory.path().string(), "--format", "png", "--overwrite"};
    REQUIRE(invoke(base).code == cli::Success);
    const QImage plain(QString::fromStdString(destination.string()));

    auto sharp = base;
    sharp.insert(sharp.end(), {"--sharpen", "100"});
    REQUIRE(invoke(sharp).code == cli::Success);
    const QImage crisp(QString::fromStdString(destination.string()));

    REQUIRE_FALSE(plain.isNull());
    REQUIRE(crisp.size() == plain.size());
    REQUIRE(crisp != plain);
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
                "--format", "png", "--log-format", "json", "--device", "cpu"});

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
        // A diagnostic about no particular photograph names none: the batch's
        // own lines have no file, every other line has one.
        REQUIRE(object.contains("file") ==
                (object["notice"] != "batch_finished" && object["notice"] != "cpu_used"));
        ++objects;
    }
    // the device, the warning, the export, the failure, the summary
    REQUIRE(objects == 5);
}

TEST_CASE("An unknown log format is a usage error", "[cli]") {
    const test::TempDir directory;

    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--log-format", "yaml"});

    REQUIRE(result.code == cli::UsageError);
}

TEST_CASE("Help and usage errors start no Qt application", "[cli]") {
    /// A platform plugin that cannot start aborts the process, so anything
    /// that answers without doing work must not load one.
    const auto arguments =
        GENERATE(std::vector<std::string>{}, std::vector<std::string>{"--help"},
                 std::vector<std::string>{"--version"}, std::vector<std::string>{"nonsense"},
                 std::vector<std::string>{"info"}, std::vector<std::string>{"help", "gpu-test"},
                 std::vector<std::string>{"export", "--help"}, std::vector<std::string>{"export"},
                 std::vector<std::string>{"export", "--nonsense"},
                 std::vector<std::string>{"gpu-test", "--help"},
                 std::vector<std::string>{"gpu-test", "--size", "0"});
    CAPTURE(arguments);
    const auto result = invoke(arguments);
    REQUIRE(result.code != cli::Failed);
    REQUIRE(result.started.empty());
}

TEST_CASE("An export asks for a core application, once its arguments are good", "[cli]") {
    const test::TempDir directory;
    const auto input = writeRubbish(directory, "rubbish.png");
    const auto result = invoke(
        {"export", input.string(), "-o", directory.path().string(), "--quiet", "--device", "cpu"});
    REQUIRE(result.code == cli::Failed);
    REQUIRE(result.started == std::vector{cli::ApplicationKind::Core});
}

TEST_CASE("An export's application follows its device mode", "[cli][gpu]") {
    const test::TempDir directory;
    const auto input = test::fixture(card).string();
    const auto output = directory.path().string();

    struct Case {
        const char* device;
        const char* disabled;
        cli::ApplicationKind kind;
    };
    const auto [device, disabled, kind] = GENERATE(
        Case{"cpu", nullptr, cli::ApplicationKind::Core},
        Case{"cpu", "1", cli::ApplicationKind::Core}, Case{"auto", "1", cli::ApplicationKind::Core},
        Case{"auto", "0", cli::ApplicationKind::OffscreenDevice},
        Case{"auto", nullptr, cli::ApplicationKind::OffscreenDevice},
        Case{"gpu", nullptr, cli::ApplicationKind::OffscreenDevice});
    CAPTURE(device, disabled);
    const ScopedEnvironment environment(cli::disableGpuVariable, disabled);

    const auto result =
        invoke({"export", input, "-o", output, "--device", device, "--overwrite", "--quiet"});
    REQUIRE(result.started == std::vector{kind});
}

TEST_CASE("Export asks for a display's platform only for OpenGL", "[cli][gpu]") {
    const test::TempDir directory;
    const ScopedEnvironment enabled(cli::disableGpuVariable, nullptr);
    const auto opengl =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string(),
                "--gpu-backend", "opengl", "--overwrite", "--quiet"});
    REQUIRE(opengl.started == std::vector{cli::ApplicationKind::Gui});
}

TEST_CASE("Export --gpu-backend opengl asks for the GPU whatever --device says", "[cli][gpu]") {
    const test::TempDir directory;
    const auto input = test::fixture(card).string();
    const auto output = directory.path().string();

    SECTION("Auto with the GPU disabled is a usage error that names opengl") {
        const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
        const auto result = invoke({"export", input, "-o", output, "--gpu-backend", "opengl"});
        REQUIRE(result.code == cli::UsageError);
        REQUIRE_THAT(result.err, ContainsSubstring("--gpu-backend opengl"));
        REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
        REQUIRE(result.started.empty());
        REQUIRE(std::filesystem::is_empty(directory.path()));
    }
    SECTION("Auto with another backend still falls back") {
        const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
        const auto result = invoke(
            {"export", input, "-o", output, "--gpu-backend", "vulkan", "--overwrite", "--quiet"});
        REQUIRE(result.code == cli::Success);
        REQUIRE(result.started == std::vector{cli::ApplicationKind::Core});
    }
    SECTION("Cpu stays on the CPU, whatever the backend") {
        const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
        const auto result = invoke({"export", input, "-o", output, "--device", "cpu",
                                    "--gpu-backend", "opengl", "--overwrite", "--quiet"});
        REQUIRE(result.code == cli::Success);
        REQUIRE(result.started == std::vector{cli::ApplicationKind::Core});
    }
    SECTION("A GPU that cannot be made fails the export rather than falling back") {
        // The suite's QCoreApplication has no platform plugin, so creation fails.
        const ScopedEnvironment enabled(cli::disableGpuVariable, nullptr);
        const auto result = invoke({"export", input, "-o", output, "--gpu-backend", "opengl",
                                    "--overwrite", "--log-format", "json"});
        REQUIRE(result.code == cli::Failed);
        REQUIRE_THAT(result.err, ContainsSubstring("\"notice\":\"gpu_failed\""));
        REQUIRE_THAT(result.err, !ContainsSubstring("gpu_fallback"));
    }
}

TEST_CASE("The device options are validated before anything starts", "[cli][gpu]") {
    const test::TempDir directory;
    const auto input = test::fixture(card).string();
    const auto output = directory.path().string();
    const ScopedEnvironment environment(cli::disableGpuVariable, nullptr);

    SECTION("An unknown device is named") {
        const auto result = invoke({"export", input, "-o", output, "--device", "tpu"});
        REQUIRE(result.code == cli::UsageError);
        REQUIRE_THAT(result.err, ContainsSubstring("unknown device 'tpu'"));
        REQUIRE(result.started.empty());
    }
    SECTION("An unknown backend is named") {
        const auto result = invoke({"export", input, "-o", output, "--gpu-backend", "null"});
        REQUIRE(result.code == cli::UsageError);
        REQUIRE_THAT(result.err, ContainsSubstring("unknown backend 'null'"));
        REQUIRE(result.started.empty());
    }
    SECTION("The known values parse") {
        for (const char* device : {"auto", "gpu", "cpu", "GPU"}) {
            for (const char* backend : {"vulkan", "opengl", "d3d11", "d3d12", "metal"}) {
                CAPTURE(device, backend);
                const auto result =
                    invoke({"export", input, "-o", output, "--device", device, "--gpu-backend",
                            backend, "--allow-software", "--overwrite", "--quiet"});
                REQUIRE(result.code != cli::UsageError);
            }
        }
    }
    SECTION("The help describes the options") {
        const auto help = invoke({"export", "--help"}).out;
        REQUIRE_THAT(help, ContainsSubstring("--device"));
        REQUIRE_THAT(help, ContainsSubstring("--gpu-backend"));
        REQUIRE_THAT(help, ContainsSubstring("--allow-software"));
    }
}

TEST_CASE("Export --device gpu with the GPU disabled is a usage error", "[cli][gpu]") {
    const test::TempDir directory;
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");

    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--device", "gpu"});

    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
    REQUIRE(result.started.empty());
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("Export --device auto with the GPU disabled exports on the CPU and says so once",
          "[cli][gpu]") {
    const test::TempDir directory;
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");

    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--log-format", "json"});

    REQUIRE(result.code == cli::Success);
    REQUIRE(std::filesystem::exists(directory.file("testcard-61x41-srgb8.jpg")));
    REQUIRE(result.started == std::vector{cli::ApplicationKind::Core});
    REQUIRE_THAT(result.err, ContainsSubstring("\"notice\":\"gpu_fallback\""));
    REQUIRE_THAT(result.err, ContainsSubstring("\"severity\":\"warning\""));
    REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
    REQUIRE_THAT(result.err, ContainsSubstring("\"notice\":\"cpu_used\""));
    REQUIRE_THAT(result.err, !ContainsSubstring("gpu_used"));
}

TEST_CASE("Export --device auto falls back to the CPU when no device can be made", "[cli][gpu]") {
    /// The suite's QCoreApplication has no platform plugin, so creation fails.
    const test::TempDir directory;
    const ScopedEnvironment enabled(cli::disableGpuVariable, nullptr);
    const auto input = test::fixture(card).string();

    SECTION("in text, one warning then the device line") {
        const auto result = invoke({"export", input, "-o", directory.path().string()});
        REQUIRE(result.code == cli::Success);
        REQUIRE(std::filesystem::exists(directory.file("testcard-61x41-srgb8.jpg")));
        REQUIRE_THAT(result.err, StartsWith("warning: "));
        REQUIRE_THAT(result.err, ContainsSubstring("exporting on the CPU instead"));
        REQUIRE_THAT(result.err, ContainsSubstring("exporting on the CPU\n"));
    }
    SECTION("under --quiet only the warning remains") {
        const auto result = invoke({"export", input, "-o", directory.path().string(), "--quiet"});
        REQUIRE(result.code == cli::Success);
        REQUIRE_THAT(result.err, StartsWith("warning: "));
        REQUIRE_THAT(result.err, !ContainsSubstring("exporting on the CPU\n"));
        REQUIRE_THAT(result.err, !ContainsSubstring("written to"));
    }
    SECTION("in JSON, both notices are structured") {
        const auto result =
            invoke({"export", input, "-o", directory.path().string(), "--log-format", "json"});
        REQUIRE(result.code == cli::Success);
        std::istringstream lines(result.err);
        std::string line;
        std::vector<std::string> notices;
        while (std::getline(lines, line)) {
            const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(line));
            REQUIRE(parsed.isObject());
            notices.push_back(parsed.object()["notice"].toString().toStdString());
        }
        REQUIRE(notices == std::vector<std::string>{"gpu_fallback", "cpu_used", "exported"});
    }
}

TEST_CASE("Export --device cpu says which device it used and never warns", "[cli][gpu]") {
    const test::TempDir directory;
    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--device", "cpu"});
    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.err, StartsWith("exporting on the CPU\n"));
    REQUIRE_THAT(result.err, !ContainsSubstring("warning"));
}

TEST_CASE("Export --device gpu without a usable device fails before any input", "[cli][gpu]") {
    const test::TempDir directory;
    const ScopedEnvironment enabled(cli::disableGpuVariable, nullptr);

    const auto result =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string(), "--device",
                "gpu", "--log-format", "json"});

    REQUIRE(result.code == cli::Failed);
    REQUIRE(result.started == std::vector{cli::ApplicationKind::OffscreenDevice});
    REQUIRE(std::filesystem::is_empty(directory.path()));
    REQUIRE_THAT(result.err, ContainsSubstring("\"notice\":\"gpu_failed\""));
    REQUIRE_THAT(result.err, !ContainsSubstring("\"notice\":\"exported\""));
    REQUIRE_THAT(result.err, !ContainsSubstring("fallback"));
}

TEST_CASE("gpu-test takes --gpu-backend, and --backend as its alias", "[cli][gpu]") {
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    for (const char* name : {"--gpu-backend", "--backend"}) {
        CAPTURE(name);
        REQUIRE(invoke({"gpu-test", name, "vulkan", "--size", "1"}).code == cli::Failed);
        const auto bad = invoke({"gpu-test", name, "nonsense"});
        REQUIRE(bad.code == cli::UsageError);
        REQUIRE_THAT(bad.err, ContainsSubstring("unknown backend 'nonsense'"));
    }
    const auto help = invoke({"gpu-test", "--help"}).out;
    REQUIRE_THAT(help, ContainsSubstring("--gpu-backend"));
    REQUIRE_THAT(help, ContainsSubstring("Alias of --gpu-backend"));
}

TEST_CASE("The GPU probe asks for a GUI application unless the GPU is off", "[cli][gpu]") {
    {
        const ScopedEnvironment enabled(cli::disableGpuVariable, "0");
        REQUIRE(invoke({"gpu-test", "--size", "1"}).started ==
                std::vector{cli::ApplicationKind::Gui});
    }
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    REQUIRE(invoke({"gpu-test", "--size", "1"}).started.empty());
}

TEST_CASE("A command's help names the program as arraw-cli", "[cli]") {
    const auto command = GENERATE(as<std::string>{}, "export", "gpu-test");
    CAPTURE(command);
    REQUIRE_THAT(invoke({command, "--help"}).out,
                 StartsWith("Usage: arraw-cli [options] " + command));
}

TEST_CASE("The GPU probe logs through the diagnostic log, as JSON when asked", "[cli][gpu]") {
    /// One writer for everything a command says, so --log-format json covers
    /// the probe's failures as it covers an export's (ADR 006). The device
    /// report itself is what was asked for, and stays on stdout.
    struct Case {
        const char* disabled;
        std::string notice;
    };
    const auto [disabled, notice] = GENERATE(Case{"1", "gpu_disabled"}, Case{"0", "gpu_failed"});
    CAPTURE(notice);
    const ScopedEnvironment environment(cli::disableGpuVariable, disabled);

    const auto result = invoke({"gpu-test", "--size", "1", "--log-format", "json"});

    REQUIRE(result.code == cli::Failed);
    std::istringstream lines(result.err);
    std::string line;
    REQUIRE(std::getline(lines, line));
    const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(line));
    REQUIRE(parsed.isObject());
    REQUIRE(parsed.object()["notice"] == QString::fromStdString(notice));
    REQUIRE(parsed.object()["severity"] == "error");
    REQUIRE_FALSE(parsed.object().contains("file"));
    REQUIRE_FALSE(std::getline(lines, line));
}

TEST_CASE("The GPU probe's text log reads as it always has", "[cli][gpu]") {
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    REQUIRE(invoke({"gpu-test", "--size", "1"}).err ==
            "error: the GPU is disabled by ARRAW_DISABLE_GPU; unset it, or set it to 0, to probe "
            "the device\n");
}

TEST_CASE("An unknown log format is a usage error for the GPU probe too", "[cli][gpu]") {
    const auto result = invoke({"gpu-test", "--log-format", "yaml"});
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("'yaml'"));
    REQUIRE(result.started.empty());
}

TEST_CASE("A device choice is auto, cpu, gpu, or gpu and a number, in any case", "[cli][gpu]") {
    using cli::DeviceKind;
    using Choice = cli::DeviceChoice;

    REQUIRE(cli::parseDeviceChoice("auto") == Choice{DeviceKind::Auto, std::nullopt});
    REQUIRE(cli::parseDeviceChoice("CPU") == Choice{DeviceKind::Cpu, std::nullopt});
    REQUIRE(cli::parseDeviceChoice("gpu") == Choice{DeviceKind::Gpu, std::nullopt});
    REQUIRE(cli::parseDeviceChoice("GPU") == Choice{DeviceKind::Gpu, std::nullopt});
    REQUIRE(cli::parseDeviceChoice("gpu0") == Choice{DeviceKind::Gpu, 0});
    REQUIRE(cli::parseDeviceChoice("Gpu12") == Choice{DeviceKind::Gpu, 12});
    REQUIRE(cli::parseDeviceChoice("gpu007") == Choice{DeviceKind::Gpu, 7});

    for (const std::string_view bad :
         {"", " ", "gpux", "gpu-1", "gpu+1", "gpu1.5", "gpu 1", "gpu1 ", "gpu1x", "cpu0", "auto1",
          "cuda", "gpu99999999999999999999999"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(cli::parseDeviceChoice(bad).has_value());
    }
}

TEST_CASE("A wrong --device is a usage error for both commands, naming the value", "[cli][gpu]") {
    const auto bad = GENERATE(std::string{"gpux"}, std::string{"gpu-1"}, std::string{"gpu1.5"},
                              std::string{"gpu 1"}, std::string{"tpu"});
    const std::string image = test::fixture(card).string();
    const test::TempDir directory;
    CAPTURE(bad);

    const auto exported =
        invoke({"export", image, "-o", directory.path().string(), "--device", bad});
    REQUIRE(exported.code == cli::UsageError);
    REQUIRE_THAT(exported.err, ContainsSubstring("'" + bad + "'"));
    REQUIRE_THAT(exported.err, ContainsSubstring("gpuN"));
    REQUIRE(exported.started.empty());

    const auto probed = invoke({"gpu-test", "--device", bad});
    REQUIRE(probed.code == cli::UsageError);
    REQUIRE_THAT(probed.err, ContainsSubstring("'" + bad + "'"));
    REQUIRE_THAT(probed.err, ContainsSubstring("gpuN"));
    REQUIRE(probed.started.empty());
}

TEST_CASE("The GPU probe has no CPU to test", "[cli][gpu]") {
    const auto result = invoke({"gpu-test", "--device", "cpu"});
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("--device cpu"));
    REQUIRE(result.started.empty());
}

TEST_CASE("Both commands' help explains gpuN", "[cli][gpu]") {
    for (const std::string command : {"export", "gpu-test"}) {
        CAPTURE(command);
        const auto result = invoke({command, "--help"});
        REQUIRE(result.code == cli::Success);
        REQUIRE_THAT(result.out, ContainsSubstring("gpuN"));
    }
    REQUIRE_THAT(invoke({"gpu-test", "--help"}).out, ContainsSubstring("every adapter"));
}

TEST_CASE("A numbered GPU cannot be used while ARRAW_DISABLE_GPU turns the GPU off", "[cli][gpu]") {
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    const test::TempDir directory;
    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--device", "gpu2"});

    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("--device gpu2"));
    REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
}

TEST_CASE("The GPU probe fails on a disabled GPU whatever device was asked for", "[cli][gpu]") {
    const ScopedEnvironment disabled(cli::disableGpuVariable, "1");
    for (const std::string device : {"auto", "gpu", "gpu1"}) {
        CAPTURE(device);
        const auto result = invoke({"gpu-test", "--device", device});
        REQUIRE(result.code == cli::Failed);
        REQUIRE_THAT(result.err, ContainsSubstring("ARRAW_DISABLE_GPU"));
    }
}

namespace {

constexpr std::string_view sidecarRaw = "linear-32x24-neutral.dng";

/// @brief Copies the RAW fixture into a directory under a name, so a sidecar can sit beside it.
std::filesystem::path copyRaw(const test::TempDir& directory, const std::string& name) {
    const auto path = directory.file(name);
    std::filesystem::copy_file(test::fixture(sidecarRaw), path);
    return path;
}

/// @brief Writes a sidecar holding @p settings beside a photograph.
void sidecarWith(const std::filesystem::path& photo, const DevelopSettings& settings) {
    writeSidecar(openPhoto(photo).with(DevelopState{settings}));
}

/// @brief Exports one file to PNG on the CPU with extra flags, and loads the result.
/// @param code Receives the exit code when not null.
QImage exportedPng(const std::filesystem::path& input, std::vector<std::string> flags,
                   int* code = nullptr, std::string* err = nullptr) {
    const test::TempDir output;
    std::vector<std::string> arguments{
        "export",   input.string(), "-o",       output.path().string(),
        "--format", "png",          "--device", "cpu"};
    arguments.insert(arguments.end(), flags.begin(), flags.end());
    const auto result = invoke(arguments);
    if (code != nullptr) {
        *code = result.code;
    }
    if (err != nullptr) {
        *err = result.err;
    }
    return QImage(QString::fromStdString((output.path() / input.stem()).string() + ".png"));
}

/// @brief Settings that change every part of the picture the flags below can name.
DevelopSettings editedSettings() {
    DevelopSettings settings;
    settings.tone.exposure = 0.5F;
    settings.tone.shadows = 30.0F;
    settings.color.whiteBalance = WhiteBalanceMode::Custom;
    settings.color.temperature = 4200.0F;
    settings.color.tint = 10.0F;
    settings.geometry.rotation = QuarterTurn::Clockwise90;
    settings.geometry.flipHorizontal = true;
    settings.geometry.crop.rectangle = UprightCropRect{0.1, 0.1, 0.9, 0.9};
    return settings;
}

const std::vector<std::string> editedFlags{
    "--exposure", "0.5", "--shadows",         "30",     "--temperature",  "4200", "--tint", "10",
    "--rotate",   "90",  "--flip-horizontal", "--crop", "0.1,0.1,0.9,0.9"};

std::vector<std::string> withNoSidecar(std::vector<std::string> flags) {
    flags.emplace_back("--no-sidecar");
    return flags;
}

} // namespace

TEST_CASE("An export renders through the photograph's own sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    sidecarWith(raw, editedSettings());

    const QImage fromSidecar = exportedPng(raw, {});
    const QImage fromFlags = exportedPng(raw, withNoSidecar(editedFlags));

    /// The sidecar and the flags say the same thing, so the pixels must be the same.
    REQUIRE_FALSE(fromSidecar.isNull());
    REQUIRE(fromSidecar == fromFlags);
    REQUIRE(fromSidecar != exportedPng(raw, {"--no-sidecar"}));
}

TEST_CASE("The --no-sidecar flag ignores the sidecar and equals a file without one",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto edited = copyRaw(directory, "edited.dng");
    sidecarWith(edited, editedSettings());
    const auto bare = copyRaw(directory, "bare.dng");

    const QImage ignored = exportedPng(edited, {"--no-sidecar", "--contrast", "20"});

    REQUIRE(ignored == exportedPng(bare, {"--contrast", "20"}));
}

TEST_CASE("A flag overrides its own key and keeps the rest of the sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.tone.exposure = 1.0F;
    settings.tone.contrast = 20.0F;
    sidecarWith(raw, settings);

    const QImage overridden = exportedPng(raw, {"--exposure", "0"});

    REQUIRE(overridden == exportedPng(raw, {"--no-sidecar", "--contrast", "20"}));
    REQUIRE(overridden != exportedPng(raw, {}));
}

TEST_CASE("Flips and geometry keys the flags do not name stay as the sidecar has them",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.geometry.flipHorizontal = true;
    settings.geometry.crop.rectangle = UprightCropRect{0.1, 0.1, 0.5, 0.9};
    sidecarWith(raw, settings);

    /// Naming the tone alone touches no geometry: the flip and the rectangle stay.
    const QImage image = exportedPng(raw, {"--contrast", "20"});

    REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--flip-horizontal", "--crop",
                                       "0.1,0.1,0.5,0.9", "--contrast", "20"}));
    REQUIRE(image !=
            exportedPng(raw, {"--no-sidecar", "--crop", "0.1,0.1,0.5,0.9", "--contrast", "20"}));
}

TEST_CASE("A --crop on a sidecar with an aspect leaves the aspect free", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.geometry.crop.aspect = CropRatio{1.0};
    sidecarWith(raw, settings);

    int code = 0;
    std::string err;
    const QImage image = exportedPng(raw, {"--crop", "0.1,0.1,0.9,0.9"}, &code, &err);

    REQUIRE(code == cli::Success);
    REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--crop", "0.1,0.1,0.9,0.9"}));
}

TEST_CASE("The --crop and --crop-aspect flags together are taken as given", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.geometry.crop.aspect = CropRatio{1.0};
    sidecarWith(raw, settings);

    /// 0.6 x 0.8 of a 4:3 image is square, so the sidecar's aspect and this rectangle agree.
    REQUIRE(
        exportedPng(raw, {"--crop", "0.2,0.1,0.8,0.9", "--crop-aspect", "1:1"}) ==
        exportedPng(raw, {"--no-sidecar", "--crop", "0.2,0.1,0.8,0.9", "--crop-aspect", "1:1"}));
}

TEST_CASE("A --crop-aspect on a sidecar with a rectangle makes the crop automatic",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.geometry.crop.rectangle = UprightCropRect{0.2, 0.1, 0.8, 0.9};
    settings.geometry.crop.aspect = CropRatio{1.0};
    sidecarWith(raw, settings);

    SECTION("a different ratio") {
        int code = 0;
        const QImage image = exportedPng(raw, {"--crop-aspect", "3:2"}, &code);
        REQUIRE(code == cli::Success);
        REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--crop-aspect", "3:2"}));
    }
    SECTION("original") {
        int code = 0;
        const QImage image = exportedPng(raw, {"--crop-aspect", "original"}, &code);
        REQUIRE(code == cli::Success);
        REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--crop-aspect", "original"}));
    }
    SECTION("the same ratio keeps the rectangle") {
        REQUIRE(exportedPng(raw, {"--crop-aspect", "1:1"}) ==
                exportedPng(raw,
                            {"--no-sidecar", "--crop", "0.2,0.1,0.8,0.9", "--crop-aspect", "1:1"}));
    }
    SECTION("free keeps the rectangle") {
        REQUIRE(exportedPng(raw, {"--crop-aspect", "free"}) ==
                exportedPng(raw, {"--no-sidecar", "--crop", "0.2,0.1,0.8,0.9"}));
    }
}

TEST_CASE("A --rotate that turns the frame reframes a sidecar's crop, and says so",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    /// Square on the 4:3 image; the ratio stays square once the image is a quarter-turn.
    settings.geometry.crop.rectangle = UprightCropRect{0.2, 0.1, 0.8, 0.9};
    settings.geometry.crop.aspect = CropRatio{1.0};
    sidecarWith(raw, settings);

    int code = 0;
    std::string err;
    const QImage image = exportedPng(raw, {"--rotate", "90"}, &code, &err);

    REQUIRE(code == cli::Success);
    REQUIRE_THAT(err, ContainsSubstring("automatic framing"));
    REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--rotate", "90", "--crop-aspect", "1:1"}));
}

TEST_CASE("A flip flag reframes a sidecar's crop, and --no-flip undoes a sidecar's flip",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings plain;
    plain.geometry.crop.rectangle = UprightCropRect{0.0, 0.1, 0.5, 0.6};
    DevelopSettings flipped = plain;
    flipped.geometry.flipHorizontal = true;
    flipped.geometry.crop.rectangle = UprightCropRect{0.5, 0.1, 1.0, 0.6};
    const auto other = copyRaw(directory, "other.dng");
    sidecarWith(raw, plain);
    sidecarWith(other, flipped);

    SECTION("adding a flip frames the picture anew") {
        REQUIRE(exportedPng(raw, {"--flip-horizontal"}) ==
                exportedPng(raw, {"--no-sidecar", "--flip-horizontal"}));
    }
    SECTION("removing a flip frames the picture anew") {
        REQUIRE(exportedPng(other, {"--no-flip-horizontal"}) ==
                exportedPng(other, {"--no-sidecar"}));
    }
    SECTION("a flip with a crop uses that crop") {
        REQUIRE(exportedPng(raw, {"--flip-horizontal", "--crop", "0.5,0.1,1,0.6"}) ==
                exportedPng(raw, {"--no-sidecar", "--flip-horizontal", "--crop", "0.5,0.1,1,0.6"}));
    }
    SECTION("asking for the flip a sidecar has changes nothing") {
        REQUIRE(exportedPng(other, {"--flip-horizontal"}) == exportedPng(other, {}));
    }
    SECTION("a flip and its undoing contradict") {
        int code = 0;
        std::string err;
        (void)exportedPng(raw, {"--flip-horizontal", "--no-flip-horizontal"}, &code, &err);
        REQUIRE(code == cli::UsageError);
        REQUIRE_THAT(err, ContainsSubstring("contradict"));
    }
}

TEST_CASE("The --rotate flag replaces both the quarter-turn and the straighten of a sidecar",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.geometry.rotation = QuarterTurn::Clockwise180;
    settings.geometry.straighten = 3.0;
    sidecarWith(raw, settings);

    REQUIRE(exportedPng(raw, {"--rotate", "90"}) ==
            exportedPng(raw, {"--no-sidecar", "--rotate", "90"}));
}

TEST_CASE("A --tint alone keeps the temperature of a Custom sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.color.whiteBalance = WhiteBalanceMode::Custom;
    settings.color.temperature = 4200.0F;
    sidecarWith(raw, settings);

    const QImage image = exportedPng(raw, {"--tint", "10"});

    REQUIRE(image == exportedPng(raw, {"--no-sidecar", "--temperature", "4200", "--tint", "10"}));
    REQUIRE(image != exportedPng(raw, {"--no-sidecar", "--tint", "10"}));
}

TEST_CASE(
    "Naming one half of white balance leaves the other as shot when the sidecar is not Custom",
    "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    /// As shot, but with a temperature a Custom photograph would have used.
    settings.color.temperature = 3000.0F;
    sidecarWith(raw, settings);

    REQUIRE(exportedPng(raw, {"--tint", "10"}) ==
            exportedPng(raw, {"--no-sidecar", "--tint", "10"}));
}

TEST_CASE("The --white-balance as-shot flag overrides a Custom sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.color.whiteBalance = WhiteBalanceMode::Custom;
    settings.color.temperature = 4200.0F;
    settings.color.tint = 10.0F;
    sidecarWith(raw, settings);

    const QImage image = exportedPng(raw, {"--white-balance", "as-shot"});

    REQUIRE(image == exportedPng(raw, {"--no-sidecar"}));
    REQUIRE(image != exportedPng(raw, {}));
}

TEST_CASE("A temperature on a non-RAW still fails that file with a sidecar present",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto png = directory.file("card.png");
    std::filesystem::copy_file(test::fixture(card), png);
    sidecarWith(png, {});

    int code = 0;
    std::string err;
    (void)exportedPng(png, {"--temperature", "5000"}, &code, &err);

    REQUIRE(code == cli::Failed);
    REQUIRE_THAT(err, ContainsSubstring("sensor"));
}

TEST_CASE("An unreadable sidecar fails its file unless --no-sidecar is given", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    std::ofstream(directory.file("frame.xmp")) << "this is not xml <<<";

    int code = 0;
    std::string err;
    const test::TempDir output;
    const auto result = invoke({"export", raw.string(), "-o", output.path().string(), "--format",
                                "png", "--device", "cpu", "--exposure", "0.5"});

    REQUIRE(result.code == cli::Failed);
    REQUIRE_THAT(result.err, ContainsSubstring("error:"));
    REQUIRE_THAT(result.err, ContainsSubstring("frame.xmp"));
    REQUIRE_THAT(result.err, ContainsSubstring("--no-sidecar"));
    REQUIRE_FALSE(std::filesystem::exists(output.path() / "frame.png"));

    const QImage image = exportedPng(raw, {"--no-sidecar", "--exposure", "0.5"}, &code, &err);
    REQUIRE(code == cli::Success);
    REQUIRE_FALSE(image.isNull());
}

TEST_CASE("One file's unreadable sidecar leaves the rest of the batch exported", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto broken = copyRaw(directory, "broken.dng");
    const auto fine = copyRaw(directory, "fine.dng");
    std::ofstream(directory.file("broken.xmp")) << "not xml";
    const test::TempDir output;

    const auto result =
        invoke({"export", broken.string(), fine.string(), "-o", output.path().string(), "--format",
                "png", "--device", "cpu", "--log-format", "json"});

    REQUIRE(result.code == cli::Failed);
    REQUIRE_FALSE(std::filesystem::exists(output.path() / "broken.png"));
    REQUIRE(std::filesystem::exists(output.path() / "fine.png"));
    REQUIRE_THAT(result.err, ContainsSubstring("\"notice\":\"batch_finished\""));
}

TEST_CASE("A sidecar value out of range is clamped and reported, in text and JSON",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    std::ofstream(directory.file("frame.xmp"))
        << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
           "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
           "<rdf:Description rdf:about=\"\" xmlns:arraw=\"http://ns.arraw.org/develop/1.0/\" "
           "arraw:exposure=\"99\" arraw:futureKnob=\"7\"/></rdf:RDF></x:xmpmeta>";

    int code = 0;
    std::string err;
    (void)exportedPng(raw, {}, &code, &err);
    REQUIRE(code == cli::Success);
    REQUIRE_THAT(err, ContainsSubstring("warning:"));
    REQUIRE_THAT(err, ContainsSubstring("'exposure' is 99"));
    REQUIRE_THAT(err, ContainsSubstring("'futureKnob' is not a setting"));

    (void)exportedPng(raw, {"--log-format", "json"}, &code, &err);
    REQUIRE_THAT(err, ContainsSubstring("\"notice\":\"setting_clamped\""));
    REQUIRE_THAT(err, ContainsSubstring("\"notice\":\"setting_unknown\""));
    /// Every diagnostic is about the photograph, not its sidecar.
    REQUIRE_THAT(err, ContainsSubstring("\"file\":\"" + raw.string() + "\""));
    REQUIRE_THAT(err, !ContainsSubstring("frame.xmp\""));
}

TEST_CASE("An unreadable sidecar has a JSON notice of its own, about the photograph",
          "[cli][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    std::ofstream(directory.file("frame.xmp")) << "not xml";

    std::string err;
    (void)exportedPng(raw, {"--log-format", "json"}, nullptr, &err);

    REQUIRE_THAT(err, ContainsSubstring("\"notice\":\"sidecar_unreadable\""));
    REQUIRE_THAT(err, ContainsSubstring("\"severity\":\"error\""));
    REQUIRE_THAT(err, ContainsSubstring("\"file\":\"" + raw.string() + "\""));
}

TEST_CASE("A batch renders each file through its own sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto first = copyRaw(directory, "first.dng");
    const auto second = copyRaw(directory, "second.dng");
    DevelopSettings brighter;
    brighter.tone.exposure = 1.0F;
    DevelopSettings darker;
    darker.tone.exposure = -1.0F;
    sidecarWith(first, brighter);
    sidecarWith(second, darker);
    const test::TempDir output;

    const auto result =
        invoke({"export", first.string(), second.string(), "-o", output.path().string(), "--format",
                "png", "--device", "cpu", "--contrast", "10"});

    REQUIRE(result.code == cli::Success);
    const auto load = [&](const char* name) {
        return QImage(QString::fromStdString((output.path() / name).string()));
    };
    REQUIRE(load("first.png") ==
            exportedPng(first, withNoSidecar({"--exposure", "1", "--contrast", "10"})));
    REQUIRE(load("second.png") ==
            exportedPng(second, withNoSidecar({"--exposure", "-1", "--contrast", "10"})));
}

TEST_CASE("The command line never writes or changes a sidecar", "[cli][sidecar]") {
    const test::TempDir directory;
    const auto edited = copyRaw(directory, "edited.dng");
    sidecarWith(edited, editedSettings());
    const auto bare = copyRaw(directory, "bare.dng");
    const auto slurp = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const std::string before = slurp(directory.file("edited.xmp"));

    (void)exportedPng(edited, {"--exposure", "1"});
    (void)exportedPng(bare, {"--exposure", "1"});

    REQUIRE(slurp(directory.file("edited.xmp")) == before);
    REQUIRE_FALSE(std::filesystem::exists(directory.file("bare.xmp")));
}

TEST_CASE("The export help documents --no-sidecar and the sidecar rule", "[cli][sidecar]") {
    const auto result = invoke({"export", "--help"});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("--no-sidecar"));
    REQUIRE_THAT(result.out, ContainsSubstring("own .xmp sidecar"));
}

namespace {

/// @brief Reads the edits of some flags, which must be well-formed.
cli::ExportEdits editsOf(const std::vector<std::string>& flags) {
    std::ostringstream err;
    const auto edits = cli::readExportEdits(flags, err);
    REQUIRE(edits.has_value());
    return *edits;
}

/// @brief Applies flags to settings.
DevelopSettings applied(const DevelopSettings& base, const std::vector<std::string>& flags) {
    CollectedDiagnostics log;
    return cli::applyEdits(base, editsOf(flags), log, "frame.dng");
}

/// @brief Whether a crop rectangle is, to rounding, the one expected.
bool isNear(const std::optional<UprightCropRect>& actual, const UprightCropRect& expected) {
    using Catch::Approx;
    return actual && actual->left == Approx(expected.left) && actual->top == Approx(expected.top) &&
           actual->right == Approx(expected.right) && actual->bottom == Approx(expected.bottom);
}

} // namespace

TEST_CASE("Every numeric flag replaces its own setting and no other", "[cli][sidecar]") {
    struct Row {
        const char* flag;
        std::optional<float> (*read)(const DevelopSettings&);
        const char* value;
        float replaced;
    };
    const Row rows[]{
        {"--exposure", [](const DevelopSettings& s) { return std::optional(s.tone.exposure); }, "2",
         2.0F},
        {"--contrast", [](const DevelopSettings& s) { return std::optional(s.tone.contrast); },
         "-40", -40.0F},
        {"--shadows", [](const DevelopSettings& s) { return std::optional(s.tone.shadows); }, "40",
         40.0F},
        {"--highlights", [](const DevelopSettings& s) { return std::optional(s.tone.highlights); },
         "-40", -40.0F},
        {"--blacks", [](const DevelopSettings& s) { return std::optional(s.tone.blacks); }, "20",
         20.0F},
        {"--whites", [](const DevelopSettings& s) { return std::optional(s.tone.whites); }, "-20",
         -20.0F},
        {"--filmic-highlights",
         [](const DevelopSettings& s) { return std::optional(s.tone.filmicHighlights); }, "0",
         0.0F},
        {"--temperature", [](const DevelopSettings& s) { return s.color.temperature; }, "6500",
         6500.0F},
        {"--tint", [](const DevelopSettings& s) { return s.color.tint; }, "-10", -10.0F},
    };
    DevelopSettings sidecar;
    sidecar.tone = {.exposure = 1.0F,
                    .contrast = 20.0F,
                    .shadows = 30.0F,
                    .highlights = -20.0F,
                    .blacks = 10.0F,
                    .whites = -10.0F,
                    .filmicHighlights = 60.0F};
    sidecar.color.whiteBalance = WhiteBalanceMode::Custom;
    sidecar.color.temperature = 4200.0F;
    sidecar.color.tint = 10.0F;

    const DevelopSettings untouched = applied(sidecar, {});
    REQUIRE(untouched == sidecar);
    for (const Row& row : rows) {
        DYNAMIC_SECTION(row.flag) {
            const DevelopSettings result = applied(sidecar, {row.flag, row.value});
            for (const Row& other : rows) {
                if (&other == &row) {
                    REQUIRE(other.read(result) == std::optional(row.replaced));
                } else {
                    REQUIRE(other.read(result) == other.read(sidecar));
                }
            }
            REQUIRE(result.geometry == sidecar.geometry);
        }
    }
}

TEST_CASE("The colour flags replace their own setting and keep the sidecar's others",
          "[cli][sidecar][colour]") {
    DevelopSettings sidecar;
    sidecar.color.saturation = 10.0F;
    sidecar.hsl.green.luminance = -20.0F;
    sidecar.blackAndWhite.blue = 30.0F;

    SECTION("saturation and vibrance") {
        const DevelopSettings result =
            applied(sidecar, {"--saturation", "-60", "--vibrance", "25"});
        REQUIRE(result.color.saturation == -60.0F);
        REQUIRE(result.color.vibrance == 25.0F);
        REQUIRE(result.hsl == sidecar.hsl);
        REQUIRE(result.blackAndWhite == sidecar.blackAndWhite);
    }
    SECTION("an HSL band") {
        const DevelopSettings result =
            applied(sidecar, {"--hue-red", "40", "--luminance-magenta", "-5"});
        REQUIRE(result.hsl.red.hue == 40.0F);
        REQUIRE(result.hsl.magenta.luminance == -5.0F);
        REQUIRE(result.hsl.green.luminance == -20.0F);
        REQUIRE(result.color == sidecar.color);
    }
    SECTION("the black and white mix and switch") {
        const DevelopSettings result =
            applied(sidecar, {"--convert-to-grayscale", "--gray-red", "55"});
        REQUIRE(result.blackAndWhite.convertToGrayscale);
        REQUIRE(result.blackAndWhite.red == 55.0F);
        REQUIRE(result.blackAndWhite.blue == 30.0F);

        sidecar.blackAndWhite.convertToGrayscale = true;
        REQUIRE_FALSE(
            applied(sidecar, {"--no-convert-to-grayscale"}).blackAndWhite.convertToGrayscale);
        REQUIRE(applied(sidecar, {}).blackAndWhite.convertToGrayscale);
    }
    SECTION("the switch cannot contradict itself, and a weight is range checked") {
        std::ostringstream err;
        REQUIRE_FALSE(
            cli::readExportEdits({"--convert-to-grayscale", "--no-convert-to-grayscale"}, err));
        std::ostringstream rangeErr;
        REQUIRE_FALSE(cli::readExportEdits({"--gray-aqua", "101"}, rangeErr));
        REQUIRE_THAT(rangeErr.str(), ContainsSubstring("--gray-aqua accepts -100 to 100"));
    }
}

TEST_CASE("Geometry flags keep the sidecar's geometry they do not name", "[cli][sidecar]") {
    DevelopSettings sidecar;
    sidecar.geometry.rotation = QuarterTurn::Clockwise180;
    sidecar.geometry.straighten = 3.0;
    sidecar.geometry.flipHorizontal = true;
    sidecar.geometry.flipVertical = true;
    sidecar.geometry.crop.rectangle = UprightCropRect{0.1, 0.2, 0.6, 0.7};
    sidecar.geometry.crop.aspect = CropRatio{2.0};

    SECTION("nothing named") {
        REQUIRE(applied(sidecar, {"--exposure", "1"}).geometry == sidecar.geometry);
    }
    SECTION("the vertical flip is undone, and the crop is framed anew") {
        CollectedDiagnostics log;
        const auto geometry =
            cli::applyEdits(sidecar, editsOf({"--no-flip-vertical"}), log, "frame.dng").geometry;
        REQUIRE_FALSE(geometry.flipVertical);
        REQUIRE(geometry.flipHorizontal);
        REQUIRE_FALSE(geometry.crop.rectangle);
        REQUIRE(geometry.crop.aspect == CropAspect{CropRatio{2.0}});
        REQUIRE(geometry.rotation == QuarterTurn::Clockwise180);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::CropReset);
        REQUIRE(log.entries().front().severity == Severity::Warning);
    }
    SECTION("a vertical flip already there changes nothing") {
        REQUIRE(applied(sidecar, {"--flip-vertical"}).geometry == sidecar.geometry);
    }
    SECTION("a rotation that changes frames the crop anew and reciprocates the ratio") {
        const auto geometry = applied(sidecar, {"--rotate", "270"}).geometry;
        REQUIRE(geometry.rotation == QuarterTurn::Clockwise270);
        REQUIRE(geometry.straighten == 0.0);
        REQUIRE_FALSE(geometry.crop.rectangle);
        REQUIRE(geometry.crop.aspect == CropAspect{CropRatio{0.5}});
    }
    SECTION("a half-turn keeps the ratio") {
        const auto geometry = applied(sidecar, {"--rotate", "3"}).geometry;
        REQUIRE(geometry.rotation == QuarterTurn::None);
        REQUIRE_FALSE(geometry.crop.rectangle);
        REQUIRE(geometry.crop.aspect == CropAspect{CropRatio{2.0}});
    }
    SECTION("a crop given with a flip is the crop used") {
        const auto geometry =
            applied(sidecar, {"--no-flip-vertical", "--crop", "0.2,0.2,0.4,0.3"}).geometry;
        REQUIRE(isNear(geometry.crop.rectangle, {0.2, 0.2, 0.4, 0.3}));
    }
    SECTION("a rotation that only straightens leaves the crop") {
        const auto geometry = applied(sidecar, {"--rotate", "183"}).geometry;
        REQUIRE(geometry.rotation == QuarterTurn::Clockwise180);
        REQUIRE(geometry.straighten == 3.0);
        REQUIRE(geometry.crop == sidecar.geometry.crop);
    }
    SECTION("--crop auto keeps the aspect") {
        const auto geometry = applied(sidecar, {"--crop", "auto"}).geometry;
        REQUIRE_FALSE(geometry.crop.rectangle);
        REQUIRE(geometry.crop.aspect == CropAspect{CropRatio{2.0}});
    }
}

TEST_CASE("Stale temperature and tint of a non-Custom sidecar are dropped, flags or not",
          "[cli][sidecar]") {
    DevelopSettings sidecar;
    sidecar.color.temperature = 3000.0F;
    sidecar.color.tint = 5.0F;

    const DevelopSettings result = applied(sidecar, {});

    REQUIRE_FALSE(result.color.temperature);
    REQUIRE_FALSE(result.color.tint);
}

namespace {

/// @brief Sidecar settings and marks that differ from the defaults in a few keys.
Photo editedPhoto(const std::filesystem::path& path) {
    DevelopSettings settings;
    settings.tone.exposure = 0.5F;
    settings.geometry.crop.rectangle = UprightCropRect{0.1, 0.2, 0.9, 0.8};
    return openPhoto(path)
        .with(DevelopState{settings})
        .with(PhotoMarks{.rating = 4, .label = ColorLabel::Green});
}

QJsonObject firstFile(const std::string& text) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &parseError);
    REQUIRE(parseError.error == QJsonParseError::NoError);
    return document.object().value("files").toArray().at(0).toObject();
}

} // namespace

TEST_CASE("Info shows a file without a sidecar as it opens", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    const auto result = invoke({"info", raw.string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE(result.err.empty());
    REQUIRE(result.started == std::vector{cli::ApplicationKind::Core});
    REQUIRE_THAT(result.out, StartsWith(raw.string() + "\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("size: 32 x 24"));
    REQUIRE_THAT(result.out, ContainsSubstring("orientation: normal"));
    REQUIRE_THAT(result.out, ContainsSubstring("encoding: camera"));
    REQUIRE_THAT(result.out, ContainsSubstring("sidecar: none"));
    REQUIRE_THAT(result.out, ContainsSubstring("develop settings: defaults"));
    REQUIRE_THAT(result.out, !ContainsSubstring("rating"));
    REQUIRE_THAT(result.out, !ContainsSubstring("label"));
}

TEST_CASE("Info gives no encoding for an ordinary image, whose own it cannot read", "[cli][info]") {
    const auto result = invoke({"info", test::fixture(card).string()});
    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("size: 61 x 41"));
    REQUIRE_THAT(result.out, !ContainsSubstring("encoding"));

    const auto json = invoke({"info", "--json", test::fixture(card).string()});
    REQUIRE(firstFile(json.out).value("encoding").isNull());
}

TEST_CASE("Info shows what the file records about its capture", "[cli][info][exif]") {
    const auto result = invoke({"info", test::fixture("exif-32x24.dng").string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE(result.err.empty());
    REQUIRE_THAT(result.out, ContainsSubstring("  camera: Arraw Fixture One\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  lens: Fixture 35mm F2.8\n"));
    REQUIRE_THAT(
        result.out,
        ContainsSubstring("  exposure: 1/250 s  f/2.8  ISO 400  35 mm (52 mm equivalent)\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  exposure bias: -0.33 EV\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  flash: did not fire\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  taken: 2024:05:01 10:00:00 +02:00\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  GPS: 50.0877, 14.4217, 235.5 m\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  artist: Ada Lovelace\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("  copyright: (c) 2024 Ada Lovelace\n"));
}

TEST_CASE("Info says nothing about capture information a file does not record",
          "[cli][info][exif]") {
    const auto result = invoke({"info", test::fixture(card).string()});

    REQUIRE(result.code == cli::Success);
    // No notice either: the absence is what the report shows.
    REQUIRE(result.err.empty());
    REQUIRE_THAT(result.out, !ContainsSubstring("camera:"));
    REQUIRE_THAT(result.out, !ContainsSubstring("exposure:"));
    REQUIRE_THAT(result.out, !ContainsSubstring("GPS:"));

    const auto json = invoke({"info", "--json", test::fixture(card).string()});
    REQUIRE(firstFile(json.out).value("exif").toObject().isEmpty());
}

TEST_CASE("Info --json gives the capture information with the EXIF names", "[cli][info][exif]") {
    const auto result = invoke({"info", "--json", test::fixture("exif-32x24.dng").string()});
    REQUIRE(result.code == cli::Success);
    const auto exif = firstFile(result.out).value("exif").toObject();

    REQUIRE(exif.value("make").toString() == "Arraw");
    REQUIRE(exif.value("model").toString() == "Fixture One");
    REQUIRE(exif.value("lensModel").toString() == "Fixture 35mm F2.8");
    REQUIRE(exif.value("dateTimeOriginal").toString() == "2024:05:01 10:00:00");
    REQUIRE(exif.value("offsetTimeOriginal").toString() == "+02:00");
    REQUIRE(exif.value("exposureTime").toObject().value("numerator").toInt() == 1);
    REQUIRE(exif.value("exposureTime").toObject().value("denominator").toInt() == 250);
    REQUIRE(exif.value("fNumber").toObject().value("numerator").toInt() == 28);
    REQUIRE(exif.value("fNumber").toObject().value("denominator").toInt() == 10);
    REQUIRE(exif.value("photographicSensitivity").toInt() == 400);
    REQUIRE(exif.value("focalLength").toObject().value("numerator").toInt() == 35);
    REQUIRE(exif.value("focalLengthIn35mmFilm").toInt() == 52);
    REQUIRE(exif.value("exposureBiasValue").toObject().value("numerator").toInt() == -1);
    REQUIRE(exif.value("exposureBiasValue").toObject().value("denominator").toInt() == 3);
    REQUIRE(exif.value("flash").toInt() == 16);
    REQUIRE(exif.value("gps").toObject().value("latitude").toDouble() == Catch::Approx(50.0877083));
    REQUIRE(exif.value("gps").toObject().value("longitude").toDouble() ==
            Catch::Approx(14.4216667));
    REQUIRE(exif.value("gps").toObject().value("altitude").toDouble() == Catch::Approx(235.5));
    REQUIRE(exif.value("artist").toString() == "Ada Lovelace");
    REQUIRE(exif.value("copyright").toString() == "(c) 2024 Ada Lovelace");
}

TEST_CASE("Info leaves out settings a render would not read", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;
    settings.color.temperature = 4200.0F;
    settings.color.tint = 10.0F;
    writeSidecar(openPhoto(raw).with(DevelopState{settings}));

    SECTION("not in Custom white balance") {
        const auto result = invoke({"info", raw.string()});
        REQUIRE_THAT(result.out, ContainsSubstring("develop settings: defaults"));
        REQUIRE_THAT(result.out, !ContainsSubstring("temperature"));
    }
    SECTION("in Custom white balance, a tint of zero is listed") {
        settings.color.whiteBalance = WhiteBalanceMode::Custom;
        settings.color.tint = 0.0F;
        writeSidecar(openPhoto(raw).with(DevelopState{settings}));
        const auto result = invoke({"info", raw.string()});
        REQUIRE_THAT(result.out, ContainsSubstring("    whiteBalance: "));
        REQUIRE_THAT(result.out, ContainsSubstring("    temperature: 4200\n"));
        REQUIRE_THAT(result.out, ContainsSubstring("    tint: 0\n"));
    }
    SECTION("on a non-RAW") {
        const test::TempDir pngs;
        const auto png = pngs.file("card.png");
        std::filesystem::copy_file(test::fixture(card), png);
        settings.color.whiteBalance = WhiteBalanceMode::Custom;
        writeSidecar(openPhoto(png).with(DevelopState{settings}));
        const auto result = invoke({"info", png.string()});
        REQUIRE_THAT(result.out, ContainsSubstring("whiteBalance"));
        REQUIRE_THAT(result.out, !ContainsSubstring("temperature"));
        REQUIRE_THAT(result.out, !ContainsSubstring("tint"));
    }
}

TEST_CASE("Info lists a crop aspect in each of its forms", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    DevelopSettings settings;

    SECTION("a ratio") {
        settings.geometry.crop.aspect = CropRatio{1.5};
        writeSidecar(openPhoto(raw).with(DevelopState{settings}));
        REQUIRE_THAT(invoke({"info", raw.string()}).out,
                     ContainsSubstring("    cropAspect: 1.5\n"));
        const auto json = firstFile(invoke({"info", "--json", raw.string()}).out);
        REQUIRE(json.value("settings")
                    .toObject()
                    .value("cropAspect")
                    .toObject()
                    .value("ratio")
                    .toDouble() == 1.5);
    }
    SECTION("original") {
        settings.geometry.crop.aspect = OriginalCropAspect{};
        writeSidecar(openPhoto(raw).with(DevelopState{settings}));
        REQUIRE_THAT(invoke({"info", raw.string()}).out,
                     ContainsSubstring("    cropAspect: original\n"));
    }
}

#ifndef _WIN32
TEST_CASE("Info escapes a path in JSON", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "say \"hi\".dng");
    const auto file = firstFile(invoke({"info", "--json", raw.string()}).out);
    REQUIRE(file.value("path").toString().toStdString() == raw.string());
}
#endif

TEST_CASE("Info lists only what the sidecar changes, and its marks", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    writeSidecar(editedPhoto(raw));
    const auto result = invoke({"info", raw.string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("sidecar: " + directory.file("frame.xmp").string()));
    REQUIRE_THAT(result.out, ContainsSubstring("rating: 4"));
    REQUIRE_THAT(result.out, ContainsSubstring("label: Green"));
    REQUIRE_THAT(result.out, ContainsSubstring("develop settings:\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("    exposure: 0.5\n"));
    REQUIRE_THAT(result.out, ContainsSubstring("    cropRectangle: 0.1,0.2,0.9,0.8\n"));
    REQUIRE_THAT(result.out, !ContainsSubstring("contrast"));
    REQUIRE_THAT(result.out, !ContainsSubstring("defaults"));
    REQUIRE(result.out.find("exposure") < result.out.find("cropRectangle"));
}

TEST_CASE("Info says rejected for a rating of -1", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    writeSidecar(openPhoto(raw).with(PhotoMarks{.rating = rejectedRating}));
    const auto result = invoke({"info", raw.string()});
    REQUIRE_THAT(result.out, ContainsSubstring("rating: rejected"));
}

TEST_CASE("Info --all lists every setting", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    const auto result = invoke({"info", "--all", raw.string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, !ContainsSubstring("defaults"));
    std::size_t previous = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        CAPTURE(descriptor.key);
        const auto at = result.out.find("    " + std::string(descriptor.key) + ": ");
        REQUIRE(at != std::string::npos);
        REQUIRE(at >= previous);
        previous = at;
    }
    REQUIRE(developSettingDescriptors.size() == 51);
    REQUIRE_THAT(result.out, ContainsSubstring("temperature: unset"));
}

TEST_CASE("Info names the other tools that wrote in the sidecar", "[cli][info][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    std::filesystem::copy_file(test::fixture("sidecar-foreign.xmp"), directory.file("frame.xmp"));

    const auto text = invoke({"info", raw.string()});
    REQUIRE(text.code == cli::Success);
    REQUIRE_THAT(text.out, ContainsSubstring("  other tools:\n"
                                             "    written by: Adobe Lightroom Classic 13.0 "
                                             "(Macintosh)\n"
                                             "    Adobe Camera Raw / Lightroom develop settings "
                                             "(crs:, 4 properties)\n"
                                             "    unknown (acme:, 2 properties)\n"
                                             "    Dublin Core (dc:, 1 property)\n"));

    const auto file = firstFile(invoke({"info", "--json", raw.string()}).out);
    REQUIRE(file.value("creatorTool").toString() == "Adobe Lightroom Classic 13.0 (Macintosh)");
    const auto others = file.value("others").toArray();
    REQUIRE(others.size() == 3);
    REQUIRE(others[0].toObject().value("prefix").toString() == "crs");
    REQUIRE(others[0].toObject().value("uri").toString() ==
            "http://ns.adobe.com/camera-raw-settings/1.0/");
    REQUIRE(others[0].toObject().value("properties").toInt() == 4);
    REQUIRE(others[0].toObject().value("owner").toString() ==
            "Adobe Camera Raw / Lightroom develop settings");
    REQUIRE(others[1].toObject().value("owner").isNull());

    const auto bare = invoke({"info", "--no-sidecar", raw.string()});
    REQUIRE_THAT(bare.out, !ContainsSubstring("other tools"));
    const auto bareJson = firstFile(invoke({"info", "--json", "--no-sidecar", raw.string()}).out);
    REQUIRE(bareJson.value("creatorTool").isNull());
    REQUIRE(bareJson.value("others").toArray().isEmpty());
}

TEST_CASE("Info prints no other-tools block for a sidecar only arraw wrote",
          "[cli][info][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    writeSidecar(editedPhoto(raw));
    const auto text = invoke({"info", raw.string()});
    REQUIRE_THAT(text.out, !ContainsSubstring("other tools"));
    REQUIRE_THAT(text.out, !ContainsSubstring("written by"));
    const auto file = firstFile(invoke({"info", "--json", raw.string()}).out);
    REQUIRE(file.value("creatorTool").isNull());
    REQUIRE(file.value("others").toArray().isEmpty());
}

TEST_CASE("Info --json is one document with the settings in table order", "[cli][info]") {
    const test::TempDir directory;
    const auto plain = copyRaw(directory, "plain.dng");
    const auto edited = copyRaw(directory, "edited.dng");
    writeSidecar(editedPhoto(edited));

    SECTION("a file without a sidecar") {
        const auto result = invoke({"info", "--json", plain.string()});
        REQUIRE(result.code == cli::Success);
        const auto file = firstFile(result.out);
        REQUIRE(file.value("path").toString().toStdString() == plain.string());
        REQUIRE(file.value("size").toObject().value("width").toInt() == 32);
        REQUIRE(file.value("size").toObject().value("height").toInt() == 24);
        REQUIRE(file.value("orientation").toString() == "normal");
        REQUIRE(file.value("encoding").toString() == "camera");
        REQUIRE(file.value("sidecar").isNull());
        REQUIRE(file.value("marks").toObject().value("rating").toInt() == 0);
        REQUIRE(file.value("marks").toObject().value("label").isNull());
        REQUIRE(file.value("settings").toObject().isEmpty());
    }
    SECTION("a file with one") {
        const auto result = invoke({"info", "--json", edited.string()});
        REQUIRE(result.code == cli::Success);
        const auto file = firstFile(result.out);
        REQUIRE(file.value("sidecar").toString().toStdString() ==
                directory.file("edited.xmp").string());
        REQUIRE(file.value("marks").toObject().value("rating").toInt() == 4);
        REQUIRE(file.value("marks").toObject().value("label").toString() == "Green");
        const auto settings = file.value("settings").toObject();
        REQUIRE(settings.keys().size() == 2);
        REQUIRE(settings.value("exposure").toDouble() == Catch::Approx(0.5));
        REQUIRE(settings.value("cropRectangle").toObject().value("right").toDouble() ==
                Catch::Approx(0.9));
        REQUIRE(result.out.find("\"exposure\"") < result.out.find("\"cropRectangle\""));
    }
    SECTION("--all lists every key in table order") {
        const auto result = invoke({"info", "--json", "--all", plain.string()});
        REQUIRE(result.code == cli::Success);
        REQUIRE(firstFile(result.out).value("settings").toObject().size() == 51);
        std::size_t previous = 0;
        for (const FieldDescriptor& descriptor : developSettingDescriptors) {
            const auto at = result.out.find("\"" + std::string(descriptor.key) + "\":");
            REQUIRE(at != std::string::npos);
            REQUIRE(at >= previous);
            previous = at;
        }
        REQUIRE_THAT(result.out, ContainsSubstring("\"temperature\": null"));
    }
}

TEST_CASE("Info shows the rest when one file is broken, and exits 1", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    const auto rubbish = writeRubbish(directory, "rubbish.dng");

    SECTION("text") {
        const auto result = invoke({"info", rubbish.string(), raw.string()});
        REQUIRE(result.code == cli::Failed);
        REQUIRE_THAT(result.out, StartsWith(raw.string() + "\n"));
        REQUIRE_THAT(result.err, ContainsSubstring("error:"));
        REQUIRE_THAT(result.err, ContainsSubstring("rubbish.dng"));
        REQUIRE_THAT(result.err, ContainsSubstring("1 of 2 failed"));
    }
    SECTION("JSON") {
        const auto result = invoke({"info", "--json", rubbish.string(), raw.string()});
        REQUIRE(result.code == cli::Failed);
        REQUIRE(firstFile(result.out).value("path").toString().toStdString() == raw.string());
    }
}

TEST_CASE("Info --no-sidecar shows the file as it opens without one", "[cli][info]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    writeSidecar(editedPhoto(raw));
    const auto result = invoke({"info", "--no-sidecar", raw.string()});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.out, ContainsSubstring("sidecar: " + directory.file("frame.xmp").string() +
                                               " (ignored)"));
    REQUIRE_THAT(result.out, ContainsSubstring("develop settings: defaults"));
    REQUIRE_THAT(result.out, !ContainsSubstring("rating"));

    const auto json = firstFile(invoke({"info", "--json", "--no-sidecar", raw.string()}).out);
    REQUIRE(json.value("sidecarRead").toBool() == false);
    REQUIRE(json.value("sidecar").toString().toStdString() == directory.file("frame.xmp").string());
}

TEST_CASE("Info fails a file whose sidecar is unreadable, unless --no-sidecar is given",
          "[cli][info][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    std::ofstream(directory.file("frame.xmp")) << "this is not xml <<<";

    const auto failed = invoke({"info", raw.string()});
    REQUIRE(failed.code == cli::Failed);
    REQUIRE(failed.out.empty());
    REQUIRE_THAT(failed.err, ContainsSubstring("error:"));
    REQUIRE_THAT(failed.err, ContainsSubstring("frame.xmp"));
    REQUIRE_THAT(failed.err, ContainsSubstring("--no-sidecar"));

    const auto bare = invoke({"info", "--no-sidecar", raw.string()});
    REQUIRE(bare.code == cli::Success);
    REQUIRE(bare.err.empty());
}

TEST_CASE("Info reports sidecar warnings through the log, and --quiet keeps them",
          "[cli][info][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    writeSidecar(openPhoto(raw));
    {
        std::ifstream in(directory.file("frame.xmp"));
        std::string xmp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto at = xmp.find("arraw:exposure=\"");
        REQUIRE(at != std::string::npos);
        const auto end = xmp.find('"', at + 16);
        xmp.replace(at, end - at + 1, "arraw:exposure=\"99\"");
        std::ofstream(directory.file("frame.xmp")) << xmp;
    }
    const auto text = invoke({"info", "--quiet", raw.string()});
    REQUIRE(text.code == cli::Success);
    REQUIRE_THAT(text.err, ContainsSubstring("exposure"));
    REQUIRE_THAT(text.out, ContainsSubstring("exposure: 5"));

    const auto json = invoke({"info", "--log-format", "json", raw.string()});
    REQUIRE_THAT(json.err, ContainsSubstring("setting_clamped"));
}

TEST_CASE("Info has its own help, usage errors, and writes nothing", "[cli][info]") {
    const auto help = invoke({"info", "--help"});
    REQUIRE(help.code == cli::Success);
    REQUIRE_THAT(help.out, ContainsSubstring("info <input>..."));
    REQUIRE_THAT(help.out, ContainsSubstring("--all"));
    REQUIRE_THAT(help.out, ContainsSubstring("--json"));
    REQUIRE_THAT(help.out, ContainsSubstring("--no-sidecar"));
    REQUIRE_THAT(help.out, !ContainsSubstring("--overwrite"));
    REQUIRE(help.started.empty());

    const auto top = invoke({"--help"});
    REQUIRE_THAT(top.out, ContainsSubstring("info"));
    REQUIRE(cli::findCommand("info")->run != nullptr);

    for (const auto& arguments :
         {std::vector<std::string>{"info"}, std::vector<std::string>{"info", "--nonsense", "x"},
          std::vector<std::string>{"info", "--log-format", "xml", "x"}}) {
        CAPTURE(arguments);
        const auto result = invoke(arguments);
        REQUIRE(result.code == cli::UsageError);
        REQUIRE(result.started.empty());
    }
}

namespace {

/// @brief Reads a file's bytes.
std::string bytesOf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Lists the names in a directory, sorted.
std::vector<std::string> namesIn(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

} // namespace

TEST_CASE("Info never writes or changes a sidecar", "[cli][info][sidecar]") {
    const test::TempDir directory;
    const auto raw = copyRaw(directory, "frame.dng");
    const auto before = namesIn(directory.path());
    REQUIRE(invoke({"info", raw.string()}).code == cli::Success);
    REQUIRE(namesIn(directory.path()) == before);

    writeSidecar(editedPhoto(raw));
    const auto xmp = directory.file("frame.xmp");
    const auto bytes = bytesOf(xmp);
    const auto names = namesIn(directory.path());
    for (const auto& flags :
         {std::vector<std::string>{}, {"--all"}, {"--json"}, {"--no-sidecar"}}) {
        auto arguments = std::vector<std::string>{"info"};
        arguments.insert(arguments.end(), flags.begin(), flags.end());
        arguments.push_back(raw.string());
        CAPTURE(arguments);
        REQUIRE(invoke(arguments).code == cli::Success);
        REQUIRE(bytesOf(xmp) == bytes);
        REQUIRE(namesIn(directory.path()) == names);
    }

    std::ofstream(xmp, std::ios::binary) << "this is not xml <<<";
    const auto broken = bytesOf(xmp);
    REQUIRE(invoke({"info", raw.string()}).code == cli::Failed);
    REQUIRE(invoke({"info", "--json", raw.string()}).code == cli::Failed);
    REQUIRE(bytesOf(xmp) == broken);
    REQUIRE(namesIn(directory.path()) == names);
}

namespace {

/// @brief Exports the test card as a PNG with the given extra arguments, and reads it back.
/// @return The image, or a null image if the export did not succeed.
QImage exportCard(const std::vector<std::string>& extra, int* code = nullptr) {
    const test::TempDir directory;
    std::vector<std::string> arguments{"export",   test::fixture(card).string(),
                                       "-o",       directory.path().string(),
                                       "--format", "png",
                                       "--device", "cpu"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    const auto result = invoke(arguments);
    if (code != nullptr) {
        *code = result.code;
    }
    return QImage(QString::fromStdString(directory.file("testcard-61x41-srgb8.png").string()));
}

} // namespace

TEST_CASE("Each form of --resize gives the output size ADR 007 describes", "[cli][resize]") {
    const auto geometry =
        GENERATE(std::vector<std::string>{}, std::vector<std::string>{"--rotate", "90"},
                 std::vector<std::string>{"--crop", "0.1,0.2,0.8,0.9"},
                 std::vector<std::string>{"--rotate", "90", "--crop", "0.1,0.2,0.8,0.9"});
    const auto spec = GENERATE(as<std::string>{}, "30", "40x40", "100x20", "50%", "12.5%", "200%",
                               "100", "500x500");
    const bool upscale = GENERATE(false, true);
    CAPTURE(geometry, spec, upscale);

    const QImage whole = exportCard(geometry);
    REQUIRE_FALSE(whole.isNull());
    std::vector<std::string> extra = geometry;
    extra.insert(extra.end(), {"--resize", spec});
    if (upscale) {
        extra.emplace_back("--allow-upscale");
    }
    const QImage resized = exportCard(extra);
    REQUIRE_FALSE(resized.isNull());

    RenderRequest request;
    request.size = cli::parseResize(spec);
    request.upscale = upscale ? Upscale::Allowed : Upscale::Never;
    const ImageSize expected = resolvedSize(request, {static_cast<std::uint32_t>(whole.width()),
                                                      static_cast<std::uint32_t>(whole.height())});
    REQUIRE(resized.width() == static_cast<int>(expected.width));
    REQUIRE(resized.height() == static_cast<int>(expected.height));
}

TEST_CASE("Resize options without a size say they do nothing", "[cli][resize]") {
    const test::TempDir directory;
    const auto result =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string(), "--format",
                "png", "--device", "cpu", "--allow-upscale", "--resize-filter", "bilinear"});

    REQUIRE(result.code == cli::Success);
    REQUIRE_THAT(result.err, ContainsSubstring("--allow-upscale does nothing without --resize"));
    REQUIRE_THAT(result.err, ContainsSubstring("--resize-filter does nothing without --resize"));

    const auto quiet =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string(), "--format",
                "png", "--device", "cpu", "--overwrite", "--resize", "20", "--allow-upscale"});
    REQUIRE_THAT(quiet.err, !ContainsSubstring("does nothing"));
}

TEST_CASE("Resizing gives the sizes written out in ADR 007 for the test card", "[cli][resize]") {
    const auto sized = [](const std::vector<std::string>& extra) {
        const QImage image = exportCard(extra);
        return std::pair{image.width(), image.height()};
    };
    using Size = std::pair<int, int>;
    /// The card is 61x41, a landscape frame; rotated, a portrait one of 41x61.
    REQUIRE(sized({"--resize", "30"}) == Size{30, 20});
    REQUIRE(sized({"--rotate", "90", "--resize", "30"}) == Size{20, 30});
    REQUIRE(sized({"--resize", "40x40"}) == Size{40, 27});
    REQUIRE(sized({"--resize", "100x20"}) == Size{30, 20});
    REQUIRE(sized({"--resize", "50%"}) == Size{31, 21});
    REQUIRE(sized({"--resize", "50.5%"}) == Size{31, 21});
    /// Shrink only: a size past the photograph's own is its own size.
    REQUIRE(sized({"--resize", "500"}) == Size{61, 41});
    REQUIRE(sized({"--resize", "200%"}) == Size{61, 41});
    REQUIRE(sized({"--resize", "122", "--allow-upscale"}) == Size{122, 82});
    REQUIRE(sized({"--resize", "200%", "--allow-upscale"}) == Size{122, 82});
}

TEST_CASE("The resize filter changes the pixels, lanczos by default", "[cli][resize]") {
    const QImage byDefault = exportCard({"--resize", "20"});
    const QImage lanczos = exportCard({"--resize", "20", "--resize-filter", "lanczos"});
    const QImage bilinear = exportCard({"--resize", "20", "--resize-filter", "bilinear"});
    REQUIRE_FALSE(byDefault.isNull());
    REQUIRE(byDefault == lanczos);
    REQUIRE(bilinear.size() == lanczos.size());
    REQUIRE(bilinear != lanczos);
}

TEST_CASE("The resize flags are documented", "[cli][resize]") {
    const auto help = invoke({"export", "--help"});
    for (const char* name : {"--resize ", "--allow-upscale", "--resize-filter"}) {
        REQUIRE_THAT(help.out, ContainsSubstring(name));
    }
}

TEST_CASE("The forms of --resize are parsed by shape", "[cli][resize]") {
    using Box = RenderRequest::FitInside;
    using Scale = RenderRequest::Scale;
    REQUIRE(std::get<Box>(cli::parseResize("2048")).width == 2048);
    REQUIRE(std::get<Box>(cli::parseResize("2048")).height == 2048);
    REQUIRE(std::get<Box>(cli::parseResize("2048x1365")).height == 1365);
    REQUIRE(std::get<Box>(cli::parseResize("1X2")).width == 1);
    REQUIRE(std::get<Scale>(cli::parseResize("50%")).factor == Catch::Approx(0.5));
    REQUIRE(std::get<Scale>(cli::parseResize("12.5%")).factor == Catch::Approx(0.125));
    REQUIRE(std::get<Scale>(cli::parseResize("150%")).factor == Catch::Approx(1.5));
}

TEST_CASE("A malformed --resize is a usage error before any file is touched", "[cli][resize]") {
    const test::TempDir directory;
    const auto spec =
        GENERATE("0", "-5", "-5%", "0%", "0.0%", "%", "x", "100x", "x100", "0x100", "100x0",
                 "100x-5", "1.5", "nan", "inf", "nan%", "inf%", "1e999%", "abc", "", "10%%",
                 "5.5.5%", "99999999999", "10x20x30", "0x10%", " 100", "+100", "0x20");
    CAPTURE(spec);
    const auto result = invoke({"export", test::fixture(card).string(), "-o",
                                directory.path().string(), "--resize", spec});
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("--resize"));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("An unknown --resize-filter is a usage error", "[cli][resize]") {
    const test::TempDir directory;
    const auto result =
        invoke({"export", test::fixture(card).string(), "-o", directory.path().string(), "--resize",
                "20", "--resize-filter", "nearest"});
    REQUIRE(result.code == cli::UsageError);
    REQUIRE_THAT(result.err, ContainsSubstring("--resize-filter"));
    REQUIRE(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("Info is unaffected by resizing flags", "[cli][resize]") {
    const auto result = invoke({"info", test::fixture(card).string()});
    REQUIRE(result.code == cli::Success);
}
