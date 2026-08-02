#include "cli/InfoCommand.h"
#include "cli/ImagePathPreflight.h"
#include "core/ImageMetadata.h"
#include "develop/DevelopGroup.h"
#include "develop/DevelopPreset.h"
#include "io/XmpSidecar.h"
#include "pipeline/RawProcessor.h"
#include <libraw/libraw.h>
#include <memory>
#include <vector>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cli {

namespace {

// Everything `info` knows about one file, read once and rendered twice: the
// table and --json are two views of this, never two reads (docs/adr/0053).
struct FileReport {
    QString path;
    ExifData exif;
    ImageMetadata exifRows; // the display formatting the GUI's Info panel uses
    bool hasSidecar = false;
    UserMetadata metadata;
    GlobalAdjustment adjustments;
    GroupSelection nonDefaultGroups;
    QString error; // non-empty: this file alone failed, the batch continues
};

// The cheap LibRaw path the FilmStrip tooltips already use: open_file parses
// the header and EXIF without unpacking or demosaicing a single pixel
// (docs/adr/0053) — `info` never decodes and never touches the GPU.
bool readExif(const QString& path, FileReport& report) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.toLocal8Bit().constData()) != LIBRAW_SUCCESS)
        return false;
    report.exif = extractExifData(*raw);
    report.exifRows = extractMetadata(*raw);
    return true;
}

FileReport readFile(const QString& path) {
    FileReport report;
    report.path = path;

    // A RAW LibRaw can't open is that file's failure. A standard image simply
    // has no EXIF to read — StandardImageLoader extracts none today, and that
    // pre-existing gap is not this command's to close (docs/adr/0053).
    if (!readExif(path, report) && RawProcessor::canLoad(path))
        report.error = QStringLiteral("cannot read image");

    const SidecarLoadResult sidecar = XmpSidecar::loadWithStatus(path);
    report.hasSidecar = sidecar.status != SidecarLoadStatus::Missing;
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

// Only fields the user actually filled in get a line: an empty Title is
// absence, not a value, and a report padded with blanks buries the signal.
void writeUserMetadata(const UserMetadata& meta, QTextStream& out) {
    out << "  Rating: " << ratingText(meta.rating) << "\n";
    if (meta.label != ColourLabel::None)
        out << "  Colour label: " << colourLabelToString(meta.label) << "\n";
    if (!meta.title.isEmpty())
        out << "  Title: " << meta.title << "\n";
    if (!meta.caption.isEmpty())
        out << "  Caption: " << meta.caption << "\n";
    if (!meta.creator.isEmpty())
        out << "  Creator: " << meta.creator << "\n";
    if (!meta.copyright.isEmpty())
        out << "  Copyright: " << meta.copyright << "\n";
    if (!meta.keywords.isEmpty())
        out << "  Keywords: " << meta.keywords.join(", ") << "\n";
}

// The reason `info` exists rather than being redundant with any EXIF viewer:
// which develop groups this photo actually carries, and what they changed.
// Goes deeper than `preset list`'s names-only summary because a file's edit
// state has no `preset show` companion to defer the detail to (docs/adr/0053).
void writeDevelopGroups(const FileReport& report, QTextStream& out) {
    if (report.nonDefaultGroups.none()) {
        out << "  Develop: no edits\n";
        return;
    }

    out << "  Develop:\n";
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (!hasGroup(report.nonDefaultGroups, g))
            continue;
        out << "    " << developGroupLabel(g) << "\n";
        for (const QString& line : describeGroupNonDefaults(g, report.adjustments))
            out << "      " << line << "\n";
    }
}

// One detail block per file, not a summary row: a file carries ~15-20 EXIF
// rows plus sidecar fields plus per-group changed values — too much for
// row/column shape (docs/adr/0053).
void writeTable(const FileReport& report, QTextStream& out) {
    out << report.path << "\n";
    for (const auto& [label, value] : report.exifRows.rows)
        out << "  " << label << ": " << value << "\n";
    out << "  Sidecar: " << (report.hasSidecar ? "present" : "none") << "\n";
    writeUserMetadata(report.metadata, out);
    writeDevelopGroups(report, out);
}

QJsonObject toJson(const FileReport& report) {
    QJsonObject o;
    o["path"] = report.path;
    if (!report.error.isEmpty()) {
        o["error"] = report.error; // inline, and mirrored to stderr
        return o;
    }
    o["exif"] = toJson(report.exif);
    o["hasSidecar"] = report.hasSidecar;
    o["rating"] = report.metadata.rating;
    // Omitted rather than emitted as "" when unset, so absence reads the same
    // way for every optional field and no script has to special-case a third,
    // empty-string state.
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

int runInfo(const QStringList& paths, bool json, QTextStream& out, QTextStream& err) {
    const QString preflightError = preflightImagePaths(paths);
    if (!preflightError.isEmpty()) {
        err << "arraw info: " << preflightError << "\n";
        return 2;
    }

    std::vector<FileReport> reports;
    reports.reserve(size_t(paths.size()));
    bool anyFailed = false;
    for (const QString& path : paths) {
        reports.push_back(readFile(path));
        if (!reports.back().error.isEmpty()) {
            err << path << ": " << reports.back().error << "\n";
            anyFailed = true;
        }
    }

    if (json) {
        QJsonArray root;
        for (const FileReport& report : reports)
            root.append(toJson(report));
        out << QJsonDocument(root).toJson(QJsonDocument::Compact) << "\n";
    } else {
        for (const FileReport& report : reports)
            if (report.error.isEmpty())
                writeTable(report, out);
    }
    return anyFailed ? 1 : 0;
}

} // namespace cli
