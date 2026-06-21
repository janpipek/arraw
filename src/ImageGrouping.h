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

// Short label for a primary's companions, e.g. "JPG" for one or "JPG+1" for two.
// Empty when there are no companions. Used by the filmstrip cell badge.
QString companionBadgeText(const QStringList& companions);
