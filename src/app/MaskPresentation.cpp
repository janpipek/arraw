#include "MaskPresentation.h"

#include <LocalAdjustmentEdits.h>

#include <QCoreApplication>

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>

namespace arraw::app {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("arraw::app::MaskPresentation", text);
}

/// @brief Gives the tool tip of a local control with a global counterpart.
QString toolTipFor(std::string_view key) {
    if (key == "exposure") {
        return tr("Brightens or darkens the masked area, added to the photograph's Exposure.");
    }
    if (key == "contrast") {
        return tr("Adds or removes contrast in the masked area, on top of the photograph's.");
    }
    if (key == "highlights") {
        return tr("Recovers or brightens the bright tones in the masked area.");
    }
    if (key == "shadows") {
        return tr("Lifts or deepens the dark tones in the masked area.");
    }
    if (key == "whites") {
        return tr("Moves the white point in the masked area.");
    }
    if (key == "blacks") {
        return tr("Moves the black point in the masked area.");
    }
    if (key == "texture") {
        return tr("Adds or smooths fine detail in the masked area.");
    }
    if (key == "clarity") {
        return tr("Adds or softens local contrast in the masked area.");
    }
    if (key == "dehaze") {
        return tr("Removes or adds haze in the masked area.");
    }
    if (key == "saturation") {
        return tr("Makes the colours in the masked area more or less intense.");
    }
    return tr("Boosts or calms the muted colours in the masked area.");
}

const std::map<std::string, SettingPresentation, std::less<>>& table() {
    // Built on first use, so that translations see the installed translator.
    static const std::map<std::string, SettingPresentation, std::less<>> rows = [] {
        std::map<std::string, SettingPresentation, std::less<>> built;
        for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
            SettingPresentation presentation{{}, {}, 0, 1.0, {}};
            if (descriptor.globalKey.empty()) {
                const bool temperature = descriptor.key == "relativeTemperature";
                presentation.label = temperature ? tr("Temp") : tr("Tint");
                presentation.toolTip =
                    temperature
                        ? tr("Warms (right) or cools (left) the masked area, relative to the "
                             "photograph's white balance.")
                        : tr("Moves the masked area towards magenta (right) or green (left), "
                             "relative to the photograph's white balance.");
            } else {
                presentation = presentationOf(descriptor.globalKey);
                presentation.toolTip = toolTipFor(descriptor.key);
            }
            built.emplace(std::string(descriptor.key), std::move(presentation));
        }
        return built;
    }();
    return rows;
}

} // namespace

const SettingPresentation& localPresentationOf(std::string_view localKey) {
    const auto& rows = table();
    const auto found = rows.find(localKey);
    if (found == rows.end()) {
        throw std::out_of_range("No presentation for the local control " + std::string(localKey));
    }
    return found->second;
}

const SettingPresentation& maskOpacityPresentation() {
    static const SettingPresentation presentation{tr("Opacity"), tr(" %"), 0, 1.0,
                                                  tr("How strongly the mask's adjustments apply.")};
    return presentation;
}

QString maskDisplayName(const DevelopState& state, LocalAdjustmentId id) {
    const LocalAdjustment* adjustment = findLocalAdjustment(state, id);
    if (adjustment == nullptr) {
        throw std::invalid_argument("No local adjustment with that id");
    }
    if (!adjustment->name.empty()) {
        return QString::fromStdString(adjustment->name);
    }
    const auto ordinal = static_cast<int>(maskOrdinal(state, id));
    if (std::holds_alternative<LinearMask>(adjustment->shape)) {
        return tr("Linear %1").arg(ordinal);
    }
    if (std::holds_alternative<RadialMask>(adjustment->shape)) {
        return tr("Radial %1").arg(ordinal);
    }
    if (std::holds_alternative<BrushMask>(adjustment->shape)) {
        return tr("Brush %1").arg(ordinal);
    }
    return tr("Mask %1").arg(ordinal);
}

} // namespace arraw::app
