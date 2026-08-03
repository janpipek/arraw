#include "cli/InfoCommand.h"
#include "cli/ImagePathPreflight.h"
#include "core/ImageMetadata.h"
#include "develop/DevelopGroup.h"
#include "develop/DevelopPreset.h"
#include "io/XmpSidecar.h"
#include "pipeline/RawProcessor.h"
#include <libraw/libraw.h>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cli {

namespace {

// Everything `info` knows about one file. The EXIF half and the sidecar half
// are read independently, so one failing never blanks the other: a RAW LibRaw
// chokes on still reports the rating and develop groups its sidecar carries.
struct FileReport {
    QString path;
    ExifData exif;          // --json's typed camera facts
    ImageMetadata exifRows; // the display formatting the GUI's Info panel uses
    SidecarLoadStatus sidecar = SidecarLoadStatus::Missing;
    UserMetadata metadata;
    GlobalAdjustment adjustments;
    GroupSelection nonDefaultGroups;
    QString error; // non-empty: this file's EXIF is unreadable, batch continues

    bool hasSidecar() const { return sidecar != SidecarLoadStatus::Missing; }

    // A sidecar that exists but won't parse means the edit state below is
    // defaults standing in for something real — never silently reported as
    // "no edits" (the GUI says "Sidecar unreadable; defaults applied").
    bool sidecarUnreadable() const { return sidecar == SidecarLoadStatus::ParseError; }
};

// The cheap LibRaw path the FilmStrip tooltips already use: open_file parses
// the header and EXIF without unpacking or demosaicing a single pixel
// (docs/adr/0053) — `info` never decodes and never touches the GPU. Only the
// half the chosen output actually renders is extracted: the display rows for
// the table, the typed fields for --json, never both.
bool readExif(const QString& path, bool json, FileReport& report) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return false;
    if (json)
        report.exif = extractExifData(*raw);
    else
        report.exifRows = extractMetadata(*raw);
    return true;
}

FileReport readFile(const QString& path, bool json) {
    FileReport report;
    report.path = path;

    // A RAW LibRaw can't open is that file's failure. A standard image simply
    // has no EXIF to read — StandardImageLoader extracts none today, and that
    // pre-existing gap is not this command's to close (docs/adr/0053).
    if (!readExif(path, json, report) && RawProcessor::canLoad(path))
        report.error = QStringLiteral("cannot read image");

    const SidecarLoadResult sidecar = XmpSidecar::loadWithStatus(path);
    report.sidecar = sidecar.status;
    report.metadata = sidecar.data.metadata;
    report.adjustments = sidecar.data.adjustments;
    report.nonDefaultGroups = groupsWithNonDefaultValues(report.adjustments);
    return report;
}

// Rating's two sentinel values read as words; 1..5 read as the star count
// (CONTEXT.md: 0 unrated, -1 reject, no separate pick flag).
QString ratingText(int rating) {
    if (rating < 0)
        return QStringLiteral("reject");
    if (rating == 0)
        return QStringLiteral("unrated");
    return QString::number(rating);
}

QString ratingPainted(int rating, const TextStyle& style) {
    const QString text = ratingText(rating);
    if (rating < 0)
        return style.paint(text, Ink::Red);
    if (rating == 0)
        return style.dim(text);
    return style.paint(text, Ink::Yellow);
}

// A colour label printed in its own colour: the one place where the seasoning
// carries the value rather than decorating it.
Ink inkFor(ColourLabel label) {
    switch (label) {
    case ColourLabel::Red:
        return Ink::Red;
    case ColourLabel::Yellow:
        return Ink::Yellow;
    case ColourLabel::Green:
        return Ink::Green;
    case ColourLabel::Blue:
        return Ink::Blue;
    case ColourLabel::Purple:
        return Ink::Magenta;
    case ColourLabel::None:
        break;
    }
    return Ink::Dim;
}

// Labels are scaffolding, values are the report: the label goes dim so the
// eye lands on what the file actually says.
void writeRow(QTextStream& out, const TextStyle& style, const QString& label, const QString& value) {
    out << "  " << style.dim(label + ":") << " " << value << "\n";
}

QString sidecarPainted(const FileReport& report, const TextStyle& style) {
    switch (report.sidecar) {
    case SidecarLoadStatus::Loaded:
        return style.paint(QStringLiteral("present"), Ink::Green);
    case SidecarLoadStatus::ParseError:
        return style.paint(QStringLiteral("unreadable"), Ink::Yellow);
    case SidecarLoadStatus::Missing:
        break;
    }
    return style.dim(QStringLiteral("none"));
}

// Only fields the user actually filled in get a line: an empty Title is
// absence, not a value, and a report padded with blanks buries the signal.
void writeUserMetadata(const UserMetadata& meta, QTextStream& out, const TextStyle& style) {
    writeRow(out, style, QStringLiteral("Rating"), ratingPainted(meta.rating, style));
    if (meta.label != ColourLabel::None) {
        writeRow(
            out,
            style,
            QStringLiteral("Colour label"),
            style.paint(colourLabelToString(meta.label), inkFor(meta.label)));
    }
    if (!meta.title.isEmpty())
        writeRow(out, style, QStringLiteral("Title"), meta.title);
    if (!meta.caption.isEmpty())
        writeRow(out, style, QStringLiteral("Caption"), meta.caption);
    if (!meta.creator.isEmpty())
        writeRow(out, style, QStringLiteral("Creator"), meta.creator);
    if (!meta.copyright.isEmpty())
        writeRow(out, style, QStringLiteral("Copyright"), meta.copyright);
    if (!meta.keywords.isEmpty())
        writeRow(out, style, QStringLiteral("Keywords"), meta.keywords.join(", "));
}

// The reason `info` exists rather than being redundant with any EXIF viewer:
// which develop groups this photo actually carries, and what they changed.
// Goes deeper than `preset list`'s names-only summary because a file's edit
// state has no `preset show` companion to defer the detail to (docs/adr/0053).
void writeDevelopGroups(const FileReport& report, QTextStream& out, const TextStyle& style) {
    if (report.nonDefaultGroups.none()) {
        writeRow(out, style, QStringLiteral("Develop"), style.dim(QStringLiteral("no edits")));
        return;
    }

    out << "  " << style.dim(QStringLiteral("Develop:")) << "\n";
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (!hasGroup(report.nonDefaultGroups, g))
            continue;
        out << "    " << style.paint(developGroupLabel(g), Ink::Cyan) << "\n";
        for (const QString& line : describeGroupNonDefaults(g, report.adjustments))
            out << "      " << line << "\n";
    }
}

// One detail block per file, not a summary row: a file carries ~15-20 EXIF
// rows plus sidecar fields plus per-group changed values — too much for
// row/column shape (docs/adr/0053).
void writeTable(const FileReport& report, QTextStream& out, const TextStyle& style) {
    out << style.bold(report.path) << "\n";
    // A file whose EXIF failed still gets its block: the error is a line in
    // the report, not a reason to leave the path off stdout entirely.
    if (!report.error.isEmpty())
        writeRow(out, style, QStringLiteral("Error"), style.paint(report.error, Ink::Red));
    for (const auto& [label, value] : report.exifRows.rows)
        writeRow(out, style, label, value);
    writeRow(out, style, QStringLiteral("Sidecar"), sidecarPainted(report, style));
    writeUserMetadata(report.metadata, out, style);
    writeDevelopGroups(report, out, style);
}

QJsonObject toJson(const FileReport& report) {
    QJsonObject o;
    o["path"] = report.path;
    // Inline, and mirrored to stderr. Only `exif` is missing when it is set —
    // the sidecar half was read independently and still reports.
    if (!report.error.isEmpty())
        o["error"] = report.error;
    else
        o["exif"] = toJson(report.exif);

    o["hasSidecar"] = report.hasSidecar();
    // Omitted rather than emitted as false/"" when it doesn't apply, so
    // absence reads the same way for every optional field and no script has to
    // special-case a third, empty state.
    if (report.sidecarUnreadable())
        o["sidecarUnreadable"] = true;
    o["rating"] = report.metadata.rating;
    if (report.metadata.label != ColourLabel::None)
        o["colourLabel"] = colourLabelToString(report.metadata.label);
    if (!report.metadata.title.isEmpty())
        o["title"] = report.metadata.title;
    if (!report.metadata.caption.isEmpty())
        o["caption"] = report.metadata.caption;
    if (!report.metadata.creator.isEmpty())
        o["creator"] = report.metadata.creator;
    if (!report.metadata.copyright.isEmpty())
        o["copyright"] = report.metadata.copyright;
    if (!report.metadata.keywords.isEmpty())
        o["keywords"] = QJsonArray::fromStringList(report.metadata.keywords);

    QJsonObject groups;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (hasGroup(report.nonDefaultGroups, g))
            groups[developGroupKey(g)] = groupToJson(g, report.adjustments);
    }
    o["developGroups"] = groups;
    return o;
}

} // namespace

int runInfo(
    const QStringList& paths, bool json, QTextStream& out, QTextStream& err, const TextStyle& style) {
    const QString preflightError = preflightImagePaths(paths);
    if (!preflightError.isEmpty()) {
        err << "arraw info: " << preflightError << "\n";
        return 2;
    }

    // The table streams a block per file as it is read — a folder's worth of
    // files shouldn't sit silent until the last one opens. --json is the one
    // shape that must buffer: it emits a single document (docs/adr/0050).
    QJsonArray root;
    bool anyFailed = false;
    bool firstBlock = true;
    for (const QString& path : paths) {
        const FileReport report = readFile(path, json);
        if (!report.error.isEmpty()) {
            err << path << ": " << report.error << "\n";
            anyFailed = true;
        }
        if (report.sidecarUnreadable()) {
            err << path << ": sidecar unreadable; reporting develop defaults\n";
            anyFailed = true;
        }

        if (json) {
            root.append(toJson(report));
            continue;
        }
        if (!firstBlock)
            out << "\n"; // one blank line between blocks, none before the first
        firstBlock = false;
        writeTable(report, out, style);
    }

    if (json)
        out << QJsonDocument(root).toJson(QJsonDocument::Compact) << "\n";
    return anyFailed ? 1 : 0;
}

} // namespace cli
