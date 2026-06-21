#pragma once
#include <QList>
#include <QString>
#include <QStringList>

/**
 * Collapses a RAW+JPEG capture into one logical frame.
 *
 * Cameras shooting RAW+JPEG write two files that share a base name (e.g.
 * IMG_001.CR2 and IMG_001.JPG). For culling these are one shot, not two.
 * groupImageFiles() turns a flat directory listing into per-shot groups: a
 * RAW becomes the primary and owns any same-stem standard images as
 * companions. Files that do not form a RAW+standard pair stand alone.
 */
struct ImageGroup {
    QString primary;        // the file shown and edited (RAW when paired)
    QStringList companions; // same-stem standard images attached to the primary
};

// Groups a flat list of image paths by capture. See ImageGroup. Pure: depends
// only on the path strings (no filesystem access).
QList<ImageGroup> groupImageFiles(const QStringList& paths);

// The filmstrip Format Label for a shot: the canonical format name of the
// primary, then each companion's, joined by '+' with each format listed once —
// e.g. "ARW", "JPEG", or "ARW+JPEG". jpg/jpeg map to JPEG and tif/tiff to TIFF;
// other suffixes are uppercased. Describes the shot's own formats.
QString formatLabelText(const ImageGroup& group);
