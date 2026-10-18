#include "LensOptions.h"

#include "Command.h"

#include <ColorEncoding.h>
#include <LensResolution.h>

#include <algorithm>
#include <cctype>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

using namespace arraw;

namespace {

/// @brief Reads the list `--lens-corrections` takes.
/// @return The corrections it names, or nothing for an unknown name or an empty list.
std::optional<LensKinds> parseCorrections(const std::string& list) {
    std::string text = list;
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "all") {
        return LensKinds{true, true, true};
    }
    LensKinds kinds;
    if (text == "none") {
        return kinds;
    }
    std::size_t start = 0;
    while (true) {
        const auto comma = text.find(',', start);
        const auto name = text.substr(start, comma == std::string::npos ? comma : comma - start);
        if (name == "distortion") {
            kinds.distortion = true;
        } else if (name == "vignetting") {
            kinds.vignetting = true;
        } else if (name == "ca") {
            kinds.lateralCa = true;
        } else {
            return std::nullopt;
        }
        if (comma == std::string::npos) {
            return kinds;
        }
        start = comma + 1;
    }
}

} // namespace

void cli::addLensOptions(QCommandLineParser& parser, bool edits) {
    if (edits) {
        parser.addOption(
            {"lens-corrections",
             "Which lens corrections a RAW gets, from its lens profile: distortion, vignetting "
             "and ca (lateral chromatic aberration) joined by commas, or all, or none. Default: "
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
        options.corrections = parseCorrections(parser.value("lens-corrections").toStdString());
        if (!options.corrections) {
            code = commandUsageError(err, command,
                                     "--lens-corrections takes all, none, or a comma-separated "
                                     "list of distortion, vignetting and ca");
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
        negative.lens.distortion = options.corrections->distortion;
        negative.lens.vignetting = options.corrections->vignetting;
        negative.lens.lateralCa = options.corrections->lateralCa;
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
