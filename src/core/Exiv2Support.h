#pragma once

#include <filesystem>
#include <string>

/// @brief What every use of exiv2 shares: its one-time set-up and its way of spelling a path.
///
/// Reading EXIF (::arraw::readExif) and finding embedded previews
/// (::arraw::readEmbeddedPreview) open files through the same library, so they
/// are made thread-safe and quiet in the same place (ADR 028).
namespace arraw::exiv2support {

/// @brief Prepares exiv2 for use from any thread, once.
///
/// Gives the XMP toolkit a lock and mutes exiv2's own log, which a library
/// must not write to stderr. Call before the first exiv2 object is made; it is
/// cheap and safe to call every time.
void prepare();

/// @brief Spells a path the way exiv2 0.28 opens it.
///
/// It takes a narrow string: UTF-8 on Windows, which it widens itself, and the
/// path's own bytes elsewhere.
/// @param path Path to spell.
/// @return The path as exiv2 takes it.
[[nodiscard]] std::string path(const std::filesystem::path& path);

} // namespace arraw::exiv2support
