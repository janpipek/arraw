#include <ImageImport.h>
#include <NegativeSettings.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>

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

void arraw::validate(const NegativeSettings& settings) {
    if (static_cast<std::size_t>(settings.demosaic) >= demosaicCount) {
        throw std::invalid_argument("the demosaic is not one arraw knows");
    }
}

NegativeSettings arraw::defaultNegativeFor(const ImageMetadata&) {
    return {};
}
