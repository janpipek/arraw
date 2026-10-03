#pragma once

#include <DevelopState.h>
#include <Diagnostics.h>
#include <Photo.h>
#include <PhotoMarks.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw {

/// @brief Namespace of the develop settings in an XMP sidecar; a placeholder until confirmed.
inline constexpr std::string_view sidecarNamespace = "http://ns.arraw.org/develop/1.0/";

/// @brief Prefix the develop settings get when a sidecar has to declare their namespace.
inline constexpr std::string_view sidecarPrefix = "arraw";

/// @brief Newest version of the sidecar's develop settings this arraw reads and writes.
inline constexpr int sidecarVersion = 1;

/// @brief A namespace other tools left properties in, as a sidecar shows it.
struct ForeignNamespace {
    /// @brief Namespace URI.
    std::string uri;

    /// @brief Prefix of its first counted property, empty for the default namespace.
    std::string prefix;

    /// @brief Number of top-level properties of the `rdf:Description`s in this namespace.
    int properties = 0;

    friend bool operator==(const ForeignNamespace&, const ForeignNamespace&) = default;
};

/// @brief What an XMP sidecar holds that arraw understands, and who else wrote in it.
struct SidecarContents {
    /// @brief Develop state, defaults for whatever the sidecar does not say.
    DevelopState state;

    /// @brief Culling marks, none for whatever the sidecar does not say.
    PhotoMarks marks;

    /// @brief Program that wrote the sidecar, from `xmp:CreatorTool`.
    std::optional<std::string> creatorTool;

    /// @brief Namespaces of other tools, in the order of their first counted property.
    ///
    /// Every namespace with a property on an `rdf:Description` other than arraw's,
    /// `rdf:`, `x:`, `xmp:` and the XML ones, so `dc:` is listed. Only top-level
    /// properties are counted, not what a property holds. Among namespaces that
    /// appear only as attributes of one description, the order is by attribute name.
    std::vector<ForeignNamespace> others;

    friend bool operator==(const SidecarContents&, const SidecarContents&) = default;
};

/// @brief A known XMP namespace and the tool or standard behind it.
struct XmpNamespaceOwner {
    /// @brief Namespace URI.
    std::string_view uri;

    /// @brief Name of the tool or vocabulary.
    std::string_view name;
};

/// @brief Namespaces ::arraw::xmpNamespaceOwner knows.
inline constexpr auto xmpNamespaceOwners = std::to_array<XmpNamespaceOwner>({
    {"http://ns.adobe.com/camera-raw-settings/1.0/",
     "Adobe Camera Raw / Lightroom develop settings"},
    {"http://ns.adobe.com/lightroom/1.0/", "Lightroom hierarchical keywords"},
    {"http://ns.adobe.com/photoshop/1.0/", "Photoshop / IPTC fields"},
    {"http://ns.adobe.com/xap/1.0/mm/", "XMP media management"},
    {"http://ns.adobe.com/exif/1.0/aux/", "EXIF auxiliary (copied)"},
    {"http://cipa.jp/exif/1.0/", "EXIF extended (copied)"},
    {"http://ns.adobe.com/xap/1.0/rights/", "XMP rights"},
    {"http://iptc.org/std/Iptc4xmpExt/2008-02-29/", "IPTC Extension"},
    {"http://ns.microsoft.com/photo/1.2/", "Microsoft Photo (people regions)"},
    {"http://www.metadataworkinggroup.com/schemas/regions/", "MWG regions"},
    {"http://darktable.sf.net/", "darktable"},
    {"http://www.digikam.org/ns/1.0/", "digiKam"},
    {"http://ns.microsoft.com/photo/1.0/", "Windows Photo Gallery"},
    {"http://ns.adobe.com/exif/1.0/", "EXIF (copied)"},
    {"http://ns.adobe.com/tiff/1.0/", "TIFF (copied)"},
    {"http://purl.org/dc/elements/1.1/", "Dublin Core"},
    {"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/", "IPTC Core"},
});

/// @brief Names the tool or standard behind an XMP namespace.
/// @param uri Namespace URI, compared exactly.
/// @return Its human name, or nothing for a namespace not in ::arraw::xmpNamespaceOwners.
[[nodiscard]] std::optional<std::string_view> xmpNamespaceOwner(std::string_view uri);

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
/// rating or label that does not fit is reported and read as no mark; the file
/// keeps it (see ::arraw::writeSidecar). `arraw:version` above
/// ::arraw::sidecarVersion gives ::arraw::Notice::NewerSettingsVersion.
/// `crs:` and anything else is not read, but it is reported: `xmp:CreatorTool`
/// and the other namespaces that hold properties (see ::arraw::SidecarContents).
/// @param photo Path of the photograph, which is also the subject of every warning.
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
/// thing once. `xmp:Rating` and `xmp:Label` are written only when the
/// photograph's marks differ from what reading the file gives, so a rating or
/// label arraw cannot represent (`9`, `Rot`) stays as another tool wrote it
/// until the mark is changed; clearing the label removes it. Without a
/// sidecar, a new one is created. The
/// file is replaced atomically. What is preserved is the meaning of the
/// document, not its bytes: attributes may be reordered, the packet's padding
/// and trailing newline are dropped, and the declaration is rewritten as UTF-8.
/// @param photo Photograph whose settings and marks to write.
/// @throws std::runtime_error if the existing sidecar is not XMP, or is of a
/// newer ::arraw::sidecarVersion, naming it, in which case it is left alone,
/// or if the file cannot be written.
void writeSidecar(const Photo& photo);

/// @brief Writes only the marks of a photograph into its sidecar.
///
/// As ::arraw::writeSidecar does for the marks, and leaves the develop settings,
/// `arraw:version` and everything foreign as the file has them. Without a
/// sidecar a new one is created, with default develop settings. This is how a
/// session writes a rating without saving the edits it has not saved (ADR 030).
/// @param photo Path of the photograph.
/// @param marks Marks to write.
/// @throws std::invalid_argument if the rating of @p marks is outside -1 to 5.
/// @throws std::runtime_error as ::arraw::writeSidecar does: for an existing
/// sidecar that is not XMP or is of a newer version, which is left alone, or
/// that cannot be read, and for a file that cannot be written.
void writeSidecarMarks(const std::filesystem::path& photo, const PhotoMarks& marks);

} // namespace arraw
