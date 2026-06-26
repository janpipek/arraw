#pragma once
#include <QString>
#include <QStringList>

// User-authored, writable metadata that travels with an image. Distinct from
// the read-only camera EXIF in ImageMetadata. Persisted in the develop sidecar
// as xmp:Rating / xmp:Label plus the descriptive Dublin Core fields owned by
// arraw (docs/adr/0033). Bounded named fields, not a free string map.

enum class ColourLabel { None, Red, Yellow, Green, Blue, Purple };

struct UserMetadata {
    int rating = 0; // 0 unrated, -1 reject, 1..5 stars
    ColourLabel label = ColourLabel::None;
    QString title;
    QString caption;
    QStringList keywords;
    QString creator;
    QString copyright;

    bool operator==(const UserMetadata&) const = default;
};

struct UserMetadataPresence {
    bool title = false;
    bool caption = false;
    bool keywords = false;
    bool creator = false;
    bool copyright = false;

    bool operator==(const UserMetadataPresence&) const = default;
};

// Canonical English colour names, matching Lightroom's default label set so the
// label survives a round-trip. None maps to an empty string (the attribute is
// absent), and any unrecognised string reads back as None.
inline QString colourLabelToString(ColourLabel label) {
    switch (label) {
    case ColourLabel::Red:
        return QStringLiteral("Red");
    case ColourLabel::Yellow:
        return QStringLiteral("Yellow");
    case ColourLabel::Green:
        return QStringLiteral("Green");
    case ColourLabel::Blue:
        return QStringLiteral("Blue");
    case ColourLabel::Purple:
        return QStringLiteral("Purple");
    case ColourLabel::None:
        break;
    }
    return {};
}

inline ColourLabel colourLabelFromString(const QString& s) {
    if (s == QLatin1String("Red"))
        return ColourLabel::Red;
    if (s == QLatin1String("Yellow"))
        return ColourLabel::Yellow;
    if (s == QLatin1String("Green"))
        return ColourLabel::Green;
    if (s == QLatin1String("Blue"))
        return ColourLabel::Blue;
    if (s == QLatin1String("Purple"))
        return ColourLabel::Purple;
    return ColourLabel::None;
}
