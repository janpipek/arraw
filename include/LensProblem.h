#pragma once

namespace arraw {

/// @brief Why a lens correction could not be resolved for a photograph (ADR 050, section 6).
///
/// The reasons the lens database and the matcher give, by name. Each is a fact about one
/// photograph or one database, never an error to abort on: the switches stay as set, the
/// correction is skipped, and whoever asked is told why.
enum class LensProblem {
    /// @brief The photograph names no lens that can be told from another, such as an adapted
    /// manual lens.
    LensUnidentified,

    /// @brief A zoom was used and its focal length was not recorded, so no profile applies.
    LensFocalUnknown,

    /// @brief The camera is not in the database. Informational: matching goes on without it.
    CameraNotInDatabase,

    /// @brief No lens in the database fits the name, the focal range, the aperture and the mount.
    LensNotInDatabase,

    /// @brief Two lenses fit equally well, so none is applied; a photographer chooses.
    LensAmbiguous,

    /// @brief A recorded lens is no longer in the database.
    LensProfileMissing,

    /// @brief The database holds another stamp than the one a lens was recorded against.
    /// Informational: the same lens's calibration may have been revised.
    LensDatabaseChanged,

    /// @brief No lens database could be loaded from the directory or the places searched.
    LensDatabaseMissing,

    /// @brief This build has no lensfun (`ARRAW_WITH_LENSFUN`).
    LensfunUnavailable,
};

} // namespace arraw
