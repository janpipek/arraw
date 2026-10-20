#include "LensfunSource.h"

#include <LensDatabase.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#ifdef ARRAW_HAS_LENSFUN
#include <lensfun.h>

#ifdef _WIN32
#include <windows.h>
#endif
#endif

// The 0.3 series and its version 1 database (ADR 050, section 5). A 0.3.95 or newer library
// reads a version 2 database and changes some signatures; it gets its own branch when a
// supported platform ships it.
#ifdef ARRAW_HAS_LENSFUN
static_assert(LF_VERSION >= ((0 << 24) | (3 << 16)), "arraw needs lensfun 0.3 or newer");
#define ARRAW_LENSFUN_STRINGIZE_(x) #x
#define ARRAW_LENSFUN_STRINGIZE(x) ARRAW_LENSFUN_STRINGIZE_(x)
#endif

namespace fs = std::filesystem;

namespace arraw {
namespace {

std::atomic<std::size_t> systemLoads{0};

/// @brief Lower-cases ASCII, which is all the names in lensfun's database need.
std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

/// @brief Reduces a name to its words, lower-cased and single-spaced.
std::string squeezed(std::string_view text) {
    std::string result;
    bool space = false;
    for (const char character : lowered(text)) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            space = !result.empty();
        } else {
            if (space) {
                result += ' ';
                space = false;
            }
            result += character;
        }
    }
    return result;
}

/// @brief Checks whether a directory holds at least one lensfun XML file.
bool holdsDatabase(const fs::path& directory) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error) && lowered(entry.path().extension().string()) == ".xml") {
            return true;
        }
    }
    return false;
}

/// @brief Gives the age of the database in a directory, in seconds since 1970.
///
/// What `lensfun-update-data` and the distributions' packages write in `timestamp.txt`, else the
/// newest XML file's modification time.
/// @return The age, or nothing when the directory holds no database.
std::optional<std::int64_t> ageOf(const fs::path& directory) {
    if (!holdsDatabase(directory)) {
        return std::nullopt;
    }
    if (std::ifstream file(directory / "timestamp.txt"); file) {
        std::int64_t seconds = 0;
        if (file >> seconds && seconds > 0) {
            return seconds;
        }
    }
    std::int64_t newest = 0;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || lowered(entry.path().extension().string()) != ".xml") {
            continue;
        }
        const auto written = fs::last_write_time(entry.path(), error);
        if (error) {
            continue;
        }
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch())
                .count();
        newest = std::max<std::int64_t>(newest, seconds);
    }
    return newest;
}

/// @brief Writes seconds since 1970 as a UTC time, `2023-11-14T22:13:20Z`.
std::string formatStamp(std::int64_t seconds) {
    const auto time = static_cast<std::time_t>(seconds);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &time);
#else
    gmtime_r(&time, &parts);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

} // namespace

bool lensfunAvailable() noexcept {
#ifdef ARRAW_HAS_LENSFUN
    return true;
#else
    return false;
#endif
}

const char* lensfunVersion() noexcept {
#ifdef ARRAW_HAS_LENSFUN
    return ARRAW_LENSFUN_STRINGIZE(LF_VERSION_MAJOR) "." ARRAW_LENSFUN_STRINGIZE(
        LF_VERSION_MINOR) "." ARRAW_LENSFUN_STRINGIZE(LF_VERSION_MICRO);
#else
    return "";
#endif
}

std::size_t systemLensDatabaseLoads() noexcept {
    return systemLoads.load();
}

#ifdef ARRAW_HAS_LENSFUN

// -------------------------------------------------------------------------------------------
// With lensfun.

class LensDatabase::Impl {
public:
    lfDatabase database;
    mutable std::mutex mutex;
    fs::path directory;
    std::string stamp;
    std::int64_t age = 0;
    std::optional<LensProblem> problem = LensProblem::LensDatabaseMissing;

    /// @brief Loads one directory over what is there, remembering the newest.
    bool load(const fs::path& from) {
        const auto seconds = ageOf(from);
        if (!seconds || !database.LoadDirectory(from.string().c_str())) {
            return false;
        }
        if (*seconds >= age) {
            age = *seconds;
            directory = from;
            stamp = formatStamp(age);
        }
        problem.reset();
        return true;
    }
};

namespace {

/// @brief Gives the directory holding the running executable.
fs::path executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return fs::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : executable.parent_path();
#else
    return {};
#endif
}

/// @brief Gives lensfun's own places for a database, as far as they can be told.
///
/// lensfun's `Load()` reads them itself; this only finds the directory and the age to report.
std::vector<fs::path> defaultCandidates(const lfDatabase& database) {
    std::vector<fs::path> bases{"/usr/share/lensfun", "/usr/local/share/lensfun"};
    if (database.HomeDataDir != nullptr) {
        bases.emplace_back(database.HomeDataDir);
    }
    if (database.UserUpdatesDir != nullptr) {
        bases.emplace_back(database.UserUpdatesDir);
    }
    std::vector<fs::path> candidates;
    for (const fs::path& base : bases) {
        candidates.push_back(base / "version_1");
        candidates.push_back(base / "updates" / "version_1");
        candidates.push_back(base);
    }
    return candidates;
}

/// @brief Gives a null-terminated array's length.
template <typename T> std::size_t countOf(const T* const* array) {
    std::size_t count = 0;
    for (; array != nullptr && array[count] != nullptr; ++count) {
    }
    return count;
}

/// @brief Reads a lensfun string, which may be null.
std::string textOf(const char* text) {
    return text != nullptr ? text : "";
}

/// @brief Widens one of lensfun's floats to the number it was written as.
///
/// The database holds decimals of a few digits, which a float cannot hold; 1.6 would come out as
/// 1.60000002, and then be written so into a sidecar.
double decimal(float value) {
    return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
}

LensEntry entryOf(const lfLens& lens) {
    LensEntry entry{.maker = textOf(lens.Maker),
                    .model = textOf(lens.Model),
                    .mounts = {},
                    .cropFactor = decimal(lens.CropFactor),
                    .minFocal = decimal(lens.MinFocal),
                    .maxFocal = decimal(lens.MaxFocal),
                    .widestAperture = decimal(lens.MinAperture),
                    .score = lens.Score};
    for (std::size_t index = 0; lens.Mounts != nullptr && lens.Mounts[index] != nullptr; ++index) {
        entry.mounts.emplace_back(lens.Mounts[index]);
    }
    return entry;
}

/// @brief Checks whether a lens fits a camera mount, as lensfun's mount list says.
bool fitsMount(const lfDatabase& database, const lfLens& lens, const std::string& mount) {
    const lfMount* camera = database.FindMount(mount.c_str());
    for (std::size_t index = 0; lens.Mounts != nullptr && lens.Mounts[index] != nullptr; ++index) {
        const std::string_view lensMount = lens.Mounts[index];
        if (lensMount == mount) {
            return true;
        }
        for (std::size_t compat = 0;
             camera != nullptr && camera->Compat != nullptr && camera->Compat[compat] != nullptr;
             ++compat) {
            if (lensMount == camera->Compat[compat]) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

LensDatabase::LensDatabase(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LensDatabase::LensDatabase(LensDatabase&&) noexcept = default;
LensDatabase& LensDatabase::operator=(LensDatabase&&) noexcept = default;
LensDatabase::~LensDatabase() = default;

LensDatabase LensDatabase::fromDirectory(const fs::path& directory) {
    auto impl = std::make_unique<Impl>();
    impl->load(directory);
    return LensDatabase(std::move(impl));
}

LensDatabase LensDatabase::fromSearch(const LensDatabaseSearch& search) {
    if (search.explicitDirectory) {
        return fromDirectory(*search.explicitDirectory);
    }
    auto impl = std::make_unique<Impl>();
    bool found = false;
    for (const fs::path& bundled : search.bundledDirectories) {
        if (!bundled.empty() && impl->load(bundled)) {
            found = true;
            break;
        }
    }
    if (!found) {
        for (const fs::path& system : search.systemDirectories) {
            found = (!system.empty() && impl->load(system)) || found;
        }
    }
    if (!found && search.lensfunDefaults && impl->database.Load() == LF_NO_ERROR) {
        // lensfun read its own places, the user's update over the system's when newer. Report
        // the newest of them as the one in use.
        for (const fs::path& candidate : defaultCandidates(impl->database)) {
            if (const auto age = ageOf(candidate); age && *age >= impl->age) {
                impl->age = *age;
                impl->directory = candidate;
                impl->stamp = formatStamp(*age);
            }
        }
        impl->problem.reset();
        return LensDatabase(std::move(impl));
    }
    if (found && search.userUpdateDirectory) {
        // An update the user fetched replaces a database that is older than it, as lensfun's
        // own search does for the system's.
        if (const auto age = ageOf(*search.userUpdateDirectory); age && *age > impl->age) {
            impl->load(*search.userUpdateDirectory);
        }
    }
    return LensDatabase(std::move(impl));
}

const LensDatabase& LensDatabase::system() {
    static const LensDatabase database = [] {
        ++systemLoads;
        LensDatabaseSearch search;
        if (const fs::path executable = executableDirectory(); !executable.empty()) {
            search.bundledDirectories = {executable / ".." / "share" / "lensfun" / "db",
                                         executable / "lensfun" / "db"};
        }
        const lfDatabase probe;
        if (probe.UserUpdatesDir != nullptr) {
            search.userUpdateDirectory = fs::path(probe.UserUpdatesDir) / "version_1";
        }
        search.lensfunDefaults = true;
        return fromSearch(search);
    }();
    return database;
}

bool LensDatabase::loaded() const noexcept {
    return !impl_->problem;
}

std::optional<LensProblem> LensDatabase::problem() const noexcept {
    return impl_->problem;
}

const fs::path& LensDatabase::directory() const noexcept {
    return impl_->directory;
}

const std::string& LensDatabase::stamp() const noexcept {
    return impl_->stamp;
}

std::size_t LensDatabase::cameraCount() const noexcept {
    const std::scoped_lock lock(impl_->mutex);
    return countOf(impl_->database.GetCameras());
}

std::size_t LensDatabase::lensCount() const noexcept {
    const std::scoped_lock lock(impl_->mutex);
    return countOf(impl_->database.GetLenses());
}

std::optional<CameraEntry> LensDatabase::findCamera(std::string_view maker,
                                                    std::string_view model) const {
    if (!loaded() || model.empty()) {
        return std::nullopt;
    }
    const std::string makerText(maker);
    const std::string modelText(model);
    const std::scoped_lock lock(impl_->mutex);
    const lfCamera** found =
        impl_->database.FindCameras(maker.empty() ? nullptr : makerText.c_str(), modelText.c_str());
    std::optional<CameraEntry> result;
    // lensfun finds cameras by words; accept only the one whose model is the whole of the EXIF's,
    // so that a model that merely contains another's words cannot take its profile.
    const std::string wanted = squeezed(model);
    for (std::size_t index = 0; found != nullptr && found[index] != nullptr; ++index) {
        const lfCamera& camera = *found[index];
        if (squeezed(textOf(camera.Model)) == wanted) {
            result = CameraEntry{.maker = textOf(camera.Maker),
                                 .model = textOf(camera.Model),
                                 .mount = textOf(camera.Mount),
                                 .cropFactor = decimal(camera.CropFactor)};
            break;
        }
    }
    lf_free(found);
    return result;
}

std::vector<LensEntry> LensDatabase::findLenses(std::string_view name,
                                                const LensSearchLimits& limits) const {
    std::vector<LensEntry> result;
    if (!loaded() || name.empty()) {
        return result;
    }
    // The camera lensfun searches by is a stand-in that carries only what we know of it. An
    // unknown crop factor is set high, since lensfun drops lenses measured on a larger crop
    // than the camera's.
    lfCamera camera;
    if (limits.mount) {
        camera.SetMount(limits.mount->c_str());
    }
    camera.CropFactor = static_cast<float>(limits.cropFactor.value_or(100.0));
    const std::string nameText(name);
    const std::scoped_lock lock(impl_->mutex);
    const lfLens** found = impl_->database.FindLenses(&camera, nullptr, nameText.c_str());
    for (std::size_t index = 0; found != nullptr && found[index] != nullptr; ++index) {
        result.push_back(entryOf(*found[index]));
    }
    lf_free(found);
    return result;
}

namespace {

/// @brief Finds a lens by its exact names, and of lenses sharing them the calibration for a sensor.
///
/// lensfun's database lists some lenses more than once, each calibrated on another crop factor.
/// As lensfun itself prefers, the one chosen is calibrated on the largest crop factor that is no
/// greater than the image's (within a percent); when there is none, the smallest. Ties keep the
/// database's order.
const lfLens* findLens(const lfDatabase& database, const LensIdentity& identity, double imageCrop) {
    const lfLens* const* all = database.GetLenses();
    const lfLens* best = nullptr;
    const auto fits = [&](const lfLens& lens) { return lens.CropFactor <= imageCrop * 1.01; };
    for (std::size_t index = 0; all != nullptr && all[index] != nullptr; ++index) {
        const lfLens& lens = *all[index];
        if (textOf(lens.Maker) != identity.maker || textOf(lens.Model) != identity.model) {
            continue;
        }
        const bool better = best == nullptr ||
                            (fits(lens) && (!fits(*best) || lens.CropFactor > best->CropFactor)) ||
                            (!fits(lens) && !fits(*best) && lens.CropFactor < best->CropFactor);
        if (better) {
            best = &lens;
        }
    }
    return best;
}

} // namespace

std::optional<LensEntry> LensDatabase::lens(std::string_view maker, std::string_view model,
                                            std::optional<double> imageCrop) const {
    if (!loaded()) {
        return std::nullopt;
    }
    const std::scoped_lock lock(impl_->mutex);
    const lfLens* found = findLens(impl_->database, {std::string(maker), std::string(model)},
                                   imageCrop.value_or(100.0));
    if (found == nullptr) {
        return std::nullopt;
    }
    return entryOf(*found);
}

std::vector<LensEntry> LensDatabase::lenses(std::string_view search,
                                            const std::optional<std::string>& mount) const {
    std::vector<LensEntry> result;
    if (!loaded()) {
        return result;
    }
    std::vector<std::string> words;
    {
        std::string word;
        for (const char character : lowered(search) + ' ') {
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                if (!word.empty()) {
                    words.push_back(std::exchange(word, {}));
                }
            } else {
                word += character;
            }
        }
    }
    const std::scoped_lock lock(impl_->mutex);
    const lfLens* const* all = impl_->database.GetLenses();
    for (std::size_t index = 0; all != nullptr && all[index] != nullptr; ++index) {
        const lfLens& lens = *all[index];
        const std::string name = lowered(textOf(lens.Maker) + " " + textOf(lens.Model));
        if (!std::ranges::all_of(words, [&](const std::string& word) {
                return name.find(word) != std::string::npos;
            })) {
            continue;
        }
        if (mount && !fitsMount(impl_->database, lens, *mount)) {
            continue;
        }
        result.push_back(entryOf(lens));
    }
    std::ranges::sort(result, [](const LensEntry& a, const LensEntry& b) {
        return std::tie(a.maker, a.model) < std::tie(b.maker, b.model);
    });
    return result;
}

std::optional<std::string> LensDatabase::mountOf(std::string_view maker,
                                                 std::string_view model) const {
    if (const auto camera = findCamera(maker, model); camera && !camera->mount.empty()) {
        return camera->mount;
    }
    return std::nullopt;
}

// -------------------------------------------------------------------------------------------
// Sampling lensfun's models into tables.

struct LensfunAccess {
    static lfDatabase& database(const LensDatabase& from) {
        return from.impl_->database;
    }

    static std::mutex& mutex(const LensDatabase& from) {
        return from.impl_->mutex;
    }
};

namespace {

/// @brief Gives lensfun's optical centre for a lens and a size, in pixel-centre index coordinates.
///
/// lensfun puts the middle of the image at `(size - 1) / 2` and shifts it by the calibrated
/// centre, a fraction of the shorter side's half-extent (checked against its own fixed points in
/// tests/test_LensCorrection.cpp).
std::array<double, 2> centreOf(const lfLens& lens, ImageSize size) {
    const double shorter = std::min(size.width, size.height);
    return {(size.width - 1.0) / 2.0 + lens.CenterX * (shorter - 1.0) / 2.0,
            (size.height - 1.0) / 2.0 + lens.CenterY * (shorter - 1.0) / 2.0};
}

/// @brief What lensfun needs of a photograph besides the lens.
struct Conditions {
    double focal;
    double aperture;
    double distance;
};

Conditions conditionsOf(const LensMatch& match, const LensFacts& facts) {
    return {match.focalLength, facts.aperture.value_or(assumedAperture),
            facts.focusDistance.value_or(assumedDistance)};
}

constexpr int allCorrections = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING;

/// @brief A modifier set up for a photograph.
struct Modifier {
    lfModifier modifier;
    int effective;

    Modifier(const lfLens& lens, const LensMatch& match, const Conditions& conditions,
             ImageSize size, int flags)
        : modifier(&lens, static_cast<float>(match.cropFactor), static_cast<int>(size.width),
                   static_cast<int>(size.height)),
          effective(modifier.Initialize(&lens, LF_PF_F32, static_cast<float>(conditions.focal),
                                        static_cast<float>(conditions.aperture),
                                        static_cast<float>(conditions.distance), 1.0F, lens.Type,
                                        flags, false)) {}
};

LensProvenance provenanceOf(const LensMatch& match, const Conditions& conditions,
                            const LensFacts& facts, const LensDatabase& database) {
    return {.lens = match.lens,
            .cropFactor = match.cropFactor,
            .focalLength = conditions.focal,
            .focalLengthRead = match.focalLengthRead,
            .aperture = conditions.aperture,
            .apertureRead = facts.aperture.has_value(),
            .distance = conditions.distance,
            .distanceRead = facts.focusDistance.has_value(),
            .database = database.stamp()};
}

/// @brief Extrapolates a table's first node from the next two, for a smooth model about the centre.
///
/// The ratio at the centre itself is 0/0; the models are even in the radius, so a parabola
/// through the first two nodes past it gives the limit.
void extrapolateFirst(std::vector<float>& table) {
    table[0] = table[1] - (table[2] - table[1]) / 3.0F;
}

} // namespace

std::optional<LensCorrection> sampleLensfun(const LensDatabase& database, const LensMatch& match,
                                            const LensFacts& facts, ImageSize size, LensKinds which,
                                            std::size_t nodes) {
    if (!database.loaded() || size.empty() || nodes < 4) {
        return std::nullopt;
    }
    const std::scoped_lock lock(LensfunAccess::mutex(database));
    const lfLens* lens = findLens(LensfunAccess::database(database), match.lens, match.cropFactor);
    if (lens == nullptr) {
        return std::nullopt;
    }
    const Conditions conditions = conditionsOf(match, facts);
    int flags = 0;
    flags |= which.distortion ? LF_MODIFY_DISTORTION : 0;
    flags |= which.lateralCa ? LF_MODIFY_TCA : 0;
    flags |= which.vignetting ? LF_MODIFY_VIGNETTING : 0;
    const Modifier setup(*lens, match, conditions, size, flags);
    const lfModifier& modifier = setup.modifier;

    // The ray from the centre to the farthest corner, in index coordinates.
    const auto [centreX, centreY] = centreOf(*lens, size);
    const double left = -0.5;
    const double top = -0.5;
    const double right = size.width - 0.5;
    const double bottom = size.height - 0.5;
    const double cornerX = centreX - left > right - centreX ? left : right;
    const double cornerY = centreY - top > bottom - centreY ? top : bottom;
    const double reach = std::hypot(cornerX - centreX, cornerY - centreY);
    const double directionX = (cornerX - centreX) / reach;
    const double directionY = (cornerY - centreY) / reach;

    LensCorrection result;
    result.centerU = (centreX + 0.5) / size.width;
    result.centerV = (centreY + 0.5) / size.height;
    result.aspect = static_cast<double>(size.width) / size.height;
    const LensProvenance provenance = provenanceOf(match, conditions, facts, database);

    const auto nodeAt = [&](std::size_t index) {
        const double radius = static_cast<double>(index) / (nodes - 1) * reach;
        return std::array<double, 3>{centreX + directionX * radius, centreY + directionY * radius,
                                     radius};
    };

    if ((setup.effective & LF_MODIFY_DISTORTION) != 0) {
        result.distortion.assign(nodes, 1.0F);
        for (std::size_t index = 1; index < nodes; ++index) {
            const auto [x, y, radius] = nodeAt(index);
            float answer[2] = {};
            if (modifier.ApplyGeometryDistortion(static_cast<float>(x), static_cast<float>(y), 1, 1,
                                                 answer)) {
                result.distortion[index] = static_cast<float>(
                    std::hypot(answer[0] - centreX, answer[1] - centreY) / radius);
            }
        }
        extrapolateFirst(result.distortion);
        result.distortionFrom = provenance;
    }
    if ((setup.effective & LF_MODIFY_TCA) != 0) {
        result.redScale.assign(nodes, 1.0F);
        result.blueScale.assign(nodes, 1.0F);
        for (std::size_t index = 1; index < nodes; ++index) {
            const auto [x, y, radius] = nodeAt(index);
            float answer[6] = {};
            // At the sensor position: lensfun's subpixel step comes after its geometry step.
            if (modifier.ApplySubpixelDistortion(static_cast<float>(x), static_cast<float>(y), 1, 1,
                                                 answer)) {
                result.redScale[index] = static_cast<float>(
                    std::hypot(answer[0] - centreX, answer[1] - centreY) / radius);
                result.blueScale[index] = static_cast<float>(
                    std::hypot(answer[4] - centreX, answer[5] - centreY) / radius);
            }
        }
        extrapolateFirst(result.redScale);
        extrapolateFirst(result.blueScale);
        result.lateralCaFrom = provenance;
    }
    if ((setup.effective & LF_MODIFY_VIGNETTING) != 0) {
        result.vignetting.assign(nodes, 1.0F);
        for (std::size_t index = 0; index < nodes; ++index) {
            const auto [x, y, radius] = nodeAt(index);
            (void)radius;
            float pixel[3] = {1.0F, 1.0F, 1.0F};
            // A white pixel comes back as the gain that restores it.
            if (modifier.ApplyColorModification(pixel, static_cast<float>(x), static_cast<float>(y),
                                                1, 1, LF_CR_3(RED, GREEN, BLUE),
                                                3 * static_cast<int>(sizeof(float)))) {
                result.vignetting[index] = pixel[1];
            }
        }
        result.vignettingFrom = provenance;
    }
    return withFillScale(std::move(result));
}

class detail::LensfunReference::Impl {
public:
    const LensDatabase& database;
    std::mutex& mutex;
    const lfLens* lens = nullptr;
    std::unique_ptr<Modifier> modifier;
    std::array<double, 2> centre{};

    Impl(const LensDatabase& from, const LensMatch& match, const LensFacts& facts, ImageSize size)
        : database(from), mutex(LensfunAccess::mutex(from)) {
        const std::scoped_lock lock(mutex);
        lens = findLens(LensfunAccess::database(from), match.lens, match.cropFactor);
        if (lens != nullptr) {
            modifier = std::make_unique<Modifier>(*lens, match, conditionsOf(match, facts), size,
                                                  allCorrections);
            centre = centreOf(*lens, size);
        }
    }
};

detail::LensfunReference::LensfunReference(const LensDatabase& database, const LensMatch& match,
                                           const LensFacts& facts, ImageSize size)
    : impl_(std::make_unique<Impl>(database, match, facts, size)) {}

detail::LensfunReference::~LensfunReference() = default;

bool detail::LensfunReference::valid() const noexcept {
    return impl_->lens != nullptr;
}

std::array<double, 2> detail::LensfunReference::centre() const noexcept {
    return impl_->centre;
}

std::array<std::array<double, 2>, 3> detail::LensfunReference::sourcePositions(double x,
                                                                               double y) const {
    float answer[6] = {};
    const std::scoped_lock lock(impl_->mutex);
    impl_->modifier->modifier.ApplySubpixelGeometryDistortion(static_cast<float>(x),
                                                              static_cast<float>(y), 1, 1, answer);
    return {{{answer[0], answer[1]}, {answer[2], answer[3]}, {answer[4], answer[5]}}};
}

double detail::LensfunReference::gainAt(double x, double y) const {
    float pixel[3] = {1.0F, 1.0F, 1.0F};
    const std::scoped_lock lock(impl_->mutex);
    impl_->modifier->modifier.ApplyColorModification(
        pixel, static_cast<float>(x), static_cast<float>(y), 1, 1, LF_CR_3(RED, GREEN, BLUE),
        3 * static_cast<int>(sizeof(float)));
    return pixel[1];
}

#else

// -------------------------------------------------------------------------------------------
// Without lensfun: a database that was never loaded, and says so.

class LensDatabase::Impl {
public:
    std::filesystem::path directory;
    std::string stamp;
    std::optional<LensProblem> problem = LensProblem::LensfunUnavailable;
};

LensDatabase::LensDatabase(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LensDatabase::LensDatabase(LensDatabase&&) noexcept = default;
LensDatabase& LensDatabase::operator=(LensDatabase&&) noexcept = default;
LensDatabase::~LensDatabase() = default;

LensDatabase LensDatabase::fromDirectory(const fs::path&) {
    return LensDatabase(std::make_unique<Impl>());
}

LensDatabase LensDatabase::fromSearch(const LensDatabaseSearch&) {
    return LensDatabase(std::make_unique<Impl>());
}

const LensDatabase& LensDatabase::system() {
    static const LensDatabase database = [] {
        ++systemLoads;
        return LensDatabase(std::make_unique<Impl>());
    }();
    return database;
}

bool LensDatabase::loaded() const noexcept {
    return false;
}

std::optional<LensProblem> LensDatabase::problem() const noexcept {
    return impl_->problem;
}

const fs::path& LensDatabase::directory() const noexcept {
    return impl_->directory;
}

const std::string& LensDatabase::stamp() const noexcept {
    return impl_->stamp;
}

std::size_t LensDatabase::cameraCount() const noexcept {
    return 0;
}

std::size_t LensDatabase::lensCount() const noexcept {
    return 0;
}

std::optional<CameraEntry> LensDatabase::findCamera(std::string_view, std::string_view) const {
    return std::nullopt;
}

std::vector<LensEntry> LensDatabase::findLenses(std::string_view, const LensSearchLimits&) const {
    return {};
}

std::optional<LensEntry> LensDatabase::lens(std::string_view, std::string_view,
                                            std::optional<double>) const {
    return std::nullopt;
}

std::vector<LensEntry> LensDatabase::lenses(std::string_view,
                                            const std::optional<std::string>&) const {
    return {};
}

std::optional<std::string> LensDatabase::mountOf(std::string_view, std::string_view) const {
    return std::nullopt;
}

std::optional<LensCorrection> sampleLensfun(const LensDatabase&, const LensMatch&, const LensFacts&,
                                            ImageSize, LensKinds, std::size_t) {
    return std::nullopt;
}

class detail::LensfunReference::Impl {};

detail::LensfunReference::LensfunReference(const LensDatabase&, const LensMatch&, const LensFacts&,
                                           ImageSize) {}

detail::LensfunReference::~LensfunReference() = default;

bool detail::LensfunReference::valid() const noexcept {
    return false;
}

std::array<double, 2> detail::LensfunReference::centre() const noexcept {
    return {};
}

std::array<std::array<double, 2>, 3> detail::LensfunReference::sourcePositions(double,
                                                                               double) const {
    return {};
}

double detail::LensfunReference::gainAt(double, double) const {
    return 1.0;
}

#endif

} // namespace arraw
