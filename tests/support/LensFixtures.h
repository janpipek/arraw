#pragma once

#include "Fixtures.h"

#include <LensDatabase.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw::test {

/// @brief Gives the small lensfun database of `tests/fixtures/lensfun`, loaded once.
///
/// Calibrations invented for the tests: the logic under test is arraw's matching, mapping and
/// kernel, never lensfun's own models. Not loaded (and a test using it skipped) in a build
/// without lensfun.
/// @return The database.
[[nodiscard]] inline const LensDatabase& fixtureDatabase() {
    static const LensDatabase database = LensDatabase::fromDirectory(fixture("lensfun"));
    return database;
}

/// @brief Writes a lensfun database of one camera and one lens into a directory.
/// @param directory Existing directory to write `lens.xml` and `timestamp.txt` in.
/// @param lensModel Model of the one lens; its maker is "Test".
/// @param timestamp Contents of `timestamp.txt`, the database's age in seconds since 1970.
inline void writeTinyDatabase(const std::filesystem::path& directory, std::string_view lensModel,
                              long long timestamp) {
    std::ofstream(directory / "lens.xml")
        << "<lensdatabase version=\"1\"><mount><name>tiny</name></mount>"
           "<camera><maker>Test</maker><model>Body</model><mount>tiny</mount>"
           "<cropfactor>1.5</cropfactor></camera>"
           "<lens><maker>Test</maker><model>"
        << lensModel
        << "</model><mount>tiny</mount><cropfactor>1.5</cropfactor>"
           "<calibration><distortion model=\"ptlens\" focal=\"50\" a=\"0\" b=\"-0.02\" c=\"0\"/>"
           "</calibration></lens></lensdatabase>";
    std::ofstream(directory / "timestamp.txt") << timestamp << "\n";
}

/// @brief Writes a database whose lenses share one name and differ in calibration.
/// @param directory Existing directory to write `lens.xml` and `timestamp.txt` in.
/// @param calibrations Pairs of (calibration crop factor, distortion `b`), in file order; every
/// lens is "Test Dup 50mm" of maker "Test" on the mount "tiny".
inline void writeDuplicateDatabase(const std::filesystem::path& directory,
                                   const std::vector<std::pair<double, double>>& calibrations) {
    std::ofstream out(directory / "lens.xml");
    out << "<lensdatabase version=\"1\"><mount><name>tiny</name></mount>"
           "<camera><maker>Test</maker><model>Body</model><mount>tiny</mount>"
           "<cropfactor>1.6</cropfactor></camera>";
    for (const auto& [crop, b] : calibrations) {
        out << "<lens><maker>Test</maker><model>Test Dup 50mm</model><mount>tiny</mount>"
               "<cropfactor>"
            << crop
            << "</cropfactor><calibration><distortion model=\"ptlens\" focal=\"50\" a=\"0\" b=\""
            << b << "\" c=\"0\"/></calibration></lens>";
    }
    out << "</lensdatabase>";
    out.close();
    std::ofstream(directory / "timestamp.txt") << 1700000000 << "\n";
}

} // namespace arraw::test
