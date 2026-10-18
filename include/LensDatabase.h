#pragma once

#include <LensProblem.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arraw {

/// @brief Tells whether this build can read a lens database at all.
///
/// `false` for a build configured with `ARRAW_WITH_LENSFUN=OFF`, or `AUTO` where lensfun was
/// not found. Such a build still runs; a lens correction asked of it is reported as
/// unavailable rather than rendered as if there were no profile (ADR 050).
/// @return Whether lensfun is compiled in.
[[nodiscard]] bool lensfunAvailable() noexcept;

/// @brief Names the lensfun this build was compiled against.
/// @return The version as `major.minor.micro`, such as `0.3.4`, or an empty string when
/// ::arraw::lensfunAvailable is `false`.
[[nodiscard]] const char* lensfunVersion() noexcept;

/// @brief A camera as the lens database lists it.
struct CameraEntry {
    std::string maker;       ///< Maker, as lensfun spells it.
    std::string model;       ///< Model, as lensfun spells it.
    std::string mount;       ///< Name of the lens mount; empty for none.
    double cropFactor = 1.0; ///< Diagonal of a full frame over the sensor's.

    friend bool operator==(const CameraEntry&, const CameraEntry&) = default;
};

/// @brief A lens as the lens database lists it.
struct LensEntry {
    std::string maker; ///< Maker, as lensfun spells it.
    std::string
        model; ///< Model, as lensfun spells it; with ::arraw::LensEntry::maker the lens's identity.
    std::vector<std::string> mounts; ///< Mounts the lens fits.
    double cropFactor = 1.0;         ///< Crop factor of the camera its profile was measured on.
    double minFocal = 0.0;           ///< Shortest focal length, in millimetres; zero when unknown.
    double maxFocal = 0.0;           ///< Longest focal length, in millimetres; zero when unknown.
    double widestAperture =
        0.0;       ///< Smallest f-number, as lensfun's `MinAperture`; zero when unknown.
    int score = 0; ///< How well a search matched it; higher is better, zero outside a search.

    friend bool operator==(const LensEntry&, const LensEntry&) = default;
};

/// @brief What a lens search is constrained by.
struct LensSearchLimits {
    /// @brief Mount of the camera; the lenses that do not fit it, directly or by the mount's
    /// compatibility list, are left out. Unset for any mount.
    std::optional<std::string> mount;

    /// @brief Crop factor of the camera; a lens profile measured on a smaller sensor than this
    /// is left out. Unset for any.
    std::optional<double> cropFactor;
};

/// @brief Where to look for a lens database, in order of precedence (ADR 050, section 5).
///
/// The first of the directories that holds a database wins: the explicit one; else the
/// first bundled one; else the system's. A user's update directory, which `lensfun-update-data`
/// fills, is loaded over a bundled or system database that is older than it.
struct LensDatabaseSearch {
    /// @brief A directory named by the caller (`--lens-database`, a test), which alone is used when
    /// set.
    std::optional<std::filesystem::path> explicitDirectory;

    /// @brief Databases shipped beside the executable, tried in order.
    std::vector<std::filesystem::path> bundledDirectories;

    /// @brief The user's update directory, loaded over an older database.
    std::optional<std::filesystem::path> userUpdateDirectory;

    /// @brief System directories, loaded in order when nothing above is found.
    std::vector<std::filesystem::path> systemDirectories;

    /// @brief Whether to fall back to lensfun's own search of the system and the user's
    /// directories when nothing above is found.
    bool lensfunDefaults = false;
};

/// @brief The lens and camera database, loaded once and read-only afterwards (ADR 050).
///
/// Holds no lensfun type in its interface. Every lookup is safe from any thread, behind a mutex
/// that lensfun's undocumented thread safety asks for. Loading takes some time and memory, so
/// ::arraw::LensDatabase::system loads it once per process; a front end asks for it off its
/// interface thread.
///
/// A database that could not be loaded is an object like any other, which finds nothing and
/// says why in ::arraw::LensDatabase::problem; none of the loading functions throws.
class LensDatabase {
public:
    LensDatabase(LensDatabase&&) noexcept;
    LensDatabase& operator=(LensDatabase&&) noexcept;
    LensDatabase(const LensDatabase&) = delete;
    LensDatabase& operator=(const LensDatabase&) = delete;
    ~LensDatabase();

    /// @brief Gives the process's database, loading it on the first call.
    ///
    /// Looks beside the executable first (`../share/lensfun/db`, then `lensfun/db`), for a
    /// bundled package, and then in lensfun's own directories; the user's update replaces
    /// either when newer. Thread-safe. The first call is slow; call it off the interface thread.
    /// @return The database, valid for the life of the program.
    [[nodiscard]] static const LensDatabase& system();

    /// @brief Loads the database in one directory.
    /// @param directory Directory holding lensfun `.xml` files, and optionally a `timestamp.txt`
    /// that names the database's age.
    /// @return The database; ::arraw::LensProblem::LensDatabaseMissing if the directory holds none,
    /// ::arraw::LensProblem::LensfunUnavailable in a build without lensfun.
    [[nodiscard]] static LensDatabase fromDirectory(const std::filesystem::path& directory);

    /// @brief Loads a database from the first place that has one.
    /// @param search Places to look, in order of precedence.
    /// @return The database, as ::arraw::LensDatabase::fromDirectory.
    [[nodiscard]] static LensDatabase fromSearch(const LensDatabaseSearch& search);

    /// @brief Checks whether any database was loaded.
    [[nodiscard]] bool loaded() const noexcept;

    /// @brief Gives why nothing was loaded.
    /// @return The reason, or nothing when ::arraw::LensDatabase::loaded.
    [[nodiscard]] std::optional<LensProblem> problem() const noexcept;

    /// @brief Gives the directory the database came from.
    /// @return The newest directory loaded; empty when none.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;

    /// @brief Gives the database's age, as its newest timestamp.
    /// @return A UTC time such as `2023-11-14T22:13:20Z` (from the directory's `timestamp.txt`,
    /// else its newest file); empty when nothing was loaded.
    [[nodiscard]] const std::string& stamp() const noexcept;

    /// @brief Counts the cameras.
    [[nodiscard]] std::size_t cameraCount() const noexcept;

    /// @brief Counts the lenses.
    [[nodiscard]] std::size_t lensCount() const noexcept;

    /// @brief Finds a camera from what its EXIF says.
    ///
    /// Maker and model are compared as lensfun does, without regard to case; the model must then
    /// be the camera's whole model, so "Canon EOS 80" does not find an 80D.
    /// @param maker EXIF `Make`.
    /// @param model EXIF `Model`.
    /// @return The camera, or nothing when it is not listed.
    [[nodiscard]] std::optional<CameraEntry> findCamera(std::string_view maker,
                                                        std::string_view model) const;

    /// @brief Finds the lenses a name could mean, best first.
    ///
    /// lensfun's own word-based matching of a human description such as `EF-S18-135mm f/3.5-5.6 IS
    /// USM`, within @p limits. Every score is lensfun's; nothing is filtered by it.
    /// @param name Lens name as a photograph records it.
    /// @param limits Mount and crop factor the lenses must fit.
    /// @return The lenses, the most likely first.
    [[nodiscard]] std::vector<LensEntry> findLenses(std::string_view name,
                                                    const LensSearchLimits& limits = {}) const;

    /// @brief Looks a lens up by its exact lensfun name.
    /// @param maker Lens maker, as lensfun spells it.
    /// @param model Lens model, as lensfun spells it.
    /// @param imageCrop Crop factor of the image, to choose among lenses of one name that are
    /// calibrated on different sensors; unset takes the one calibrated on the largest.
    /// @return The lens, or nothing when it is not listed.
    [[nodiscard]] std::optional<LensEntry> lens(std::string_view maker, std::string_view model,
                                                std::optional<double> imageCrop = {}) const;

    /// @brief Lists lenses for a picker, by words of their name.
    /// @param search Words that must all occur, in any order and case, in the lens's maker and
    /// model; empty for every lens.
    /// @param mount Mount of the camera the lenses must fit, or nothing.
    /// @return The lenses, ordered by maker and model.
    [[nodiscard]] std::vector<LensEntry> lenses(std::string_view search,
                                                const std::optional<std::string>& mount) const;

    /// @brief Gives the mount of a camera.
    /// @return Its mount, or nothing when the camera is not listed or has none.
    [[nodiscard]] std::optional<std::string> mountOf(std::string_view maker,
                                                     std::string_view model) const;

private:
    class Impl;
    friend struct LensfunAccess;
    explicit LensDatabase(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// @brief Counts how many times ::arraw::LensDatabase::system has loaded the database.
///
/// A hook for the test that checks that threads share one load; not otherwise of use.
/// @return Zero before the first load, one afterwards.
[[nodiscard]] std::size_t systemLensDatabaseLoads() noexcept;

} // namespace arraw
