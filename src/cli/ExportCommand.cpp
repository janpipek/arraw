#include "ExportCommand.h"

#include "Cli.h"
#include "Command.h"
#include "DeviceChoice.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "ProcessingPlan.h"
#include "SettingCodec.h"
#include "ShotInputs.h"
#include "SidecarWatch.h"
#include "StreamDiagnostics.h"
#include "TerminalStyle.h"
#include "TimingTrace.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <Diagnostics.h>
#include <Edits.h>
#include <EffectsSettings.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <MarksFilter.h>
#include <NoiseReductionSettings.h>
#include <Photo.h>
#include <SettingDescriptors.h>
#include <ShortestDecimal.h>
#include <ToneCurveSettings.h>
#include <WhiteBalance.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;

void cli::setRotationAngle(GeometrySettings& geometry, double degrees) {
    if (!std::isfinite(degrees)) {
        throw std::invalid_argument("A rotation angle must be finite");
    }
    const double wrapped = std::fmod(degrees, 360.0);
    const int turns = static_cast<int>(std::round(wrapped / 90.0));
    constexpr QuarterTurn rotations[]{QuarterTurn::None, QuarterTurn::Clockwise90,
                                      QuarterTurn::Clockwise180, QuarterTurn::Clockwise270};
    geometry.rotation = rotations[(turns % 4 + 4) % 4];
    geometry.straighten = wrapped - static_cast<double>(turns) * 90.0;
}

bool cli::isRangedFloatSetting(const FieldDescriptor& descriptor) {
    return descriptor.range &&
           (std::holds_alternative<float& (*)(DevelopSettings&)>(descriptor.member) ||
            std::holds_alternative<std::optional<float>& (*)(DevelopSettings&)>(descriptor.member));
}

namespace {

/// @brief Gives the key a path is compared by: canonical where it can be, else lexically normal.
std::filesystem::path pathKey(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path key = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : key;
}

/// @brief Finds the file among some that a path names.
///
/// By key first; then, when the path exists, by asking the file system, which
/// also sees a name that differs only in case on a case-insensitive one, or a
/// hard link. A batch is small, so the second pass's cost does not matter.
/// @param path Path to look for.
/// @param files The files, by ::pathKey, each with the path to report.
/// @return The reported path of the file @p path names, or nothing.
std::optional<std::filesystem::path>
sameFileIn(const std::filesystem::path& path,
           const std::map<std::filesystem::path, std::filesystem::path>& files) {
    if (const auto found = files.find(pathKey(path)); found != files.end()) {
        return found->second;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return std::nullopt;
    }
    for (const auto& [key, reported] : files) {
        if (isSameFile(path, key)) {
            return reported;
        }
    }
    return std::nullopt;
}

using cli::CropEdit;
using cli::GeometryEdits;
using cli::SettingEdit;

/// @brief File extension a format is written with.
std::string_view extensionFor(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Png:
        return ".png";
    case ImageFileFormat::Jpeg:
        return ".jpg";
    case ImageFileFormat::Tiff:
        return ".tif";
    }
    return ".bin";
}

/// @brief Finds the row of the settings table a key names.
/// @throws std::logic_error if no row has the key, which is a misspelling in this file.
const FieldDescriptor& descriptorFor(std::string_view key) {
    const FieldDescriptor* descriptor = findDescriptor(key);
    if (descriptor == nullptr) {
        throw std::logic_error("no setting is keyed '" + std::string(key) + "'");
    }
    return *descriptor;
}

/// @brief Records the value a field of @p source now holds as an edit.
/// @param edits List to append to.
/// @param key Key of the field, which must name a row.
/// @param source Settings holding the value the flags gave.
void addEdit(std::vector<SettingEdit>& edits, std::string_view key, const DevelopSettings& source) {
    const FieldDescriptor& descriptor = descriptorFor(key);
    edits.push_back({&descriptor, encode(descriptor, source)});
}

/// @brief Reads a side of a `--resize` box: digits only, at least 1, within 32 bits.
/// @throws std::invalid_argument if @p text is not one.
std::uint32_t parseSide(std::string_view text, std::string_view spec) {
    const bool digits =
        !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
    std::uint32_t value = 0;
    if (!digits ||
        std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
        throw std::invalid_argument("--resize: '" + std::string(spec) +
                                    "' is not a long edge (2048), a box (2048x1365) or a "
                                    "percentage (50%); sizes are whole numbers of pixels up to " +
                                    std::to_string(std::numeric_limits<std::uint32_t>::max()));
    }
    if (value == 0) {
        throw std::invalid_argument("--resize: '" + std::string(spec) + "' must be greater than 0");
    }
    return value;
}

/// @brief Everything the command needs, once its arguments are understood.
struct ExportRequest {
    std::vector<std::filesystem::path> inputs;
    /// @brief Which photographs are wanted by their marks; inactive wants all.
    MarksFilter filter;
    std::filesystem::path outputDirectory;
    ImageFileFormat format = ImageFileFormat::Jpeg;
    /// @brief What the flags said, applied over each photograph's own settings.
    cli::ExportEdits edits;
    /// @brief Whether each photograph's sidecar is read; false is `--no-sidecar`.
    bool useSidecars = true;
    ExportOptions options;
    /// @brief Metadata groups each export carries from its photograph (ADR 032).
    MetadataSelection metadata;
    /// @brief Size and filter of every render, built once for both devices.
    RenderRequest render;
    cli::DeviceChoice device;
    GpuBackend backend = defaultGpuBackend();
    bool allowSoftware = false;
    bool overwrite = false;
    bool quiet = false;
    cli::LogFormat logFormat = cli::LogFormat::Text;
    /// @brief Options given that do nothing without `--resize`, reported once the log exists.
    std::vector<std::string> ignoredResizeOptions;
};

/// @brief Reads the list `--metadata` takes.
/// @return The groups it names, or nothing for an unknown name or an empty list.
std::optional<MetadataSelection> parseMetadataSelection(const std::string& list) {
    MetadataSelection selection{false, false, false};
    std::string text = list;
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "all") {
        return MetadataSelection{true, true, true};
    }
    if (text == "none") {
        return selection;
    }
    std::size_t start = 0;
    while (true) {
        const auto comma = text.find(',', start);
        const auto name = text.substr(start, comma == std::string::npos ? comma : comma - start);
        if (name == "capture") {
            selection.capture = true;
        } else if (name == "location") {
            selection.location = true;
        } else if (name == "descriptive") {
            selection.descriptive = true;
        } else {
            return std::nullopt;
        }
        if (comma == std::string::npos) {
            return selection;
        }
        start = comma + 1;
    }
}

/// @brief Which filter `--resize-filter` names, if any.
std::optional<ResizeFilter> parseResizeFilter(const std::string& name) {
    if (name == "lanczos") {
        return ResizeFilter::Lanczos3;
    }
    if (name == "bilinear") {
        return ResizeFilter::Bilinear;
    }
    return std::nullopt;
}

/// @brief Reports a usage problem and the exit code that goes with it.
int usageError(std::ostream& err, const std::string& message) {
    return cli::commandUsageError(err, "export", message);
}

/// @brief Reads a named option as an integer.
/// @return `true` if the option was absent or parsed; `false` if it was malformed.
bool readInteger(const QCommandLineParser& parser, const char* name, int& value) {
    if (!parser.isSet(name)) {
        return true;
    }
    bool valid = false;
    const int parsed = parser.value(name).toInt(&valid);
    if (valid) {
        value = parsed;
    }
    return valid;
}

/// @brief Help wording of one ranged setting; its name and limits come from the descriptor table.
struct SettingHelp {
    std::string_view key;
    const char* valueName;
    const char* description;
    const char* note;
};

/// @brief Help wording for the ranged settings, keyed like ::arraw::developSettingDescriptors.
constexpr SettingHelp settingHelp[]{
    {"exposure", "stops", "Exposure adjustment in EV", ""},
    {"contrast", "amount", "Contrast", ""},
    {"shadows", "amount", "Lift or deepen the dark tones", ""},
    {"highlights", "amount", "Recover or raise the bright tones", ""},
    {"blacks", "amount", "Move the black point", ""},
    {"whites", "amount", "Move the white point", ""},
    {"temperature", "k", "White balance in kelvin", " RAW only."},
    {"tint", "amount", "Green to magenta", " RAW only."},
    {"filmicHighlights", "amount", "Highlight roll-off", " Default: 25."},
    {"texture", "amount", "Emphasise (+) or smooth (-) fine detail", ""},
    {"clarity", "amount", "Add (+) or take away (-) mid-scale local contrast", ""},
    {"dehaze", "amount", "Remove (+) or add (-) atmospheric haze", ""},
    {"toneCurveLuma", "points", "Tone curve of the luminance", ""},
    {"toneCurveRed", "points", "Tone curve of the red channel", ""},
    {"toneCurveGreen", "points", "Tone curve of the green channel", ""},
    {"toneCurveBlue", "points", "Tone curve of the blue channel", ""},
    {"saturation", "amount", "Colourfulness of every colour", ""},
    {"vibrance", "amount", "Colourfulness of the muted colours, sparing the vivid", ""},
    {"hueRed", "amount", "Shift the hue of reds", ""},
    {"saturationRed", "amount", "Colourfulness of reds", ""},
    {"luminanceRed", "amount", "Lightness of reds", ""},
    {"hueOrange", "amount", "Shift the hue of oranges", ""},
    {"saturationOrange", "amount", "Colourfulness of oranges", ""},
    {"luminanceOrange", "amount", "Lightness of oranges", ""},
    {"hueYellow", "amount", "Shift the hue of yellows", ""},
    {"saturationYellow", "amount", "Colourfulness of yellows", ""},
    {"luminanceYellow", "amount", "Lightness of yellows", ""},
    {"hueGreen", "amount", "Shift the hue of greens", ""},
    {"saturationGreen", "amount", "Colourfulness of greens", ""},
    {"luminanceGreen", "amount", "Lightness of greens", ""},
    {"hueAqua", "amount", "Shift the hue of aquas", ""},
    {"saturationAqua", "amount", "Colourfulness of aquas", ""},
    {"luminanceAqua", "amount", "Lightness of aquas", ""},
    {"hueBlue", "amount", "Shift the hue of blues", ""},
    {"saturationBlue", "amount", "Colourfulness of blues", ""},
    {"luminanceBlue", "amount", "Lightness of blues", ""},
    {"huePurple", "amount", "Shift the hue of purples", ""},
    {"saturationPurple", "amount", "Colourfulness of purples", ""},
    {"luminancePurple", "amount", "Lightness of purples", ""},
    {"hueMagenta", "amount", "Shift the hue of magentas", ""},
    {"saturationMagenta", "amount", "Colourfulness of magentas", ""},
    {"luminanceMagenta", "amount", "Lightness of magentas", ""},
    {"grayRed", "amount", "Lightness of reds in black and white", ""},
    {"grayOrange", "amount", "Lightness of oranges in black and white", ""},
    {"grayYellow", "amount", "Lightness of yellows in black and white", ""},
    {"grayGreen", "amount", "Lightness of greens in black and white", ""},
    {"grayAqua", "amount", "Lightness of aquas in black and white", ""},
    {"grayBlue", "amount", "Lightness of blues in black and white", ""},
    {"grayPurple", "amount", "Lightness of purples in black and white", ""},
    {"grayMagenta", "amount", "Lightness of magentas in black and white", ""},
    {"gradeShadowHue", "degrees", "Hue the shadows are tinted toward", ""},
    {"gradeShadowSaturation", "amount", "Strength of the shadows' tint", ""},
    {"gradeMidtoneHue", "degrees", "Hue the midtones are tinted toward", ""},
    {"gradeMidtoneSaturation", "amount", "Strength of the midtones' tint", ""},
    {"gradeHighlightHue", "degrees", "Hue the highlights are tinted toward", ""},
    {"gradeHighlightSaturation", "amount", "Strength of the highlights' tint", ""},
    {"gradeBalance", "amount", "Give the tint's range to the shadows (-) or the highlights (+)",
     ""},
    {"gradeBlending", "amount", "Softness of the transitions between the tinted zones",
     " Default: 50."},
    {"vignetteAmount", "amount", "Darken (-) or lighten (+) the edges of the cropped frame", ""},
    {"vignetteMidpoint", "amount", "Where the vignette's falloff begins, from the centre outward",
     " Default: 50."},
    {"vignetteFeather", "amount", "Softness of the vignette's falloff, 0 for a hard edge",
     " Default: 50."},
    {"grainAmount", "amount", "Strength of the film grain", ""},
    {"grainSize", "amount", "Size of the grain, relative to the cropped frame's long edge",
     " Default: 50."},
    {"grainRoughness", "amount", "How clumped the grain is, 0 for even", " Default: 50."},
    {"luminanceNoiseReduction", "amount", "Strength of the luminance noise smoothing", ""},
    {"luminanceNoiseDetail", "amount",
     "How much edge the luminance smoothing keeps, 0 smooths across most", " Default: 50."},
    {"colorNoiseReduction", "amount", "Strength of the colour noise smoothing", ""},
    {"colorNoiseSmoothness", "amount", "Size of the colour blotches smoothed", " Default: 50."},
};

/// @brief Finds the help wording of a setting.
const SettingHelp* helpFor(std::string_view key) {
    for (const SettingHelp& help : settingHelp) {
        if (help.key == key) {
            return &help;
        }
    }
    return nullptr;
}

/// @brief Spells a camelCase key as a command-line option name.
std::string optionName(std::string_view key) {
    std::string name;
    for (const char c : key) {
        if (c >= 'A' && c <= 'Z') {
            name += '-';
            name += static_cast<char>(c - 'A' + 'a');
        } else {
            name += c;
        }
    }
    return name;
}

/// @brief Whether a row is a plain number setting that gets an option of its own.
bool isNumericOption(const FieldDescriptor& descriptor) {
    return cli::isRangedFloatSetting(descriptor) && helpFor(descriptor.key);
}

/// @brief Spells a limit the way the help and the errors show it.
std::string limit(double value) {
    return QString::number(value).toStdString();
}

/// @brief Reads every numeric setting option into edits.
///
/// Names and limits come from ::arraw::developSettingDescriptors. Out of range
/// is refused rather than clamped: a photographer is present to be told, and
/// nothing invalid should enter a session (ADR 008). The renderer clamps as
/// well, for values that arrive from a file instead.
/// @return `true` if every option present was acceptable; `false` otherwise.
bool readSettings(const QCommandLineParser& parser, std::vector<SettingEdit>& edits,
                  std::ostream& err, int& code) {
    DevelopSettings given;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!isNumericOption(descriptor)) {
            continue;
        }
        const std::string name = optionName(descriptor.key);
        if (!parser.isSet(QString::fromStdString(name))) {
            continue;
        }
        bool valid = false;
        const float parsed = parser.value(QString::fromStdString(name)).toFloat(&valid);
        if (!valid || !std::isfinite(parsed)) {
            code = usageError(err, "--" + name + " takes a finite number");
            return false;
        }
        if (parsed < descriptor.range->minimum || parsed > descriptor.range->maximum) {
            code = usageError(err, "--" + name + " accepts " + limit(descriptor.range->minimum) +
                                       " to " + limit(descriptor.range->maximum));
            return false;
        }
        visitField(descriptor, given, [&](auto& field) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, float> ||
                          std::is_same_v<std::remove_cvref_t<decltype(field)>,
                                         std::optional<float>>) {
                field = parsed;
            }
        });
        addEdit(edits, descriptor.key, given);
    }
    return true;
}

/// @brief Reads the tone curve options into edits.
///
/// A curve is "x,y;x,y;..." as in a sidecar: two to sixteen points with
/// coordinates from 0 to 1, x at least ::arraw::minimumCurvePointSpacing apart,
/// the first at x = 0 and the last at x = 1, in any order (sorted, and ends
/// snapped, by ::arraw::curveFromPoints). Anything else is refused rather than
/// repaired, as out-of-range numbers are.
/// @return `true` if every curve option present was acceptable; `false` otherwise.
bool readCurves(const QCommandLineParser& parser, std::vector<SettingEdit>& edits,
                std::ostream& err, int& code) {
    DevelopSettings given;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!takesPoints(descriptor)) {
            continue;
        }
        const std::string name = optionName(descriptor.key);
        if (!parser.isSet(QString::fromStdString(name))) {
            continue;
        }
        const std::string wording = "--" + name + " takes " + toneCurveRequirements +
                                    ", as x,y joined by semicolons, for example 0,0;0.5,0.6;1,1";
        std::vector<CurvePoint> points;
        bool valid = false;
        if (const auto parsed =
                parsePointList(parser.value(QString::fromStdString(name)).toStdString())) {
            valid = std::ranges::all_of(*parsed, [](const auto& point) {
                return std::isfinite(point.first) && std::isfinite(point.second) &&
                       point.first >= 0.0 && point.first <= 1.0 && point.second >= 0.0 &&
                       point.second <= 1.0;
            });
            for (const auto& [x, y] : *parsed) {
                points.push_back({shortestFloat(x), shortestFloat(y)});
            }
        }
        const std::optional<ToneCurve> curve =
            valid ? curveFromPoints(std::move(points)) : std::nullopt;
        if (!curve) {
            code = usageError(err, wording);
            return false;
        }
        visitField(descriptor, given, [&](auto& field) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, ToneCurve>) {
                field = *curve;
            }
        });
        addEdit(edits, descriptor.key, given);
    }
    return true;
}

/// @brief Reads the geometry options into edits, without resolving or executing any transforms.
bool readGeometry(const QCommandLineParser& parser, GeometryEdits& edits, std::ostream& err,
                  int& code) {
    if (parser.isSet("rotate")) {
        bool valid = false;
        const double degrees = parser.value("rotate").toDouble(&valid);
        if (!valid || !std::isfinite(degrees)) {
            code = usageError(err, "--rotate takes a finite angle in clockwise degrees");
            return false;
        }
        edits.rotate = degrees;
    }
    const auto readFlip = [&](const char* on, const char* off, std::optional<bool>& flip) {
        if (parser.isSet(on) && parser.isSet(off)) {
            code = usageError(err, std::string("--") + on + " and --" + off + " contradict");
            return false;
        }
        if (parser.isSet(on) || parser.isSet(off)) {
            flip = parser.isSet(on);
        }
        return true;
    };
    if (!readFlip("flip-horizontal", "no-flip-horizontal", edits.flipHorizontal) ||
        !readFlip("flip-vertical", "no-flip-vertical", edits.flipVertical)) {
        return false;
    }
    if (parser.isSet("crop")) {
        const auto value = parser.value("crop").trimmed().toLower();
        CropEdit crop;
        if (value != "auto") {
            const auto edges = value.split(',');
            UprightCropRect rectangle;
            double* destinations[]{&rectangle.left, &rectangle.top, &rectangle.right,
                                   &rectangle.bottom};
            bool valid = edges.size() == 4;
            if (valid) {
                for (int index = 0; index < 4; ++index) {
                    bool parsed = false;
                    const double edge = edges[index].toDouble(&parsed);
                    valid = valid && parsed && std::isfinite(edge) && edge >= 0.0 && edge <= 1.0;
                    *destinations[index] = edge;
                }
            }
            if (!valid || rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom) {
                code = usageError(err, "--crop takes auto or left,top,right,bottom with finite "
                                       "edges from 0 to 1, left < right and top < bottom");
                return false;
            }
            crop.rectangle = rectangle;
        }
        edits.crop = crop;
    }
    if (parser.isSet("crop-aspect")) {
        const auto value = parser.value("crop-aspect").trimmed().toLower();
        if (value == "free") {
            edits.aspect = FreeCropAspect{};
        } else if (value == "original") {
            edits.aspect = OriginalCropAspect{};
        } else {
            const auto parts = value.split(':');
            bool validWidth = false;
            bool validHeight = false;
            const double width = parts.size() == 2 ? parts[0].toDouble(&validWidth) : 0.0;
            const double height = parts.size() == 2 ? parts[1].toDouble(&validHeight) : 0.0;
            const double ratio = height > 0.0 ? width / height : 0.0;
            if (!validWidth || !validHeight || !std::isfinite(width) || !std::isfinite(height) ||
                width <= 0.0 || height <= 0.0 || !std::isfinite(ratio) || ratio <= 0.0) {
                code = usageError(err, "--crop-aspect takes free, original, or positive finite "
                                       "width:height, for example 3:2 or 2:3");
                return false;
            }
            edits.aspect = CropRatio{ratio};
        }
    }
    return true;
}

/// @brief Reads every develop option, the white balance's included, into edits.
bool readEdits(const QCommandLineParser& parser, cli::ExportEdits& edits, std::ostream& err,
               int& code) {
    if (!readSettings(parser, edits.settings, err, code) ||
        !readCurves(parser, edits.settings, err, code)) {
        return false;
    }
    DevelopSettings given;
    // Naming either half of a white balance is asking for a custom one; the
    // half left unnamed stays as the photograph has it, or as the camera
    // recorded it.
    if (parser.isSet("temperature") || parser.isSet("tint")) {
        given.color.whiteBalance = WhiteBalanceMode::Custom;
        addEdit(edits.settings, "whiteBalance", given);
    }
    if (parser.isSet("white-balance")) {
        const auto mode = parser.value("white-balance").toLower();
        if (mode == "as-shot") {
            if (parser.isSet("temperature") || parser.isSet("tint")) {
                code = usageError(err, "--white-balance as-shot cannot be combined with "
                                       "--temperature or --tint");
                return false;
            }
            // Clears both halves, so nothing a sidecar kept in them survives.
            addEdit(edits.settings, "whiteBalance", given);
            addEdit(edits.settings, "temperature", given);
            addEdit(edits.settings, "tint", given);
        } else if (mode == "custom") {
            given.color.whiteBalance = WhiteBalanceMode::Custom;
            addEdit(edits.settings, "whiteBalance", given);
        } else {
            code = usageError(err, "--white-balance takes as-shot or custom");
            return false;
        }
    }
    if (parser.isSet("grain-seed")) {
        // A seed names a pattern, so only an exact whole number is taken.
        bool valid = false;
        const qulonglong seed = parser.value("grain-seed").trimmed().toULongLong(&valid);
        if (!valid || seed > std::numeric_limits<std::uint32_t>::max()) {
            code = usageError(err, "--grain-seed takes a whole number from 0 to " +
                                       std::to_string(std::numeric_limits<std::uint32_t>::max()));
            return false;
        }
        given.effects.grain.seed = static_cast<std::uint32_t>(seed);
        addEdit(edits.settings, "grainSeed", given);
    }
    if (parser.isSet("grain-model")) {
        const std::string name = parser.value("grain-model").trimmed().toStdString();
        const auto entry = std::ranges::find(grainModelNames, name,
                                             [](const auto& known) { return known.second; });
        if (entry == grainModelNames.end()) {
            std::string names;
            for (const auto& known : grainModelNames) {
                names += (names.empty() ? "" : ", ") + std::string(known.second);
            }
            code = usageError(err, "--grain-model takes " + names);
            return false;
        }
        given.effects.grain.model = entry->first;
        addEdit(edits.settings, "grainModel", given);
    }
    if (parser.isSet("luminance-noise-filter")) {
        const std::string name = parser.value("luminance-noise-filter").trimmed().toStdString();
        const auto entry = std::ranges::find(luminanceNoiseFilterNames, name,
                                             [](const auto& known) { return known.second; });
        if (entry == luminanceNoiseFilterNames.end()) {
            std::string names;
            for (const auto& known : luminanceNoiseFilterNames) {
                names += (names.empty() ? "" : ", ") + std::string(known.second);
            }
            code = usageError(err, "--luminance-noise-filter takes " + names);
            return false;
        }
        given.noiseReduction.luminanceFilter = entry->first;
        addEdit(edits.settings, "luminanceNoiseFilter", given);
    }
    if (parser.isSet("convert-to-grayscale") && parser.isSet("no-convert-to-grayscale")) {
        code = usageError(err, "--convert-to-grayscale and --no-convert-to-grayscale contradict");
        return false;
    }
    if (parser.isSet("convert-to-grayscale") || parser.isSet("no-convert-to-grayscale")) {
        given.blackAndWhite.convertToGrayscale = parser.isSet("convert-to-grayscale");
        addEdit(edits.settings, "convertToGrayscale", given);
    }
    return readGeometry(parser, edits.geometry, err, code);
}

/// @brief Configures the command's own parser.
///
/// In place rather than returned: QCommandLineParser is neither copyable nor
/// movable. Each command has its own, so this help lists export's options and
/// no other command's.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "Render images and write them out.\n"
        "\n"
        "Inputs are files, which your shell can expand from wildcards, or folders. A\n"
        "folder stands for the shots in it, not recursively: a RAW and a JPEG of the\n"
        "same name are one shot, and only the RAW is exported. Files given directly are\n"
        "all exported. --min-rating, --rejected and --label export only the photographs\n"
        "whose sidecar marks match (no sidecar, or --no-sidecar, means no rating and no\n"
        "label); the others are counted, not reported as failures. Every\n"
        "input is attempted, so one bad frame does not abandon an overnight batch. The\n"
        "exit status is 0 when all succeeded, 1 when any failed, 2 for a usage error.\n"
        "\n"
        "With no develop settings an export is a faithful conversion of the image as\n"
        "captured, save for a gentle roll-off that bends the brightest values toward\n"
        "white instead of clipping them flat; --filmic-highlights 0 turns it off.\n"
        "Each file renders through its own .xmp sidecar, if it has one: the develop\n"
        "settings given here are applied on top of the sidecar's and replace only what\n"
        "they name. --no-sidecar ignores sidecars, so the flags alone develop the file.\n"
        "Colour controls come in three families: --saturation and --vibrance, the HSL\n"
        "bands --hue-, --saturation- and --luminance- followed by red, orange, yellow,\n"
        "green, aqua, blue, purple or magenta, and black and white, which\n"
        "--convert-to-grayscale turns on and --gray- plus a band mixes.\n"
        "Tone curves are --tone-curve-luma for luminance and --tone-curve-red, -green and\n"
        "-blue for the channels, each as x,y points joined by semicolons, as in a\n"
        "sidecar: --tone-curve-luma '0,0;0.25,0.2;0.75,0.82;1,1'.\n"
        "Colour grading tints three tonal zones: --grade-shadow-, --grade-midtone- and\n"
        "--grade-highlight- followed by hue (degrees) or saturation (0 to 100, which\n"
        "leaves the zone untouched at 0), with --grade-balance and --grade-blending\n"
        "setting how the zones share the tonal range. It works on black and white too.\n"
        "Hues are Oklab hue angles, not Lightroom's: roughly 30 is red, 110 yellow,\n"
        "140 green and 260 blue. The tint fades out toward white.\n"
        "The post-crop vignette follows the crop: --vignette-amount darkens the edges as\n"
        "an exposure change (-100 is two stops in the corners) or lightens them toward\n"
        "white without clipping, shaped by --vignette-midpoint and --vignette-feather.\n"
        "Film grain is --grain-amount, with --grain-size and --grain-roughness; it is\n"
        "anchored to the cropped frame, so a smaller export shows it softer, never moved.\n"
        "Grain the flags turn on, where the sidecar has none, gets a random pattern per\n"
        "photograph and export, since the command never writes its seed back; grain the\n"
        "sidecar has keeps its pattern, and --grain-seed names one, so that exports repeat.\n"
        "Noise reduction runs first, on the photograph as decoded, so every other control\n"
        "leaves it alone: --luminance-noise-reduction smooths brightness noise, keeping\n"
        "edges according to --luminance-noise-detail and --luminance-noise-filter, and\n"
        "--color-noise-reduction smooths colour blotches of the size --color-noise-smoothness\n"
        "sets, keeping brightness exactly. Both are 0 by default, which skips the pass.\n"
        "Naming --temperature or --tint makes white balance custom; the other half\n"
        "keeps the photograph's own value, or as shot. The command never writes a\n"
        "sidecar.\n"
        "Camera orientation is honoured. Rotation, flips and cropping are applied\n"
        "after colour and tone; crops always stay inside valid image content.\n"
        "Geometry flags set the rotation, straighten and flips as the Develop panel does:\n"
        "an explicit crop in the sidecar is carried with the content it selects, and kept\n"
        "inside valid content. --crop with a rectangle replaces it and leaves the aspect\n"
        "free unless --crop-aspect is given too; --crop-aspect alone fits the largest crop\n"
        "of that ratio inside the sidecar's rectangle. A --crop that disagrees with\n"
        "--crop-aspect is reshaped to the aspect. A sidecar that cannot be read fails\n"
        "its file, unless --no-sidecar is given.\n"
        "\n"
        "Development runs on the GPU when there is one, unless --device cpu is given or\n"
        "ARRAW_DISABLE_GPU is set to a value other than 0. Without --device gpu, a GPU\n"
        "that cannot be used, or is only a software rasteriser, is reported once and the\n"
        "batch runs on the CPU; a photograph the GPU fails on is retried there. With\n"
        "--device gpu nothing falls back, and ARRAW_DISABLE_GPU is a usage error.\n"
        "--gpu-backend opengl with --device auto is --device gpu, since OpenGL needs a\n"
        "display's platform and a process cannot fall back from one that will not load.\n"
        "--device gpuN is --device gpu on the N-th adapter the --gpu-backend lists,\n"
        "counting from 0; `arraw-cli gpu-test` shows the numbers.");
    parser.addHelpOption();
    parser.addOption({{"o", "output"}, "Existing directory to write into.", "dir"});
    parser.addOption({"format", "png, jpeg, or tiff. Default: jpeg.", "name"});
    parser.addOption({"quality", "JPEG quality, 0-100. Default: 90.", "value"});
    parser.addOption({"sharpen", "Output sharpening, 0-100. Default: 0 (off).", "amount"});
    parser.addOption({"bit-depth", "8 or 16. Default: 8.", "value"});
    parser.addOption({"encoding", "srgb, display-p3, or adobe-rgb. Default: srgb.", "name"});
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!isNumericOption(descriptor)) {
            continue;
        }
        const SettingHelp& help = *helpFor(descriptor.key);
        parser.addOption({QString::fromStdString(optionName(descriptor.key)),
                          QString::fromUtf8(help.description) + ", " +
                              QString::fromStdString(limit(descriptor.range->minimum) + " to " +
                                                     limit(descriptor.range->maximum)) +
                              "." + QString::fromUtf8(help.note),
                          help.valueName});
    }
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (takesPoints(descriptor)) {
            const SettingHelp& help = *helpFor(descriptor.key);
            parser.addOption({QString::fromStdString(optionName(descriptor.key)),
                              QString::fromUtf8(help.description) +
                                  ", as x,y points joined by semicolons: 2 to 16 points, x and y "
                                  "from 0 to 1, the ends at x = 0 and x = 1, x at least 0.01 "
                                  "apart, in any order.",
                              help.valueName});
        }
    }
    parser.addOption(
        {"white-balance", "as-shot or custom. Temperature/tint imply custom.", "mode"});
    parser.addOption({"grain-seed",
                      "Which grain pattern, a whole number; 0 is one fixed pattern. Default: "
                      "the sidecar's, or a random one when the flags turn grain on.",
                      "number"});
    parser.addOption(
        {"grain-model", "How grain is drawn: valueNoise. Default: valueNoise.", "name"});
    parser.addOption({"luminance-noise-filter",
                      "How luminance noise is smoothed: bilateral. Default: bilateral.", "name"});
    parser.addOption({"convert-to-grayscale",
                      "Make the photograph black and white; the --gray-* weights mix the hues, and "
                      "saturation, vibrance and the HSL bands then have nothing to act on."});
    parser.addOption({"no-convert-to-grayscale", "Undo a sidecar's black and white conversion."});
    parser.addOption(
        {"rotate", "Any finite clockwise angle, before flips. Default: 0.", "degrees"});
    parser.addOption({"flip-horizontal", "Flip horizontally in the upright frame."});
    parser.addOption({"no-flip-horizontal", "Undo a sidecar's horizontal flip."});
    parser.addOption({"flip-vertical", "Flip vertically in the upright frame."});
    parser.addOption({"no-flip-vertical", "Undo a sidecar's vertical flip."});
    parser.addOption(
        {"crop", "auto or normalised upright left,top,right,bottom. Default: auto.", "rectangle"});
    parser.addOption(
        {"crop-aspect", "free, original, or width:height (3:2, 2:3). Default: free.", "aspect"});
    parser.addOption({"resize",
                      "Shrink to a size, after the crop: N is the long edge, WxH fits inside "
                      "a box, N% scales (12.5% is fine). A bare number is the long edge. "
                      "Never enlarges unless --allow-upscale. Default: full size.",
                      "size"});
    parser.addOption({"allow-upscale", "Let --resize enlarge a photograph past its own size."});
    parser.addOption({"resize-filter",
                      "lanczos or bilinear: how --resize resamples. Lanczos is sharper, with a "
                      "little ringing at hard edges; bilinear is softer and never rings. "
                      "Default: lanczos.",
                      "name"});
    parser.addOption({"device",
                      "auto, cpu, gpu, or gpuN. Auto uses the GPU when it can and says so when "
                      "it cannot; gpu never falls back; gpuN is gpu on the backend's N-th "
                      "adapter, counting from 0. With --gpu-backend opengl, auto is gpu. On "
                      "Windows auto is the CPU for now, until Direct3D has been validated; "
                      "pass gpu to use it. Default: auto.",
                      "name"});
    parser.addOption({"gpu-backend",
                      "vulkan, opengl, d3d11, d3d12, or metal. Default: " +
                          QString::fromUtf8(gpuBackendName(defaultGpuBackend()).data()) +
                          ". Only opengl, on Linux, uses the platform QT_QPA_PLATFORM names "
                          "(xcb or wayland); the others need no display.",
                      "name"});
    parser.addOption({"allow-software", "Accept a software rasteriser, such as llvmpipe or WARP."});
    parser.addOption({"no-sidecar",
                      "Ignore each photograph's .xmp sidecar: develop it from the defaults and "
                      "the flags alone. Without it, the flags are applied on top of the sidecar's "
                      "settings. The command never writes a sidecar."});
    parser.addOption({"no-profile", "Convert colour but do not embed the output profile."});
    parser.addOption({"metadata",
                      "Metadata to carry from the photograph: all, none, or a comma-separated "
                      "list of capture (camera, lens, exposure, time), location (GPS) and "
                      "descriptive (rating, label, title, caption, keywords, creator, "
                      "rights). Default: capture,descriptive.",
                      "list"});
    cli::addMarksFilterOptions(parser);
    parser.addOption(
        {"overwrite",
         "Replace outputs that already exist, but never a file this run reads (a shot's "
         "companions included) or has written."});
    parser.addOption({{"q", "quiet"}, "Do not report each file as it is written."});
    cli::addLogFormatOption(parser);
    // The syntax carries the command word, which Qt's usage line otherwise
    // omits: it knows only argv[0], and the command is a positional we consumed.
    parser.addPositionalArgument("input", "Files, or folders of shots, to export.",
                                 "export <input>...");
}

/// @brief Turns the parsed arguments into an ::ExportRequest.
///
/// Only the values arraw itself must interpret are checked here -- a format
/// name has to become an enumerator, so an unknown one cannot be passed on.
/// Ranges are left to ::arraw::exportImage, which already rejects them and is
/// the single place that knows what it accepts.
/// @return The request, or `std::nullopt` after reporting the problem.
std::optional<ExportRequest> buildRequest(const QCommandLineParser& parser, std::ostream& err,
                                          int& code) {
    ExportRequest request;

    for (const QString& input : parser.positionalArguments()) {
        request.inputs.emplace_back(input.toStdU16String());
    }
    if (request.inputs.empty()) {
        code = usageError(err, "no input files given");
        return std::nullopt;
    }
    if (!parser.isSet("output")) {
        code = usageError(err, "no output directory given; pass -o <dir>");
        return std::nullopt;
    }

    request.outputDirectory = parser.value("output").toStdU16String();
    if (!std::filesystem::is_directory(request.outputDirectory)) {
        code = usageError(err, "not a directory: " + request.outputDirectory.string() +
                                   "\n       arraw-cli writes into an existing directory and "
                                   "does not create one");
        return std::nullopt;
    }

    if (parser.isSet("format")) {
        const auto name = parser.value("format").toLower();
        if (name == "png") {
            request.format = ImageFileFormat::Png;
        } else if (name == "jpeg" || name == "jpg") {
            request.format = ImageFileFormat::Jpeg;
        } else if (name == "tiff" || name == "tif") {
            request.format = ImageFileFormat::Tiff;
        } else {
            code = usageError(err, "unknown format '" + name.toStdString() +
                                       "'; expected png, jpeg, or tiff");
            return std::nullopt;
        }
    }
    request.options.format = request.format;

    if (parser.isSet("encoding")) {
        const auto name = parser.value("encoding").toLower();
        if (name == "srgb") {
            request.options.encoding = NamedEncoding::Srgb;
        } else if (name == "display-p3") {
            request.options.encoding = NamedEncoding::DisplayP3;
        } else if (name == "adobe-rgb") {
            request.options.encoding = NamedEncoding::AdobeRgb;
        } else {
            code = usageError(err, "unknown encoding '" + name.toStdString() +
                                       "'; expected srgb, display-p3, or adobe-rgb");
            return std::nullopt;
        }
    }

    if (!readInteger(parser, "quality", request.options.quality) ||
        !readInteger(parser, "bit-depth", request.options.bitDepth) ||
        !readInteger(parser, "sharpen", request.options.sharpening)) {
        code = usageError(err, "--quality, --bit-depth and --sharpen take whole numbers");
        return std::nullopt;
    }
    if (request.options.sharpening < 0 || request.options.sharpening > 100) {
        code = usageError(err, "--sharpen must be between 0 and 100");
        return std::nullopt;
    }

    if (!readEdits(parser, request.edits, err, code)) {
        return std::nullopt;
    }

    if (parser.isSet("resize")) {
        try {
            request.render.size = cli::parseResize(parser.value("resize").toStdString());
        } catch (const std::invalid_argument& problem) {
            code = usageError(err, problem.what());
            return std::nullopt;
        }
    }
    request.render.upscale = parser.isSet("allow-upscale") ? Upscale::Allowed : Upscale::Never;
    if (parser.isSet("resize-filter")) {
        const auto filter =
            parseResizeFilter(parser.value("resize-filter").toLower().toStdString());
        if (!filter) {
            code = usageError(err, "unknown --resize-filter '" +
                                       parser.value("resize-filter").toStdString() +
                                       "'; expected lanczos or bilinear");
            return std::nullopt;
        }
        request.render.filter = *filter;
    }
    if (!parser.isSet("resize")) {
        for (const char* option : {"allow-upscale", "resize-filter"}) {
            if (parser.isSet(option)) {
                request.ignoredResizeOptions.push_back(std::string("--") + option);
            }
        }
    }

    if (parser.isSet("device")) {
        const auto device = cli::parseDeviceChoice(parser.value("device").toStdString());
        if (!device) {
            code = usageError(err, "unknown device '" + parser.value("device").toStdString() +
                                       "'; expected " + cli::deviceChoices());
            return std::nullopt;
        }
        request.device = *device;
    }
    if (parser.isSet("gpu-backend")) {
        const auto name = parser.value("gpu-backend").toLower().toStdString();
        const auto backend = parseGpuBackend(name);
        if (!backend) {
            code = usageError(err, "unknown backend '" + name +
                                       "'; expected vulkan, opengl, d3d11, d3d12, or metal");
            return std::nullopt;
        }
        request.backend = *backend;
    }
    request.allowSoftware = parser.isSet("allow-software");
    // OpenGL needs a display server's Qt platform, and Qt aborts the process
    // when that cannot load, so auto's promise of a CPU fallback cannot be kept.
    // Asking for OpenGL is taken as asking for the GPU: from here on the request
    // is `gpu`, and every rule that follows is the one `gpu` already has.
    const bool openGlImpliesGpu =
        request.device.kind == cli::DeviceKind::Auto && request.backend == GpuBackend::OpenGL;
    if (openGlImpliesGpu) {
        request.device.kind = cli::DeviceKind::Gpu;
    }
    if (request.device.kind == cli::DeviceKind::Gpu && cli::gpuDisabled()) {
        const std::string asked = openGlImpliesGpu
                                      ? "--gpu-backend opengl, which is --device gpu,"
                                      : "--device " + parser.value("device").toStdString();
        code = usageError(err, asked + " cannot be used while " +
                                   std::string(cli::disableGpuVariable) +
                                   " is set; unset it, or set it to 0");
        return std::nullopt;
    }

    const auto logFormat = cli::readLogFormat(parser, "export", err, code);
    if (!logFormat) {
        return std::nullopt;
    }
    request.logFormat = *logFormat;

    request.options.embedProfile = !parser.isSet("no-profile");
    if (parser.isSet("metadata")) {
        const auto selection = parseMetadataSelection(parser.value("metadata").toStdString());
        if (!selection) {
            code = usageError(err, "--metadata takes all, none, or a comma-separated list of "
                                   "capture, location and descriptive");
            return std::nullopt;
        }
        request.metadata = *selection;
    }
    request.useSidecars = !parser.isSet("no-sidecar");
    const auto filter = cli::readMarksFilter(parser, "export", err, code);
    if (!filter) {
        return std::nullopt;
    }
    request.filter = *filter;
    request.overwrite = parser.isSet("overwrite");
    request.quiet = parser.isSet("quiet");
    return request;
}

/// @brief Whether the request develops on the CPU without ever looking for a GPU.
bool cpuOnly(const ExportRequest& request) {
    return request.device.kind == cli::DeviceKind::Cpu ||
           (request.device.kind == cli::DeviceKind::Auto &&
            (cli::gpuDisabled() || !gpuUsedByDefault()));
}

/// @brief Creates the batch's one GPU context, or says why there is none.
/// @param request What was asked for.
/// @param problem Receives the reason when the result is empty.
/// @return The context, or an empty pointer.
std::unique_ptr<GpuContext> createContext(const ExportRequest& request, std::string& problem) {
    std::unique_ptr<GpuContext> context;
    try {
        context = std::make_unique<GpuContext>(request.backend, request.device.adapter);
    } catch (const std::exception& failure) {
        problem = failure.what();
        return nullptr;
    }
    if (context->info().kind == GpuDeviceKind::Software && !request.allowSoftware) {
        problem = describe(
            {.notice = Notice::GpuSoftwareRefused, .values = {context->info().deviceName}});
        return nullptr;
    }
    return context;
}

/// @brief Develops one decoded photograph on the device and returns it on the host.
///
/// Every device image, the checkpoint included, is gone before this returns,
/// so the context can be destroyed whenever its owner likes.
ImageBuffer developOnDevice(GpuContext& context, const ImageBuffer& source,
                            const DevelopState& state, const RenderRequest& render) {
    const RenderCheckpoint checkpoint =
        developOnGpu(context, source, state, Stage::Effects, render);
    return checkpoint.readBack();
}

/// @brief Exports every input, continuing past the ones that fail.
int exportAll(const ExportRequest& request, std::ostream& err) {
    cli::StreamDiagnostics log(err, request.logFormat, request.quiet);
    std::size_t failures = 0;
    for (const std::string& option : request.ignoredResizeOptions) {
        log.record({.notice = Notice::OptionIgnored,
                    .severity = Severity::Warning,
                    .values = {option, std::string("--resize")}});
    }

    // One device for the batch, created here on the main thread and destroyed
    // on it after the last input: no device image outlives an iteration.
    std::unique_ptr<GpuContext> context;
    if (cpuOnly(request)) {
        if (request.device.kind == cli::DeviceKind::Auto) {
            log.record({.notice = Notice::GpuFallback,
                        .severity = Severity::Warning,
                        .values = {describe({.notice = Notice::GpuDisabled,
                                             .values = {std::string(cli::disableGpuVariable)}})}});
        }
    } else {
        std::string problem;
        context = createContext(request, problem);
        if (!context) {
            if (request.device.kind == cli::DeviceKind::Gpu) {
                log.record({.notice = Notice::GpuFailed,
                            .severity = Severity::Error,
                            .values = {problem}});
                return cli::Failed;
            }
            log.record({.notice = Notice::GpuFallback,
                        .severity = Severity::Warning,
                        .values = {problem}});
        }
    }
    if (context) {
        log.record({.notice = Notice::GpuUsed,
                    .severity = Severity::Info,
                    .values = {std::string(gpuBackendName(context->info().backend)),
                               context->info().deviceName}});
    } else {
        log.record({.notice = Notice::CpuUsed, .severity = Severity::Info});
    }

    cli::ExpandedInputs expanded = cli::expandInputs(request.inputs, log);
    failures += expanded.unreadableFolders;
    // A file named twice (repeated, overlapping globs, or a folder and a file in
    // it) is exported once: the second would only replace the first's export.
    {
        std::set<std::filesystem::path> seen;
        std::erase_if(expanded.photographs, [&seen](const cli::ShotInput& photograph) {
            return !seen.insert(pathKey(photograph.path)).second;
        });
    }
    // Every file the batch reads, a shot's companions included: none may become
    // a destination, --overwrite or not, or a photograph is lost to an export.
    std::map<std::filesystem::path, std::filesystem::path> batchFiles;
    for (const auto& [input, shot] : expanded.photographs) {
        batchFiles.emplace(pathKey(input), input);
        if (shot) {
            for (const auto& companion : shot->companions) {
                batchFiles.emplace(pathKey(companion), companion);
            }
        }
    }
    std::size_t filteredOut = 0;
    std::uint64_t timingRequest = 0;
    // What this batch has written, by destination, and from which input.
    std::map<std::filesystem::path, std::filesystem::path> written;
    for (const auto& [input, shot] : expanded.photographs) {
        const detail::TimingSpan timing("cli.export", ++timingRequest);
        // Built from path parts, not by joining strings: a name that is not
        // ASCII must not pass through the narrow code page on Windows.
        std::filesystem::path name = input.stem();
        name += extensionFor(request.format);
        const auto destination = request.outputDirectory / name;

        try {
            if (!cli::passesFilter(request.filter, input, request.useSidecars)) {
                ++filteredOut;
                continue;
            }
            if (const auto file = sameFileIn(destination, batchFiles)) {
                if (isSameFile(*file, input)) {
                    throw std::runtime_error(destination.string() +
                                             " is the input itself; the export would replace "
                                             "it, so choose another --output folder");
                }
                throw std::runtime_error(destination.string() + " is " + file->string() +
                                         ", which this export reads; choose another "
                                         "--output folder");
            }
            // Nor a file this batch already wrote: inputs of one stem from
            // different folders map to one name, and the later would silently
            // replace the earlier one's export.
            if (const auto earlier = sameFileIn(destination, written)) {
                throw std::runtime_error(destination.string() + " would overwrite the export of " +
                                         earlier->string() +
                                         "; export them separately or to different folders");
            }
            if (!request.overwrite && std::filesystem::exists(destination)) {
                // Refused rather than replaced: the destination is usually a
                // directory of someone's photographs, and exportImage would
                // overwrite without a word.
                throw std::runtime_error(destination.string() +
                                         " already exists; pass --overwrite to replace it");
            }
            // The document first: what the file declares, including a white
            // balance it did not record, is said when the photograph opens
            // rather than when its pixels arrive. The command line's settings
            // are another snapshot of it, and the file on disk is untouched
            // (ADR 012). The decode is then not asked to repeat the warning.
            // The settings are the photograph's own, its sidecar's, with only
            // what the flags named applied on top; --no-sidecar opens it bare.
            // A sidecar that cannot be read fails the file: opened bare, the
            // photograph would export without the edits its photographer made
            // and the exit status would say all was well. --no-sidecar opts out.
            cli::SidecarWatch watch(log);
            const Photo opened = request.useSidecars ? openPhoto(input, watch)
                                                     : Photo(input, readImageMetadata(input, log));
            if (watch.unreadable) {
                throw std::runtime_error("its sidecar could not be read, so its edits are not "
                                         "applied; fix the sidecar, or pass --no-sidecar to "
                                         "export without it");
            }
            DevelopState edited = opened.state();
            // The edits' rules, grain seed included, are core's (Edits.h): grain the
            // flags turn on gets a seed of its own unless they name one, and grain the
            // photograph already has keeps its seed, zero included, so its exports
            // repeat (ADR 038).
            edited.settings = cli::applyEdits(std::move(edited.settings), request.edits, log, input,
                                              opened.metadata());
            const Photo photo = opened.with(std::move(edited));
            // Decoded once, before the device is involved: a file that cannot be
            // read is the input's failure, whichever device would have developed it.
            const ImageBuffer source = loadImage(input);
            // Planned on the host for the same reason: settings the plan rejects
            // (std::invalid_argument) fail the input on either device. What the
            // device is then blamed for is developOnGpu and readBack alone, so
            // any exception from them, an image larger than the device's
            // textures included, means "the GPU could not".
            (void)planFor(source, photo.state(), request.render);
            std::optional<ImageBuffer> developed;
            if (context) {
                try {
                    developed = developOnDevice(*context, source, photo.state(), request.render);
                } catch (const std::exception& failure) {
                    if (request.device.kind == cli::DeviceKind::Gpu) {
                        throw;
                    }
                    const bool lost = context->lost();
                    std::string reason = failure.what();
                    if (lost) {
                        reason += " (the device is lost, so the rest of the batch is exported "
                                  "on the CPU)";
                    }
                    log.record({.notice = Notice::GpuFallback,
                                .severity = Severity::Warning,
                                .subject = input,
                                .values = {reason}});
                    if (lost) {
                        context.reset();
                    }
                }
            }
            if (!developed) {
                developed = develop(source, photo.state(), request.render);
            }
            const ExportMetadata carried{input, photo.marks(), request.metadata,
                                         request.useSidecars};
            try {
                exportImage(*developed, destination, request.options, carried, log);
            } catch (const std::runtime_error& problem) {
                if (std::string_view(problem.what()).find("Cannot write the metadata") ==
                    std::string_view::npos) {
                    throw;
                }
                throw std::runtime_error(std::string(problem.what()) +
                                         "; pass --metadata none to export without it");
            }
            written.emplace(pathKey(destination), input);
            log.record({.notice = Notice::Exported,
                        .severity = Severity::Info,
                        .subject = input,
                        .values = {destination.string()}});
        } catch (const std::exception& problem) {
            log.record({.notice = Notice::InputFailed,
                        .severity = Severity::Error,
                        .subject = input,
                        .values = {std::string(problem.what())}});
            ++failures;
        }
    }

    if (filteredOut > 0) {
        log.record({.notice = Notice::FilteredOut, .values = {static_cast<double>(filteredOut)}});
    }
    if (failures > 0) {
        // Through the log like everything else, so that --log-format json emits
        // nothing a JSON reader has to skip.
        log.record({.notice = Notice::BatchFinished,
                    .severity = Severity::Error,
                    .values = {static_cast<double>(failures),
                               static_cast<double>(expanded.photographs.size() +
                                                   expanded.unreadableFolders)}});
        return cli::Failed;
    }
    return cli::Success;
}

} // namespace

std::optional<cli::ExportEdits> cli::readExportEdits(const std::vector<std::string>& flags,
                                                     std::ostream& err) {
    QCommandLineParser parser;
    configure(parser);
    QStringList arguments{"export"};
    for (const std::string& flag : flags) {
        arguments.append(QString::fromStdString(flag));
    }
    if (!parser.parse(arguments)) {
        (void)usageError(err, parser.errorText().toStdString());
        return std::nullopt;
    }
    ExportEdits edits;
    int code = Success;
    if (!readEdits(parser, edits, err, code)) {
        return std::nullopt;
    }
    return edits;
}

std::variant<RenderRequest::FitInside, RenderRequest::Scale>
cli::parseResize(std::string_view spec) {
    if (spec.ends_with('%')) {
        const std::string_view number = spec.substr(0, spec.size() - 1);
        const bool plain = !number.empty() && number != "." &&
                           std::ranges::all_of(
                               number, [](char c) { return (c >= '0' && c <= '9') || c == '.'; }) &&
                           std::ranges::count(number, '.') <= 1;
        // from_chars, not strtod: the decimal point is '.' whatever the locale.
        double percent = 0.0;
        const bool parsed =
            plain && std::from_chars(number.data(), number.data() + number.size(), percent).ptr ==
                         number.data() + number.size();
        if (!parsed || !std::isfinite(percent)) {
            throw std::invalid_argument("--resize: '" + std::string(spec) +
                                        "' is not a percentage such as 50% or 12.5%");
        }
        if (percent <= 0.0) {
            throw std::invalid_argument("--resize: '" + std::string(spec) +
                                        "' must be greater than 0%");
        }
        return RenderRequest::Scale{percent / 100.0};
    }
    if (const auto at = spec.find_first_of("xX"); at != std::string_view::npos) {
        const std::string_view width = spec.substr(0, at);
        const std::string_view height = spec.substr(at + 1);
        if (width.empty() || height.empty()) {
            throw std::invalid_argument("--resize: '" + std::string(spec) +
                                        "' needs both sides, as in 2048x1365");
        }
        return RenderRequest::FitInside{parseSide(width, spec), parseSide(height, spec)};
    }
    const std::uint32_t edge = parseSide(spec, spec);
    return RenderRequest::FitInside{edge, edge};
}

DevelopSettings cli::applyEdits(DevelopSettings base, const ExportEdits& edits, DiagnosticLog& log,
                                const std::filesystem::path& subject, const ImageMetadata& photo) {
    // Whatever a sidecar left in the settings a render does not read is dropped
    // first: naming one half of temperature and tint then leaves the other as
    // shot, as it always did, rather than adopting a value the photograph was
    // not using. This happens with no flags too, which changes nothing a render reads.
    base = withoutUnusedSettings(std::move(base),
                                 !std::holds_alternative<NamedEncoding>(photo.encoding));
    // The flags are decoded into a source, and the rules of core (white balance
    // mode, grain seed) apply them to the photograph's own settings.
    DevelopSettings source = base;
    std::vector<std::string_view> keys;
    for (const SettingEdit& edit : edits.settings) {
        decode(*edit.descriptor, edit.value, source, log, subject);
        keys.push_back(edit.descriptor->key);
    }
    // The geometry flags are values too; the rules of core (Edits.h) carry the photograph's own
    // crop through them, in the table's order.
    const GeometryEdits& geometry = edits.geometry;
    if (geometry.rotate) {
        setRotationAngle(source.geometry, *geometry.rotate);
        keys.push_back("rotation");
        keys.push_back("straighten");
    }
    if (geometry.flipHorizontal) {
        source.geometry.flipHorizontal = *geometry.flipHorizontal;
        keys.push_back("flipHorizontal");
    }
    if (geometry.flipVertical) {
        source.geometry.flipVertical = *geometry.flipVertical;
        keys.push_back("flipVertical");
    }
    if (geometry.crop) {
        source.geometry.crop.rectangle = geometry.crop->rectangle;
        keys.push_back("cropRectangle");
    }
    if (geometry.aspect) {
        source.geometry.crop.aspect = *geometry.aspect;
        keys.push_back("cropAspect");
    }
    return withValues(photo, DevelopState{std::move(base)}, keys, source).settings;
}

int cli::runExportCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                          const StartApplication& start) {
    QCommandLineParser parser;
    configure(parser);

    // parse() rather than process(): process() writes to the real stderr and
    // calls exit(), neither of which a tested function may do. Qt's help option
    // is therefore checked by hand rather than acted on for us.
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    if (parser.isSet("help")) {
        writeStyledHelp(out, commandHelp(parser));
        return Success;
    }

    int code = Success;
    const auto request = buildRequest(parser, err, code);
    if (!request) {
        return code;
    }
    // Qt Core alone when no GPU will be looked for: the image codecs beyond PNG
    // are plugins, found through an application, and a CPU export touches no
    // graphics device. Otherwise the GUI application a device is created through.
    // Only OpenGL needs a display server's platform; every other backend is
    // reached through the headless one, whatever QT_QPA_PLATFORM says.
    if (cpuOnly(*request)) {
        start(ApplicationKind::Core);
    } else {
        start(request->backend == GpuBackend::OpenGL ? ApplicationKind::Gui
                                                     : ApplicationKind::OffscreenDevice);
    }
    return exportAll(*request, err);
}
