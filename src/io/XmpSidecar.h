#pragma once
#include "core/Orientation.h"
#include "develop/GlobalAdjustment.h"
#include "develop/Snapshot.h"
#include "develop/UserMetadata.h"
#include <QByteArray>
#include <QString>
#include <vector>

// Reads and writes the XMP sidecar file: develop settings (crs: namespace) and
// user-authored culling marks (xmp:Rating / xmp:Label). See docs/adr/0007.
// Sidecar path: same directory + base name as the RAW file, .xmp extension.
// Note: Temperature is stored in absolute Kelvin (2000-12000, matching crs:);
// Tint and all other develop fields use the internal -100..100 scale.

// Whole-file in-memory view of one sidecar.
struct SidecarData {
    GlobalAdjustment adjustments;
    UserMetadata metadata;
    UserMetadataPresence metadataPresence;
    // Named A/B develop states, persisted in the arraw: namespace (docs/adr/0038).
    std::vector<Snapshot> snapshots;
    // True when the sidecar carried an explicit tiff:Orientation. When false, the
    // resolver seeds orientation from the file's EXIF instead (docs/adr/0029).
    bool orientationStored = false;
};

struct XmpPacketMetadata {
    UserMetadata metadata;
    UserMetadataPresence presence;
};

enum class SidecarLoadStatus {
    Missing,
    Loaded,
    ParseError,
};

struct SidecarLoadResult {
    SidecarData data;
    SidecarLoadStatus status = SidecarLoadStatus::Missing;
};

struct SidecarAdjustmentResult {
    GlobalAdjustment adjustments;
    SidecarLoadStatus status = SidecarLoadStatus::Missing;
};

class XmpSidecar {
public:
    static QString pathFor(const QString& rawPath);

    // Parses descriptive User Metadata from an XMP packet embedded in an image file.
    // Returns empty metadata for malformed or absent packets.
    static UserMetadata metadataFromPacket(const QByteArray& packet);
    static XmpPacketMetadata metadataPacketFromPacket(const QByteArray& packet);

    // Reads the whole sidecar. Returns defaults if it doesn't exist or can't be parsed.
    static SidecarData load(const QString& rawPath);
    static SidecarLoadResult loadWithStatus(const QString& rawPath);

    static GlobalAdjustment loadAdjustments(const QString& rawPath) {
        return load(rawPath).adjustments;
    }

    static UserMetadata loadMetadata(const QString& rawPath) { return load(rawPath).metadata; }

    // The adjustments to apply when opening rawPath: the sidecar's if one exists,
    // otherwise defaults with the crop set to defaultCrop (e.g. a DNG DefaultCrop).
    // seededOrientation is applied when the sidecar has no explicit
    // tiff:Orientation (e.g. read from the file's EXIF) — see docs/adr/0029.
    static SidecarLoadResult resolveForImage(
        const QString& rawPath,
        const QRectF& defaultCrop,
        orient::Orientation seededOrientation = {});
    static GlobalAdjustment resolveAdjustments(
        const QString& rawPath,
        const QRectF& defaultCrop,
        orient::Orientation seededOrientation = {});
    static SidecarAdjustmentResult resolveAdjustmentsWithStatus(
        const QString& rawPath,
        const QRectF& defaultCrop,
        orient::Orientation seededOrientation = {});

    // Namespace-scoped, read-first saves: each replaces only its own half of the
    // sidecar and preserves the other half already on disk (docs/adr/0007).
    static bool saveAdjustments(const QString& rawPath, const GlobalAdjustment& params);
    // Saves develop settings together with the photo's named Snapshots
    // (docs/adr/0038). The 2-arg form above preserves whatever snapshots are
    // already on disk; this form replaces them.
    static bool saveAdjustments(
        const QString& rawPath,
        const GlobalAdjustment& params,
        const std::vector<Snapshot>& snapshots);
    static bool saveMetadata(const QString& rawPath, const UserMetadata& metadata);
    static bool saveMetadata(
        const QString& rawPath, const UserMetadata& metadata, const UserMetadataPresence& descriptiveFields);
};
