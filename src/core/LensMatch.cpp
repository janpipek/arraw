#include <LensMatch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Lowest lensfun score at which a name counts as naming a lens.
///
/// lensfun 0.3 scores a full, correct name at about 80, Canon's name without the maker at about
/// 50, and a bare focal range like "17-50mm" at about 30.
constexpr int minimumScore = 40;

/// @brief How close two lenses' scores may be before they are not told apart.
constexpr int ambiguityMargin = 5;

/// @brief Fraction by which a focal length may differ from a lens's range and still agree.
constexpr double focalTolerance = 0.03;

/// @brief Fraction by which an f-number may differ from a lens's widest and still agree.
constexpr double apertureTolerance = 0.10;

/// @brief Reduces a name to its letters and digits, lower-case, for comparing spellings.
std::string compact(std::string_view name) {
    std::string result;
    for (const char character : name) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    return result;
}

/// @brief Checks whether a name is the whole of a lens's name, as spelled apart from case and
/// spacing.
///
/// Canon writes `EF-S18-135mm f/3.5-5.6 IS USM` for lensfun's `Canon EF-S 18-135mm f/3.5-5.6 IS
/// USM`: the maker left off and no space after the mount, which compares equal here.
bool isWholeName(const LensEntry& lens, std::string_view name) {
    const std::string wanted = compact(name);
    const std::string model = compact(lens.model);
    return wanted == model || compact(lens.maker) + wanted == model ||
           wanted == compact(lens.maker) + model;
}

bool isPrime(const LensEntry& lens) {
    return lens.minFocal > 0.0 && lens.minFocal == lens.maxFocal;
}

/// @brief Checks whether two focal lengths agree, as the same lens states them.
bool sameFocal(double fromFile, double fromDatabase) {
    return std::abs(fromFile - fromDatabase) <= std::max(focalTolerance * fromDatabase, 0.6);
}

/// @brief Checks whether what the photograph says is possible for a lens.
bool agrees(const LensEntry& lens, const LensFacts& facts) {
    const bool knowsFocal = lens.maxFocal > 0.0;
    if (knowsFocal && facts.focalLength &&
        (*facts.focalLength < lens.minFocal * (1.0 - focalTolerance) ||
         *facts.focalLength > lens.maxFocal * (1.0 + focalTolerance))) {
        return false;
    }
    if (knowsFocal && facts.minFocal && !sameFocal(*facts.minFocal, lens.minFocal)) {
        return false;
    }
    if (knowsFocal && facts.maxFocal && !sameFocal(*facts.maxFocal, lens.maxFocal)) {
        return false;
    }
    if (lens.widestAperture > 0.0) {
        // No lens is used wider than it opens, and a prime's maximum is the one it opens to.
        const double widest = lens.widestAperture;
        if (facts.aperture && *facts.aperture < widest * (1.0 - apertureTolerance)) {
            return false;
        }
        if (facts.maxAperture &&
            (*facts.maxAperture < widest * (1.0 - apertureTolerance) ||
             (isPrime(lens) && *facts.maxAperture > widest * (1.0 + apertureTolerance)))) {
            return false;
        }
    }
    return true;
}

LensIdentity identityOf(const LensEntry& lens) {
    return {lens.maker, lens.model};
}

/// @brief Keeps the best-scoring entry of each lens, in order of score.
std::vector<LensEntry> distinct(std::vector<LensEntry> lenses) {
    std::ranges::stable_sort(lenses, {}, [](const LensEntry& lens) { return -lens.score; });
    std::vector<LensEntry> result;
    for (LensEntry& lens : lenses) {
        if (std::ranges::none_of(result, [&](const LensEntry& kept) {
                return identityOf(kept) == identityOf(lens);
            })) {
            result.push_back(std::move(lens));
        }
    }
    return result;
}

std::vector<LensIdentity> identitiesOf(const std::vector<LensEntry>& lenses) {
    std::vector<LensIdentity> result;
    for (const LensEntry& lens : lenses) {
        result.push_back(identityOf(lens));
    }
    return result;
}

} // namespace

LensMatchResult arraw::matchLens(const LensFacts& facts, const LensDatabase& database) {
    if (const auto problem = database.problem()) {
        return LensNoMatch{*problem, {}};
    }
    std::vector<std::string> names;
    for (const std::string& name : facts.lensNames) {
        if (!compact(name).empty()) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        return LensNoMatch{LensProblem::LensUnidentified, {}};
    }

    const auto camera = database.findCamera(facts.cameraMaker, facts.cameraModel);
    LensSearchLimits limits;
    LensMatch match;
    match.cameraListed = camera.has_value();
    if (camera) {
        if (!camera->mount.empty()) {
            limits.mount = camera->mount;
        }
        limits.cropFactor = camera->cropFactor;
        match.cropFactor = camera->cropFactor;
        match.cropSource = CropSource::Camera;
    } else if (facts.focalLength && facts.focalLength35) {
        // Rounded to the hundredths a camera's crop factor is given in.
        const double crop = std::round(*facts.focalLength35 / *facts.focalLength * 100.0) / 100.0;
        limits.cropFactor = crop;
        match.cropFactor = crop;
        match.cropSource = CropSource::FocalRatio;
    }
    // Without a mount to limit it a search can only be trusted when it leaves one lens.
    const bool mountless = !limits.mount.has_value();

    for (const std::string& name : names) {
        std::vector<LensEntry> found = database.findLenses(name, limits);
        std::erase_if(found, [&](const LensEntry& lens) {
            return lens.score < minimumScore || !agrees(lens, facts);
        });
        found = distinct(std::move(found));
        if (found.empty()) {
            continue;
        }
        if (mountless && found.size() > 1) {
            return LensNoMatch{LensProblem::LensAmbiguous, identitiesOf(found)};
        }

        std::vector<LensEntry> exact;
        std::ranges::copy_if(found, std::back_inserter(exact),
                             [&](const LensEntry& lens) { return isWholeName(lens, name); });
        const LensEntry* chosen = nullptr;
        if (exact.size() == 1) {
            chosen = &exact.front();
        } else if (exact.size() > 1) {
            return LensNoMatch{LensProblem::LensAmbiguous, identitiesOf(exact)};
        } else {
            std::vector<LensEntry> close;
            std::ranges::copy_if(found, std::back_inserter(close), [&](const LensEntry& lens) {
                return lens.score > found.front().score - ambiguityMargin;
            });
            if (close.size() > 1) {
                return LensNoMatch{LensProblem::LensAmbiguous, identitiesOf(close)};
            }
            chosen = &found.front();
        }

        match.lens = identityOf(*chosen);
        match.name = name;
        match.score = chosen->score;
        match.exactName = isWholeName(*chosen, name);
        if (facts.focalLength) {
            match.focalLength = *facts.focalLength;
            match.focalLengthRead = true;
        } else if (isPrime(*chosen)) {
            match.focalLength = chosen->minFocal;
        } else {
            return LensNoMatch{LensProblem::LensFocalUnknown, {identityOf(*chosen)}};
        }
        if (match.cropSource != CropSource::Camera && match.cropSource != CropSource::FocalRatio) {
            match.cropFactor = chosen->cropFactor;
        }
        return match;
    }
    return LensNoMatch{LensProblem::LensNotInDatabase, {}};
}
