#pragma once

#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <Photo.h>
#include <PhotoMarks.h>

#include <filesystem>
#include <optional>
#include <string_view>

namespace arraw {

/// @brief Namespace of the develop settings in an XMP sidecar; a placeholder until confirmed.
inline constexpr std::string_view sidecarNamespace = "http://ns.arraw.org/develop/1.0/";

/// @brief Prefix the develop settings get when a sidecar has to declare their namespace.
inline constexpr std::string_view sidecarPrefix = "arraw";

/// @brief Newest version of the sidecar's develop settings this arraw reads and writes.
inline constexpr int sidecarVersion = 1;

/// @brief What an XMP sidecar holds that arraw understands.
struct SidecarContents {
    /// @brief Develop settings, defaults for whatever the sidecar does not say.
    DevelopSettings settings;

    /// @brief Culling marks, none for whatever the sidecar does not say.
    PhotoMarks marks;
};

/// @brief Names the sidecar of a photograph.
///
/// The sidecar is `<stem>.xmp` beside the photograph, as Lightroom names it. A
/// photograph that shares its stem with another image in the same directory
/// gets `<name>.<extension>.xmp` instead, so that a pair never share one; the
/// exception is a RAW, which keeps `<stem>.xmp` unless another RAW shares its
/// stem. The RAW extensions are those LibRaw opens, and the other images are
/// found by probing the usual extensions in lower and upper case, so the cost
/// does not grow with the directory. When `<name>.XMP` exists and `<name>.xmp`
/// does not, that file is named. Looks at the directory; nothing is created.
/// @param photo Path of the photograph.
/// @return Path of its sidecar, which need not exist.
[[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& photo);

/// @brief Reads the sidecar of a photograph, if it has one.
///
/// Every `rdf:Description` is searched. The develop settings are the
/// `arraw:` properties, as attributes or as simple child elements, applied onto
/// defaults through the same codec as the JSON document, so a value out of
/// range is clamped, and an unknown key or a value of the wrong shape is
/// skipped, each with a warning that names the sidecar. So are `xmp:Rating`
/// (a whole number, `3.0` included, clamped to -1 to 5) and `xmp:Label`. A
/// rating or label that does not fit is reported, and its text kept in the
/// ::arraw::PhotoMarks so that a write gives it back. `arraw:version` above
/// ::arraw::sidecarVersion gives ::arraw::Notice::NewerSettingsVersion.
/// `crs:` and anything else is not read.
/// @param photo Path of the photograph.
/// @param log Where the warnings go.
/// @return What the sidecar holds, or nothing when there is no sidecar.
/// @throws std::runtime_error if the sidecar cannot be read or is not XML,
/// naming it.
[[nodiscard]] std::optional<SidecarContents>
readSidecar(const std::filesystem::path& photo, DiagnosticLog& log = discardedDiagnostics());

/// @brief Writes the develop settings and marks of a photograph into its sidecar.
///
/// An existing sidecar is edited rather than replaced: everything arraw does
/// not own, such as `crs:` settings, other namespaces, unknown elements and
/// unknown `arraw:` attributes, is kept. Every settings key and `arraw:version`
/// becomes an attribute of the `rdf:Description` that already holds `arraw:`
/// content, or of the first one; an unset optional has no attribute, and a
/// child-element form of any key arraw owns is removed, so the file says each
/// thing once. `xmp:Rating` is always written, `xmp:Label` when set; a rating
/// or label kept from reading (see ::arraw::PhotoMarks) is written as it was
/// until the mark is changed. Without a sidecar, a new one is created. The
/// file is replaced atomically. What is preserved is the meaning of the
/// document, not its bytes: attributes may be reordered, the packet's padding
/// and trailing newline are dropped, and the declaration is rewritten as UTF-8.
/// @param photo Photograph whose settings and marks to write.
/// @throws std::runtime_error if the existing sidecar is not XMP, or is of a
/// newer ::arraw::sidecarVersion, naming it, in which case it is left alone,
/// or if the file cannot be written.
void writeSidecar(const Photo& photo);

} // namespace arraw
