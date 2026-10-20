#include <ImageImport.h>
#include <NegativeSettings.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

using namespace arraw;

std::string_view arraw::tokenOf(Demosaic demosaic) noexcept {
    const auto index = static_cast<std::size_t>(demosaic);
    return index < demosaicTokens.size() ? demosaicTokens[index] : std::string_view{};
}

std::optional<Demosaic> arraw::parseDemosaic(std::string_view token) noexcept {
    const auto found = std::ranges::find(demosaicTokens, token);
    if (found == demosaicTokens.end()) {
        return std::nullopt;
    }
    return static_cast<Demosaic>(found - demosaicTokens.begin());
}

std::string_view arraw::tokenOf(LensOrigin origin) noexcept {
    const auto index = static_cast<std::size_t>(origin);
    return index < lensOriginTokens.size() ? lensOriginTokens[index] : std::string_view{};
}

std::optional<LensOrigin> arraw::parseLensOrigin(std::string_view token) noexcept {
    const auto found = std::ranges::find(lensOriginTokens, token);
    if (found == lensOriginTokens.end()) {
        return std::nullopt;
    }
    return static_cast<LensOrigin>(found - lensOriginTokens.begin());
}

void arraw::validate(const NegativeSettings& settings) {
    if (static_cast<std::size_t>(settings.demosaic) >= demosaicCount) {
        throw std::invalid_argument("the demosaic is not one arraw knows");
    }
    const float amount = settings.lens.vignettingAmount;
    if (!std::isfinite(amount) || amount < minimumVignettingAmount ||
        amount > maximumVignettingAmount) {
        throw std::invalid_argument("the vignetting amount must be from " +
                                    std::to_string(static_cast<int>(minimumVignettingAmount)) +
                                    " to " +
                                    std::to_string(static_cast<int>(maximumVignettingAmount)));
    }
    if (const auto& lens = settings.lens.lens) {
        if (lens->model.empty()) {
            throw std::invalid_argument("a recorded lens needs a model");
        }
        if (!std::isfinite(lens->cropFactor) || lens->cropFactor <= 0.0) {
            throw std::invalid_argument("the crop factor of a recorded lens must be positive");
        }
        if (static_cast<std::size_t>(lens->origin) >= lensOriginCount) {
            throw std::invalid_argument("the origin of a recorded lens is not one arraw knows");
        }
    }
}

NegativeSettings arraw::defaultNegativeFor(const ImageMetadata&) {
    return {};
}
