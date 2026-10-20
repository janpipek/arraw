#include "LensOptions.h"

#include "Command.h"

#include <ColorEncoding.h>
#include <LensResolution.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace arraw;

namespace {

/// @brief Reads the amount of a `vignetting=N` item.
/// @return The amount, or nothing when @p text is not a decimal in the amount's range.
std::optional<float> parseAmount(std::string_view text) {
    float amount = 0.0F;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, amount);
    if (text.empty() || error != std::errc{} || stop != end || !std::isfinite(amount) ||
        amount < minimumVignettingAmount || amount > maximumVignettingAmount) {
        return std::nullopt;
    }
    return amount;
}

/// @brief Reads the list `--lens-corrections` takes.
/// @param list The option's value.
/// @param error Set to the reason when nothing is returned.
/// @return The corrections it names, with no lens, or nothing after setting @p error.
std::optional<LensSettings> parseCorrections(const std::string& list, std::string& error) {
    static const std::string usage =
        "--lens-corrections takes all, none, or a comma-separated list of distortion, "
        "vignetting[=amount] and ca";
    std::string text = list;
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    LensSettings settings;
    bool distortion = false;
    bool vignetting = false;
    bool ca = false;
    bool group = false; // all or none seen
    int items = 0;
    std::size_t start = 0;
    while (true) {
        const auto comma = text.find(',', start);
        const std::string name =
            text.substr(start, comma == std::string::npos ? comma : comma - start);
        ++items;
        const auto fail = [&](const std::string& why) {
            error = usage + ": " + why;
            return std::nullopt;
        };
        const auto quoted = "'" + name + "'";
        if (name.empty()) {
            return fail("an item is empty");
        }
        if (name == "all" || name == "none") {
            group = true;
            if (name == "all") {
                settings.distortion = true;
                settings.vignettingAmount = profileVignettingAmount;
                settings.lateralCa = true;
            }
        } else if (name == "distortion" || name == "ca" || name == "vignetting" ||
                   name.starts_with("vignetting=")) {
            const bool seen = name == "distortion" ? distortion : name == "ca" ? ca : vignetting;
            if (seen) {
                return fail("'" + name.substr(0, name.find('=')) + "' is given twice");
            }
            if (name == "distortion") {
                distortion = settings.distortion = true;
            } else if (name == "ca") {
                ca = settings.lateralCa = true;
            } else {
                vignetting = true;
                if (name == "vignetting") {
                    settings.vignettingAmount = profileVignettingAmount;
                } else {
                    const auto amount = parseAmount(std::string_view(name).substr(11));
                    if (!amount) {
                        return fail("vignetting takes an amount from 0 to 150, as in "
                                    "vignetting=60: " +
                                    quoted + " is not one");
                    }
                    settings.vignettingAmount = *amount;
                }
            }
        } else {
            return fail(quoted + " is not a correction");
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (group && items > 1) {
        error = usage + ": all and none stand alone";
        return std::nullopt;
    }
    return settings;
}

} // namespace

void cli::addLensOptions(QCommandLineParser& parser, bool edits) {
    if (edits) {
        parser.addOption(
            {"lens-corrections",
             "Which lens corrections a RAW gets, from its lens profile: distortion, "
             "vignetting[=amount] and ca (lateral chromatic aberration) joined by commas, or "
             "all, or none. The vignetting amount is a percentage of the profile's correction "
             "from 0 to 150; bare vignetting and all mean 100, and 0 turns it off. Default: "
             "the sidecar's, else none. Other files ignore it.",
             "list"});
        parser.addOption({"lens",
                          "The lens to take the profile of, in place of the recorded or the "
                          "found one: 'Maker|Model', as `arraw-cli lenses` spells them.",
                          "maker|model"});
    }
    parser.addOption({"lens-database",
                      "Directory of lensfun XML files to look lenses up in, in place of the "
                      "installed database.",
                      "dir"});
}

std::optional<cli::LensOptions> cli::readLensOptions(const QCommandLineParser& parser, bool edits,
                                                     std::string_view command, std::ostream& err,
                                                     int& code) {
    LensOptions options;
    if (edits && parser.isSet("lens-corrections")) {
        std::string error;
        options.corrections =
            parseCorrections(parser.value("lens-corrections").toStdString(), error);
        if (!options.corrections) {
            code = commandUsageError(err, command, error);
            return std::nullopt;
        }
    }
    if (edits && parser.isSet("lens")) {
        const std::string text = parser.value("lens").toStdString();
        const auto bar = text.find('|');
        if (bar == std::string::npos || bar + 1 == text.size()) {
            code = commandUsageError(err, command,
                                     "--lens takes 'Maker|Model', as `arraw-cli lenses` spells "
                                     "a lens: '" +
                                         text + "' has no model");
            return std::nullopt;
        }
        options.lens = LensIdentity{text.substr(0, bar), text.substr(bar + 1)};
    }
    if (parser.isSet("lens-database")) {
        options.database = std::filesystem::path(parser.value("lens-database").toStdString());
    }
    return options;
}

cli::LensDatabaseSource::LensDatabaseSource(std::optional<std::filesystem::path> directory)
    : directory_(std::move(directory)) {}

const LensDatabase& cli::LensDatabaseSource::get() {
    if (!directory_) {
        return LensDatabase::system();
    }
    if (!loaded_) {
        loaded_.emplace(LensDatabase::fromDirectory(*directory_));
    }
    return *loaded_;
}

Photo cli::applyLensOptions(const Photo& photo, const LensOptions& options,
                            LensDatabaseSource& database) {
    if (std::holds_alternative<NamedEncoding>(photo.metadata().encoding)) {
        return photo;
    }
    Photo result = photo;
    if (options.corrections) {
        NegativeSettings negative = result.negative();
        negative.lens = withCorrectionsOf(negative.lens, *options.corrections);
        result = result.with(std::move(negative));
    }
    if (options.lens) {
        const auto chosen = withChosenLens(result, *options.lens, database.get());
        if (!chosen) {
            throw std::runtime_error("--lens: '" + options.lens->maker + "|" + options.lens->model +
                                     "' is not in the lens database");
        }
        result = *chosen;
    }
    return result;
}
