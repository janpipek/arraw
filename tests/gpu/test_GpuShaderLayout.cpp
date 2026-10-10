#include "Denoise.h"
#include "GpuPlan.h"
#include "LocalPlan.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "TonePlan.h"

#include <ColorSettings.h>
#include <PresenceSettings.h>
#include <SettingDescriptors.h>
#include <ToneSettings.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <rhi/qshader.h>
#include <rhi/qshaderdescription.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The data contract between the C++ uniform blocks in GpuPlan.h and the GLSL
// that reads them, checked from both ends: the layouts the shader compiler
// reflected into the built .qsb files against offsetof and sizeof, and the
// constants the shaders spell out by hand against the C++ they mirror.
//
// Reflection sees declared members only, never a `const`, so the constants are
// read from the shader sources instead. Generating them into a shared include
// from C++ would make them impossible to get wrong rather than caught, but it
// needs a host tool run before qsb in every build, cross builds included, for
// a dozen integers; a test that reads the sources fails just as loudly, names
// the constant, and costs the build nothing.
//
// Neither half needs a device: these cases run, and must pass, on a machine
// that skips every other GPU test.

using namespace arraw;

namespace {

/// @brief Kind of scalar a uniform member holds, or a struct.
enum class Scalar { Float, Uint, Int, Struct, Other };

/// @brief Gives the scalar kind of a C++ member type of a uniform block.
template <typename T> constexpr Scalar scalarOf() {
    if constexpr (std::is_same_v<T, float>) {
        return Scalar::Float;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        return Scalar::Uint;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return Scalar::Int;
    } else if constexpr (std::is_class_v<T> && requires { typename T::value_type; }) {
        return scalarOf<typename T::value_type>();
    } else if constexpr (std::is_class_v<T>) {
        return Scalar::Struct;
    } else {
        return Scalar::Other;
    }
}

/// @brief Gives the scalar kind of a reflected member's GLSL type.
Scalar scalarOf(QShaderDescription::VariableType type) {
    using Type = QShaderDescription::VariableType;
    switch (type) {
    case Type::Float:
    case Type::Vec2:
    case Type::Vec3:
    case Type::Vec4:
        return Scalar::Float;
    case Type::Uint:
    case Type::Uint2:
    case Type::Uint3:
    case Type::Uint4:
        return Scalar::Uint;
    case Type::Int:
    case Type::Int2:
    case Type::Int3:
    case Type::Int4:
        return Scalar::Int;
    case Type::Struct:
        return Scalar::Struct;
    default:
        return Scalar::Other;
    }
}

/// @brief One member of a C++ uniform block, as the GLSL block must lay it out.
struct HostMember {
    std::string name;   ///< Member name, which the GLSL member must share.
    std::size_t offset; ///< offsetof the member.
    std::size_t size;   ///< sizeof the member.
    Scalar scalar;      ///< What its components are.
};

/// Spells a HostMember of a block from its member's name, so that the name the
/// shader is checked against cannot drift from the member it describes.
#define ARRAW_MEMBER(Block, member)                                                                \
    HostMember {                                                                                   \
        #member, offsetof(Block, member), sizeof(Block::member),                                   \
            scalarOf<decltype(Block::member)>()                                                    \
    }

/// @brief Loads a shader the GPU module compiled into its resources.
/// @param name File name, such as `develop.frag`.
QShader loadShader(const char* name) {
    QFile file(QStringLiteral(":/arraw/shaders/%1.qsb").arg(QString::fromLatin1(name)));
    REQUIRE(file.open(QIODevice::ReadOnly));
    QShader shader = QShader::fromSerialized(file.readAll());
    REQUIRE(shader.isValid());
    return shader;
}

/// @brief Gives the uniform block a shader declares at a binding.
QShaderDescription::UniformBlock blockOf(const QShader& shader, int binding) {
    for (const auto& block : shader.description().uniformBlocks()) {
        if (block.binding == binding) {
            return block;
        }
    }
    FAIL("no uniform block at binding " << binding);
    return {};
}

/// @brief Checks reflected block members against a C++ block, member for member.
///
/// Every member the shader declares must be one of @p host, at its offset, of
/// its size and with its kind of scalar; every member of @p host the shader
/// does not declare must be named in @p hostOnly, which is for padding that
/// std140 inserts by itself.
void requireSameLayout(const QList<QShaderDescription::BlockVariable>& reflected,
                       const std::vector<HostMember>& host,
                       const std::set<std::string>& hostOnly = {}) {
    std::map<std::string, const HostMember*> byName;
    for (const HostMember& member : host) {
        byName.emplace(member.name, &member);
    }
    std::set<std::string> declared;
    for (const auto& member : reflected) {
        const std::string name = member.name.toStdString();
        CAPTURE(name);
        const auto found = byName.find(name);
        REQUIRE(found != byName.end());
        REQUIRE(static_cast<std::size_t>(member.offset) == found->second->offset);
        REQUIRE(static_cast<std::size_t>(member.size) == found->second->size);
        REQUIRE(scalarOf(member.type) == found->second->scalar);
        declared.insert(name);
    }
    for (const HostMember& member : host) {
        CAPTURE(member.name);
        REQUIRE((declared.contains(member.name) || hostOnly.contains(member.name)));
    }
}

/// @brief Checks a shader's uniform block against a C++ block, size included.
void requireSameBlock(const char* shader, int binding, std::size_t size,
                      const std::vector<HostMember>& host,
                      const std::set<std::string>& hostOnly = {}) {
    CAPTURE(shader);
    const auto block = blockOf(loadShader(shader), binding);
    REQUIRE(static_cast<std::size_t>(block.size) == size);
    requireSameLayout(block.members, host, hostOnly);
}

const std::vector<HostMember> resizeMembers{
    ARRAW_MEMBER(GpuResizeBlock, plane),
    ARRAW_MEMBER(GpuResizeBlock, inputLength),
    ARRAW_MEMBER(GpuResizeBlock, offset),
};

const std::vector<HostMember> denoiseMembers{
    ARRAW_MEMBER(GpuDenoiseBlock, step),          ARRAW_MEMBER(GpuDenoiseBlock, radius),
    ARRAW_MEMBER(GpuDenoiseBlock, gridReduction), ARRAW_MEMBER(GpuDenoiseBlock, luminance),
    ARRAW_MEMBER(GpuDenoiseBlock, color),         ARRAW_MEMBER(GpuDenoiseBlock, rangeFactor),
    ARRAW_MEMBER(GpuDenoiseBlock, luminanceMix),  ARRAW_MEMBER(GpuDenoiseBlock, colorMix),
    ARRAW_MEMBER(GpuDenoiseBlock, lumaRow),       ARRAW_MEMBER(GpuDenoiseBlock, neutral),
    ARRAW_MEMBER(GpuDenoiseBlock, sourceSize),    ARRAW_MEMBER(GpuDenoiseBlock, gridSize),
    ARRAW_MEMBER(GpuDenoiseBlock, weights),
};

/// @brief Gives the source folder of the shaders, which the build bakes in.
std::filesystem::path shaderSources() {
    return ARRAW_SHADER_SOURCE_DIR;
}

/// @brief Reads a shader source with its `#include`s expanded in place.
std::string expandedSource(const std::filesystem::path& path) {
    std::ifstream file(path);
    REQUIRE(file.good());
    static const std::regex include(R"(^\s*#include\s+"([^"]+)\"\s*$)");
    std::ostringstream text;
    std::string line;
    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_match(line, match, include)) {
            text << expandedSource(path.parent_path() / match[1].str());
        } else {
            text << line << '\n';
        }
    }
    return text.str();
}

/// @brief Gives the enumerators a C++ enum in `GpuPlan.h` declares, by name.
///
/// Read from the header's text, as C++ cannot list an enum's members: a
/// step added in C++ and forgotten in the check below is then caught too.
std::set<std::string> enumeratorsOf(std::string_view name) {
    std::ifstream file(shaderSources().parent_path() / "GpuPlan.h");
    REQUIRE(file.good());
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    const std::string opening = "enum class " + std::string(name) + " ";
    const auto begin = text.find(opening);
    REQUIRE(begin != std::string::npos);
    const auto bodyBegin = text.find('{', begin);
    const auto bodyEnd = text.find("};", bodyBegin);
    REQUIRE(bodyEnd != std::string::npos);
    static const std::regex enumerator(R"(^\s*(\w+)\s*=)");
    std::set<std::string> names;
    std::istringstream body(text.substr(bodyBegin + 1, bodyEnd - bodyBegin - 1));
    std::string line;
    while (std::getline(body, line)) {
        std::smatch match;
        if (std::regex_search(line, match, enumerator)) {
            names.insert(match[1].str());
        }
    }
    return names;
}

/// @brief Gives the `const uint` and `const float` literals of a shader, by name.
std::map<std::string, std::string> constantsOf(const char* shader) {
    static const std::regex constant(
        R"(^\s*const\s+(?:uint|float)\s+(\w+)\s*=\s*([0-9.eE+-]+u?)\s*;)");
    std::map<std::string, std::string> constants;
    std::istringstream text(expandedSource(shaderSources() / shader));
    std::string line;
    while (std::getline(text, line)) {
        std::smatch match;
        if (std::regex_search(line, match, constant)) {
            constants.emplace(match[1].str(), match[2].str());
        }
    }
    return constants;
}

/// @brief Checks a shader's constants with a prefix against the C++ values they mirror.
///
/// Both ways: each named constant must hold its value, and the shader may
/// declare no other constant with the prefix, so that a step added on one side
/// only is caught.
/// @param enumName The C++ enum's name in `GpuPlan.h`, whose every enumerator must be in
/// @p expected (as the prefix and its name) or in @p exempt.
/// @param exempt Enumerators with no constant in the shader, each for a reason the caller gives.
template <typename Enum>
void requireSameEnum(const char* shader, std::string_view prefix,
                     const std::map<std::string, Enum>& expected, std::string_view enumName,
                     const std::set<std::string>& exempt = {}) {
    CAPTURE(shader);
    for (const std::string& enumerator : enumeratorsOf(enumName)) {
        CAPTURE(enumName, enumerator);
        REQUIRE(
            (exempt.contains(enumerator) || expected.contains(std::string(prefix) + enumerator)));
    }
    std::map<std::string, std::uint32_t> found;
    for (const auto& [name, literal] : constantsOf(shader)) {
        if (name.starts_with(prefix) && literal.ends_with('u')) {
            found.emplace(name, static_cast<std::uint32_t>(std::stoul(literal)));
        }
    }
    for (const auto& [name, value] : expected) {
        CAPTURE(name);
        REQUIRE(found.contains(name));
        REQUIRE(found.at(name) == static_cast<std::uint32_t>(value));
    }
    for (const auto& [name, value] : found) {
        CAPTURE(name, value);
        REQUIRE(expected.contains(name));
    }
}

/// @brief Checks that a shader's float constant reads as exactly a C++ float.
void requireSameFloat(const char* shader, const std::string& name, float expected) {
    CAPTURE(shader, name);
    const auto constants = constantsOf(shader);
    const auto found = constants.find(name);
    REQUIRE(found != constants.end());
    // from_chars, not strtof: GLSL spells the decimal point '.' whatever the locale.
    const std::string& text = found->second;
    float value = 0.0F;
    REQUIRE(std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{});
    REQUIRE(value == expected);
}

} // namespace

TEST_CASE("The pointwise shader's block is GpuPointwiseBlock, member for member", "[gpu][shader]") {
    using Block = GpuPointwiseBlock;
    requireSameBlock("develop.frag", 1, sizeof(Block),
                     {
                         ARRAW_MEMBER(Block, toWorking),
                         ARRAW_MEMBER(Block, exposureGain),
                         ARRAW_MEMBER(Block, contrastSlope),
                         ARRAW_MEMBER(Block, contrastScale),
                         ARRAW_MEMBER(Block, shadowShift),
                         ARRAW_MEMBER(Block, highlightShift),
                         ARRAW_MEMBER(Block, blackShift),
                         ARRAW_MEMBER(Block, whiteShift),
                         ARRAW_MEMBER(Block, shoulderKnee),
                         ARRAW_MEMBER(Block, shapesTone),
                         ARRAW_MEMBER(Block, rollsHighlights),
                         ARRAW_MEMBER(Block, probe),
                         ARRAW_MEMBER(Block, convertsToGrayscale),
                         ARRAW_MEMBER(Block, saturation),
                         ARRAW_MEMBER(Block, vibrance),
                         ARRAW_MEMBER(Block, adjustsSaturation),
                         ARRAW_MEMBER(Block, adjustsVibrance),
                         ARRAW_MEMBER(Block, adjustsHsl),
                         ARRAW_MEMBER(Block, curvesLuma),
                         ARRAW_MEMBER(Block, curvesRed),
                         ARRAW_MEMBER(Block, curvesGreen),
                         ARRAW_MEMBER(Block, curvesBlue),
                         ARRAW_MEMBER(Block, padding),
                         ARRAW_MEMBER(Block, hueShift),
                         ARRAW_MEMBER(Block, bandSaturation),
                         ARRAW_MEMBER(Block, bandLuminance),
                         ARRAW_MEMBER(Block, grayMix),
                         ARRAW_MEMBER(Block, grades),
                         ARRAW_MEMBER(Block, gradeBalanceShift),
                         ARRAW_MEMBER(Block, gradeZoneWidth),
                         ARRAW_MEMBER(Block, gradePadding),
                         ARRAW_MEMBER(Block, gradeShadowMidtoneTint),
                         ARRAW_MEMBER(Block, gradeHighlightTint),
                         ARRAW_MEMBER(Block, presenceLumaRow),
                         ARRAW_MEMBER(Block, presence),
                         ARRAW_MEMBER(Block, textureAmount),
                         ARRAW_MEMBER(Block, clarityAmount),
                         ARRAW_MEMBER(Block, dehazeAmount),
                         ARRAW_MEMBER(Block, fineReduction),
                         ARRAW_MEMBER(Block, coarseReduction),
                         ARRAW_MEMBER(Block, fineGridSize),
                         ARRAW_MEMBER(Block, coarseGridSize),
                         ARRAW_MEMBER(Block, presencePadding),
                         ARRAW_MEMBER(Block, localHeader),
                         ARRAW_MEMBER(Block, localGlobal),
                         ARRAW_MEMBER(Block, local),
                     },
                     // The three words before the band sets, which std140 skips by itself.
                     {"padding"});

    // The masks: sixteen structs, each member where GpuLocalMask puts it.
    const auto block = blockOf(loadShader("develop.frag"), 1);
    const auto masks = std::find_if(block.members.begin(), block.members.end(),
                                    [](const auto& member) { return member.name == "local"; });
    REQUIRE(masks != block.members.end());
    REQUIRE(masks->arrayDims == QList<int>{static_cast<int>(gpuLocalMaskCapacity)});
    using Mask = GpuLocalMask;
    requireSameLayout(masks->structMembers, {
                                                ARRAW_MEMBER(Mask, header),
                                                ARRAW_MEMBER(Mask, shapeA),
                                                ARRAW_MEMBER(Mask, shapeB),
                                                ARRAW_MEMBER(Mask, k),
                                            });
    REQUIRE(static_cast<std::size_t>(masks->size) == sizeof(Block::local));
    REQUIRE(sizeof(Mask) == 112);
    REQUIRE(sizeof(Block) == 2256);
}

TEST_CASE("The geometry shader's block is GpuGeometryBlock, member for member", "[gpu][shader]") {
    using Block = GpuGeometryBlock;
    requireSameBlock("geometry.frag", 1, sizeof(Block),
                     {
                         ARRAW_MEMBER(Block, originWhole),
                         ARRAW_MEMBER(Block, originFraction),
                         ARRAW_MEMBER(Block, columnStepHigh),
                         ARRAW_MEMBER(Block, rowStepHigh),
                         ARRAW_MEMBER(Block, columnStepLow),
                         ARRAW_MEMBER(Block, rowStepLow),
                         ARRAW_MEMBER(Block, sourceSize),
                         ARRAW_MEMBER(Block, outputSize),
                     });
}

TEST_CASE("Every resize shader's block is GpuResizeBlock, member for member", "[gpu][shader]") {
    for (const char* shader : {"resize_across.frag", "resize_across_opaque.frag",
                               "resize_down.frag", "resize_down_opaque.frag"}) {
        requireSameBlock(shader, 1, sizeof(GpuResizeBlock), resizeMembers);
    }
}

TEST_CASE("The effects shader's block is GpuEffectsBlock, grain layers included", "[gpu][shader]") {
    using Block = GpuEffectsBlock;
    requireSameBlock("effects.frag", 1, sizeof(Block),
                     {
                         ARRAW_MEMBER(Block, origin),
                         ARRAW_MEMBER(Block, step),
                         ARRAW_MEMBER(Block, vignettes),
                         ARRAW_MEMBER(Block, vignetteLightens),
                         ARRAW_MEMBER(Block, vignetteHardEdge),
                         ARRAW_MEMBER(Block, vignetteStops),
                         ARRAW_MEMBER(Block, vignetteInner),
                         ARRAW_MEMBER(Block, vignetteOuter),
                         ARRAW_MEMBER(Block, grains),
                         ARRAW_MEMBER(Block, grainModel),
                         ARRAW_MEMBER(Block, grainLayers),
                     });

    const auto block = blockOf(loadShader("effects.frag"), 1);
    const auto layers =
        std::find_if(block.members.begin(), block.members.end(),
                     [](const auto& member) { return member.name == "grainLayers"; });
    REQUIRE(layers != block.members.end());
    REQUIRE(layers->arrayDims == QList<int>{static_cast<int>(grainLayerCount)});
    using Layer = GpuGrainLayer;
    requireSameLayout(layers->structMembers, {
                                                 ARRAW_MEMBER(Layer, cell),
                                                 ARRAW_MEMBER(Layer, fraction),
                                                 ARRAW_MEMBER(Layer, delta),
                                                 ARRAW_MEMBER(Layer, weight),
                                                 ARRAW_MEMBER(Layer, seed),
                                             });
    REQUIRE(static_cast<std::size_t>(layers->size) == sizeof(Block::grainLayers));
}

TEST_CASE("Both Denoise shaders' blocks are GpuDenoiseBlock, member for member", "[gpu][shader]") {
    for (const char* shader : {"denoise_filter.frag", "denoise_combine.frag"}) {
        requireSameBlock(shader, 1, sizeof(GpuDenoiseBlock), denoiseMembers);
    }
}

TEST_CASE("The Presence shader's block is GpuPresenceBlock, member for member", "[gpu][shader]") {
    using Block = GpuPresenceBlock;
    requireSameBlock("presence_filter.frag", 1, sizeof(Block),
                     {
                         ARRAW_MEMBER(Block, step),
                         ARRAW_MEMBER(Block, radius),
                         ARRAW_MEMBER(Block, reduction),
                         ARRAW_MEMBER(Block, window),
                         ARRAW_MEMBER(Block, lumaRow),
                         ARRAW_MEMBER(Block, sourceSize),
                         ARRAW_MEMBER(Block, gridSize),
                         ARRAW_MEMBER(Block, weights),
                     });
}

TEST_CASE("The shaders' step and probe numbers are the C++ enumerators", "[gpu][shader]") {
    // Combine is a pass of its own (denoise_combine.frag), not a step the filter branches on.
    requireSameEnum<DenoiseStep>("denoise_filter.frag", "step",
                                 {
                                     {"stepReduce", DenoiseStep::Reduce},
                                     {"stepBlurAcross", DenoiseStep::BlurAcross},
                                     {"stepBlurDown", DenoiseStep::BlurDown},
                                     {"stepBilateralAcross", DenoiseStep::BilateralAcross},
                                     {"stepBilateralDown", DenoiseStep::BilateralDown},
                                 },
                                 "DenoiseStep", {"Combine"});
    requireSameEnum<PresenceStep>(
        "presence_filter.frag", "step",
        {
            {"stepReduce", PresenceStep::Reduce},
            {"stepBlurAcross", PresenceStep::BlurAcross},
            {"stepBlurDown", PresenceStep::BlurDown},
            {"stepMinimumAcross", PresenceStep::MinimumAcross},
            {"stepMinimumDown", PresenceStep::MinimumDown},
            {"stepMaximumAcross", PresenceStep::MaximumAcross},
            {"stepMaximumDown", PresenceStep::MaximumDown},
            {"stepBlurDownAboveOpening", PresenceStep::BlurDownAboveOpening},
            {"stepReconstruct", PresenceStep::Reconstruct},
            {"stepMinimumDiagonal", PresenceStep::MinimumDiagonal},
            {"stepMinimumAntidiagonal", PresenceStep::MinimumAntidiagonal},
            {"stepMaximumDiagonal", PresenceStep::MaximumDiagonal},
            {"stepMaximumAntidiagonal", PresenceStep::MaximumAntidiagonal},
        },
        "PresenceStep");
    // Developed is the default the shader falls through to, so it has no constant.
    requireSameEnum<PointwiseProbe>("develop.frag", "probe",
                                    {
                                        {"probeAfterMatrix", PointwiseProbe::AfterMatrix},
                                        {"probeAfterExposure", PointwiseProbe::AfterExposure},
                                        {"probeAfterTone", PointwiseProbe::AfterTone},
                                        {"probeAfterShoulder", PointwiseProbe::AfterShoulder},
                                        {"probeAfterCurves", PointwiseProbe::AfterCurves},
                                    },
                                    "PointwiseProbe", {"Developed"});
}

TEST_CASE("The pointwise shader's local constants are the C++ ones", "[gpu][shader]") {
    requireSameEnum<GpuLocalFlag>("develop.frag", "localFlag",
                                  {
                                      {"localFlagFineBase", GpuLocalFlag::FineBase},
                                      {"localFlagCoarseBase", GpuLocalFlag::CoarseBase},
                                      {"localFlagHazeFloor", GpuLocalFlag::HazeFloor},
                                      {"localFlagHazeMean", GpuLocalFlag::HazeMean},
                                  },
                                  "GpuLocalFlag");
    // The kinds are in LocalPlan.h, which the enumerator scan above does not read.
    const auto kinds = constantsOf("develop.frag");
    REQUIRE(std::stoul(kinds.at("maskKindLinear")) ==
            static_cast<unsigned long>(LocalMaskKind::Linear));
    REQUIRE(std::stoul(kinds.at("maskKindRadial")) ==
            static_cast<unsigned long>(LocalMaskKind::Radial));

    // The rows of the local table, by key, so that a reordered table is caught here.
    const std::map<std::string, std::string> rows{
        {"controlTemperature", "relativeTemperature"},
        {"controlTint", "relativeTint"},
        {"controlExposure", "exposure"},
        {"controlContrast", "contrast"},
        {"controlHighlights", "highlights"},
        {"controlShadows", "shadows"},
        {"controlWhites", "whites"},
        {"controlBlacks", "blacks"},
        {"controlTexture", "texture"},
        {"controlClarity", "clarity"},
        {"controlDehaze", "dehaze"},
        {"controlSaturation", "saturation"},
        {"controlVibrance", "vibrance"},
    };
    const auto constants = constantsOf("develop.frag");
    for (const auto& [name, key] : rows) {
        CAPTURE(name, key);
        const auto found = constants.find(name);
        REQUIRE(found != constants.end());
        const auto row = std::find_if(
            localAdjustmentDescriptors.begin(), localAdjustmentDescriptors.end(),
            [&key](const LocalDescriptor& descriptor) { return descriptor.key == key; });
        REQUIRE(row != localAdjustmentDescriptors.end());
        REQUIRE(std::stoul(found->second) ==
                static_cast<unsigned long>(row - localAdjustmentDescriptors.begin()));
    }
    REQUIRE(std::stoul(constants.at("controlCount")) == localAdjustmentDescriptors.size());

    requireSameFloat("develop.frag", "darkestExposure", darkestExposure);
    requireSameFloat("develop.frag", "brightestExposure", brightestExposure);
    requireSameFloat("develop.frag", "flattestContrast", flattestContrast);
    requireSameFloat("develop.frag", "steepestContrast", steepestContrast);
    requireSameFloat("develop.frag", "weakestToneControl", weakestToneControl);
    requireSameFloat("develop.frag", "strongestToneControl", strongestToneControl);
    requireSameFloat("develop.frag", "weakestPresence", weakestPresence);
    requireSameFloat("develop.frag", "strongestPresence", strongestPresence);
    requireSameFloat("develop.frag", "weakestSaturation", weakestSaturation);
    requireSameFloat("develop.frag", "strongestSaturation", strongestSaturation);
    requireSameFloat("develop.frag", "localControlLimit", static_cast<float>(localControlLimit));
    requireSameFloat("develop.frag", "regionalReach", regionalReach);
    requireSameFloat("develop.frag", "endpointReach", endpointReach);
    requireSameFloat("develop.frag", "temperatureRedStops", temperatureRedStops);
    requireSameFloat("develop.frag", "temperatureBlueStops", temperatureBlueStops);
    requireSameFloat("develop.frag", "tintRedStops", tintRedStops);
    requireSameFloat("develop.frag", "tintBlueStops", tintBlueStops);
    requireSameFloat("develop.frag", "greyPivot", greyPivot);
}

TEST_CASE("The shaders' shared float constants are the C++ ones", "[gpu][shader]") {
    for (const char* shader : {"develop.frag", "effects.frag"}) {
        requireSameFloat(shader, "perceptualExponent", 1.0F / 2.2F);
        requireSameFloat(shader, "linearExponent", 2.2F);
    }
    requireSameFloat("denoise_filter.frag", "perceptualExponent", 1.0F / 2.2F);
    for (const char* shader : {"denoise_filter.frag", "denoise_combine.frag"}) {
        requireSameFloat(shader, "ratioFloor", denoiseRatioFloor);
    }
    for (const char* shader : {"develop.frag", "presence_filter.frag"}) {
        requireSameFloat(shader, "presenceLuminanceFloor", presenceLuminanceFloor);
        requireSameFloat(shader, "presenceLuminanceCeiling", presenceLuminanceCeiling);
    }
    requireSameFloat("develop.frag", "curveRatioFloor", curveRatioFloor);
}

TEST_CASE("The pointwise shader reads the brush coverage at bindings 8 to 11",
          "[gpu][shader][brush]") {
    const QShader shader = loadShader("develop.frag");
    std::map<std::string, int> bindings;
    for (const auto& sampler : shader.description().combinedImageSamplers()) {
        bindings[sampler.name.toStdString()] = sampler.binding;
    }
    // The inputs in the order GpuContext binds them: the image at 0, the rest from 2 on.
    const std::vector<std::pair<std::string, int>> expected{
        {"source", 0},      {"curves", 2},     {"fineBase", 3},  {"coarseBase", 4},
        {"coarseCells", 5}, {"hazeFloor", 6},  {"hazeMean", 7},  {"coverage0", 8},
        {"coverage1", 9},   {"coverage2", 10}, {"coverage3", 11}};
    REQUIRE(bindings.size() == expected.size());
    for (const auto& [name, binding] : expected) {
        CAPTURE(name);
        REQUIRE(bindings.count(name) == 1);
        REQUIRE(bindings.at(name) == binding);
    }
    // The kind a brush mask has, and the words GpuPlan.h packs the texture and channel in.
    REQUIRE(std::stoul(constantsOf("develop.frag").at("maskKindBrush")) ==
            static_cast<unsigned long>(LocalMaskKind::Brush));
}
