#pragma once

#include <LensDatabase.h>
#include <LensFacts.h>
#include <LensProblem.h>

#include <string>
#include <variant>
#include <vector>

namespace arraw {

/// @brief A lens as lensfun names it: its maker and its model, exactly.
///
/// What a recorded match is looked up by (ADR 050, section 6). The database may list a lens
/// under one name more than once, calibrated on different sensors; the image's crop factor then
/// tells which calibration is used.
struct LensIdentity {
    std::string maker; ///< Lens maker, as lensfun spells it.
    std::string model; ///< Lens model, as lensfun spells it.

    friend bool operator==(const LensIdentity&, const LensIdentity&) = default;
};

/// @brief Where the crop factor of a match came from.
enum class CropSource {
    Camera,     ///< The camera listed in the database.
    FocalRatio, ///< The 35 mm focal length over the focal length, for a camera that is not listed.
    Lens, ///< The lens's own, when nothing else says; the match is then a guess at the sensor.
};

/// @brief The lens a photograph was taken with, found in the database.
struct LensMatch {
    LensIdentity lens;         ///< The lens found.
    std::string name;          ///< The name from the photograph that found it.
    int score = 0;             ///< lensfun's score for that name.
    bool exactName = false;    ///< Whether the name is the lens's whole name, not merely like it.
    bool cameraListed = false; ///< Whether the camera is in the database; informational when not.
    double cropFactor = 1.0;   ///< Crop factor of the image, as the profile is applied with.
    CropSource cropSource = CropSource::Lens; ///< Where ::arraw::LensMatch::cropFactor came from.
    double focalLength = 0.0;     ///< Focal length to resolve the profile at, in millimetres.
    bool focalLengthRead = false; ///< Whether the file recorded it; a prime's own is assumed.

    friend bool operator==(const LensMatch&, const LensMatch&) = default;
};

/// @brief Why no lens was found, and which lenses came into question.
struct LensNoMatch {
    LensProblem problem = LensProblem::LensNotInDatabase; ///< The reason.

    /// @brief The lenses that fitted equally well, for ::arraw::LensProblem::LensAmbiguous; the
    /// one found, for ::arraw::LensProblem::LensFocalUnknown; otherwise empty.
    std::vector<LensIdentity> candidates;

    friend bool operator==(const LensNoMatch&, const LensNoMatch&) = default;
};

/// @brief What matching a photograph's lens gave: a lens, or the reason there is none.
using LensMatchResult = std::variant<LensMatch, LensNoMatch>;

/// @brief Finds the lens a photograph was taken with in a lens database (ADR 050, section 5).
///
/// The camera is found first; its mount and crop factor then limit the lenses. Each name the
/// photograph gives the lens is tried in order, by lensfun's own word matching, and the first
/// name that leaves lenses decides. A lens is kept only if its score passes a threshold, its
/// focal range and widest aperture agree with what the file says, and it fits the mount. Of
/// those, a lens whose name is the photograph's whole name wins; otherwise two different lenses
/// within a small margin of each other give ::arraw::LensProblem::LensAmbiguous and nothing is
/// applied, since a wrong lens is worse than none. Without a listed camera no mount limits the
/// search, and a match is accepted only when it is the one lens left.
///
/// The crop factor is the camera's, else the 35 mm focal length over the focal length, else the
/// lens's own. A prime without a recorded focal length takes its own; a zoom without one gives
/// ::arraw::LensProblem::LensFocalUnknown.
/// @param facts What the photograph says about its camera and lens.
/// @param database Database to look in.
/// @return The match, or why there is none.
[[nodiscard]] LensMatchResult matchLens(const LensFacts& facts, const LensDatabase& database);

} // namespace arraw
