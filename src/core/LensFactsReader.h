#pragma once

#include <LensFacts.h>

#include <filesystem>

/// @brief Reading what a RAW file says about its lens, from its EXIF and maker note (ADR 050).
namespace arraw::lensfacts {

/// @brief Adds what a file's EXIF and maker note say to what the RAW decoder already told.
///
/// The decoder's values stand where the file says nothing; the EXIF's, which carry the camera's
/// own spelling of its names, come first among the names and replace the camera strings, the focal
/// length, the aperture and the 35 mm focal length. A file exiv2 cannot read leaves @p facts as
/// it was: facts are a help to matching, never a reason to fail an open.
/// @param path File to read.
/// @param facts The decoder's facts, completed in place.
void complete(const std::filesystem::path& path, LensFacts& facts);

/// @brief Writes a name once among those in a list, if it says anything.
///
/// Trims it, drops it if empty or only dashes (a camera writes `----` for no lens), and drops it if
/// another name of the list is the same apart from case and spacing.
/// @param names List to add to.
/// @param name Candidate name.
void addName(std::vector<std::string>& names, std::string name);

} // namespace arraw::lensfacts
