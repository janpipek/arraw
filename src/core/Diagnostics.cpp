#include "Diagnostics.h"

#include <format>
#include <utility>

using namespace arraw;

namespace {

/// @brief Writes one detail, whichever kind it is.
std::string text(const DiagnosticValue& value) {
    if (const auto* words = std::get_if<std::string>(&value)) {
        return *words;
    }
    return std::format("{}", std::get<double>(value));
}

/// @brief Reads one detail, or an empty one when it was not supplied.
std::string valueAt(const Diagnostic& diagnostic, std::size_t index) {
    return index < diagnostic.values.size() ? text(diagnostic.values[index]) : std::string{};
}

/// @brief A log with nowhere to put anything.
class DiscardingDiagnostics final : public DiagnosticLog {
public:
    void record(const Diagnostic&) override {}
};

} // namespace

std::string arraw::describe(const Diagnostic& diagnostic) {
    // Exhaustive and without a default, so that a notice added without
    // anything to say about it fails the build rather than the reader.
    switch (diagnostic.notice) {
    case Notice::SubstitutedWhiteBalance:
        return "this file records no white balance, so a daylight one was used; "
               "the temperature shown is an estimate, not what the camera saw";
    case Notice::Exported:
        return "written to " + valueAt(diagnostic, 0);
    case Notice::InputFailed:
        return valueAt(diagnostic, 0);
    case Notice::BatchFinished:
        return valueAt(diagnostic, 0) + " of " + valueAt(diagnostic, 1) + " failed";
    case Notice::GpuSoftwareRefused:
        return "'" + valueAt(diagnostic, 0) +
               "' is a software rasteriser, not a GPU; pass --allow-software to accept it";
    case Notice::GpuSoftwareAccepted:
        return "'" + valueAt(diagnostic, 0) +
               "' is a software rasteriser, accepted because --allow-software was given";
    case Notice::GpuAdapterSkipped:
        return valueAt(diagnostic, 0) + " ('" + valueAt(diagnostic, 1) +
               "') is a software rasteriser, so it was not tested; pass --allow-software to "
               "test it";
    case Notice::GpuNoFloatTextures:
        return "the device does not support RGBA32F textures, which development needs";
    case Notice::GpuReadBackNotPromised:
        return "the " + valueAt(diagnostic, 0) +
               " backend does not promise float readback; the round trip decides";
    case Notice::GpuFailed:
        return valueAt(diagnostic, 0);
    case Notice::GpuRoundTripRedescribed:
        return "the image read back is not described as the one uploaded";
    case Notice::GpuRoundTripChanged:
        return "the round trip changed " + valueAt(diagnostic, 0) +
               " samples; the first, channel " + valueAt(diagnostic, 1) + " of pixel (" +
               valueAt(diagnostic, 2) + ", " + valueAt(diagnostic, 3) + "), went in as " +
               valueAt(diagnostic, 4) + " and came back as " + valueAt(diagnostic, 5);
    case Notice::GpuDisabled:
        return "the GPU is disabled by " + valueAt(diagnostic, 0) +
               "; unset it, or set it to 0, to probe the device";
    case Notice::GpuUsed:
        return "exporting on the GPU: " + valueAt(diagnostic, 1) + " (" + valueAt(diagnostic, 0) +
               ")";
    case Notice::CpuUsed:
        return "exporting on the CPU";
    case Notice::GpuFallback:
        return "not using the GPU, exporting on the CPU instead: " + valueAt(diagnostic, 0);
    case Notice::SettingClamped:
        return "'" + valueAt(diagnostic, 0) + "' is " + valueAt(diagnostic, 1) +
               ", outside what it accepts, so " + valueAt(diagnostic, 2) + " was used";
    case Notice::SettingUnknown:
        return "'" + valueAt(diagnostic, 0) + "' is not a setting, so it was ignored";
    case Notice::SettingMalformed:
        return "'" + valueAt(diagnostic, 0) + "' cannot be read, so it was ignored; expected " +
               valueAt(diagnostic, 1);
    case Notice::NewerSettingsVersion:
        return "these settings are version " + valueAt(diagnostic, 0) +
               ", but this arraw knows up to version " + valueAt(diagnostic, 1) +
               "; reading what it can";
    case Notice::SidecarUnreadable:
        return valueAt(diagnostic, 0) + "; opened with default settings and no marks";
    case Notice::ExifUnreadable:
        return valueAt(diagnostic, 0) + "; no capture information is shown";
    case Notice::OptionIgnored:
        return valueAt(diagnostic, 0) + " does nothing without " + valueAt(diagnostic, 1);
    case Notice::CropReset:
        return "the crop was drawn before " + valueAt(diagnostic, 0) +
               " changed, so automatic framing was used instead";
    }
    return {};
}

DiagnosticLog& arraw::discardedDiagnostics() {
    static DiscardingDiagnostics log;
    return log;
}

void CollectedDiagnostics::record(const Diagnostic& diagnostic) {
    entries_.push_back(diagnostic);
}
