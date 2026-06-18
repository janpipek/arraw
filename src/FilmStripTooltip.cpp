#include "FilmStripTooltip.h"

#include <QStringList>

namespace {

QString rowValue(const ImageMetadata& metadata, const QString& label) {
    for (const auto& [rowLabel, value] : metadata.rows) {
        if (rowLabel == label)
            return value;
    }
    return {};
}

void appendIfPresent(QStringList& lines, const QString& value) {
    if (!value.isEmpty())
        lines << value;
}

} // namespace

QString tooltipText(const QString& filename, const ImageMetadata& metadata) {
    QStringList lines;
    lines << filename;

    appendIfPresent(lines, rowValue(metadata, "Date"));

    const QString make = rowValue(metadata, "Make");
    const QString model = rowValue(metadata, "Model");
    appendIfPresent(lines, QStringList{make, model}.join(' ').trimmed());

    appendIfPresent(lines, rowValue(metadata, "Lens"));

    QStringList exposure;
    const QString iso = rowValue(metadata, "ISO");
    if (!iso.isEmpty())
        exposure << "ISO " + iso;
    appendIfPresent(exposure, rowValue(metadata, "Shutter"));
    appendIfPresent(exposure, rowValue(metadata, "Aperture"));
    appendIfPresent(exposure, rowValue(metadata, "Focal length"));
    appendIfPresent(lines, exposure.join("  "));

    QString dimensions = rowValue(metadata, "Active area");
    if (dimensions.isEmpty())
        dimensions = rowValue(metadata, "RAW size");
    appendIfPresent(lines, dimensions);

    return lines.join('\n');
}
