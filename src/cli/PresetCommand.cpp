#include "cli/PresetCommand.h"
#include "cli/ImagePathPreflight.h"
#include "develop/DevelopGroup.h"
#include "io/XmpSidecar.h"
#include <algorithm>
#include <optional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cli {

namespace {

// Case-insensitive lookup, matching PresetStore::exists' notion of "the same
// name" (docs/adr/0051). At most one preset can match: the store's own
// save() keeps names unique post-sanitisation.
std::optional<DevelopPreset> findPreset(const PresetStore& store, const QString& name) {
    for (const DevelopPreset& p : store.loadAll())
        if (p.name.compare(name, Qt::CaseInsensitive) == 0)
            return p;
    return std::nullopt;
}

// Active groups' labels, in declaration order, joined for the table column.
QStringList groupLabels(const DevelopPreset& preset) {
    QStringList labels;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (hasGroup(preset.groups, g))
            labels << developGroupLabel(g);
    }
    return labels;
}

// Active groups' stable keys, for the JSON array (docs/adr/0050: JSON keys
// are the same machine identifiers the preset files use, never localised).
QJsonArray groupKeys(const DevelopPreset& preset) {
    QJsonArray keys;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (hasGroup(preset.groups, g))
            keys.append(developGroupKey(g));
    }
    return keys;
}

int listAsTable(const std::vector<DevelopPreset>& presets, QTextStream& out) {
    if (presets.empty()) {
        out << "No presets saved.\n";
        return 0;
    }

    qsizetype nameWidth = QStringLiteral("NAME").size();
    for (const DevelopPreset& p : presets)
        nameWidth = std::max(nameWidth, p.name.size());

    out << QStringLiteral("NAME").leftJustified(nameWidth + 2) << "GROUPS\n";
    for (const DevelopPreset& p : presets)
        out << p.name.leftJustified(nameWidth + 2) << groupLabels(p).join(", ") << "\n";
    return 0;
}

int listAsJson(const std::vector<DevelopPreset>& presets, QTextStream& out) {
    QJsonArray root;
    for (const DevelopPreset& p : presets) {
        QJsonObject o;
        o["name"] = p.name;
        o["groups"] = groupKeys(p);
        root.append(o);
    }
    out << QJsonDocument(root).toJson(QJsonDocument::Compact) << "\n";
    return 0;
}

// arraw preset: no preset named 'X' / (no presets saved), then the available
// names, all on stderr (docs/adr/0050's usage-error tier: nothing to do).
int noSuchPreset(const PresetStore& store, const QString& name, QTextStream& err) {
    const std::vector<DevelopPreset> presets = store.loadAll();
    if (presets.empty()) {
        err << "arraw preset: no presets saved\n";
        return 2;
    }
    QStringList names;
    for (const DevelopPreset& p : presets)
        names << p.name;
    err << "arraw preset: no preset named '" << name << "'\n";
    err << "available presets: " << names.join(", ") << "\n";
    return 2;
}

// Mirrors MainWindow::showPresetDetails, headless: each active group's label,
// then its changed fields (or "(resets to defaults)" if none differ).
int showAsTable(const DevelopPreset& preset, QTextStream& out) {
    out << preset.name << "\n";
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (!hasGroup(preset.groups, g))
            continue;
        out << "  " << developGroupLabel(g) << "\n";
        const QStringList lines = describeGroupNonDefaults(g, preset.values);
        if (lines.isEmpty())
            out << "    (resets to defaults)\n";
        else
            for (const QString& line : lines)
                out << "    " << line << "\n";
    }
    return 0;
}

// The preset's own file format, byte-for-byte (docs/adr/0051): the on-disk
// JSON already is the stable machine representation, so show --json exposes
// it directly instead of inventing a second schema.
int showAsJson(const DevelopPreset& preset, QTextStream& out) {
    out << serializeDevelopPreset(preset) << "\n";
    return 0;
}

struct ApplyFailure {
    QString path;
    QString error;
};

struct ApplyOutcome {
    QStringList applied;
    std::vector<ApplyFailure> failed;
};

// The GUI's batch apply, byte-for-byte (MainWindow::applyPresetToPaths /
// BatchPaste's writeBatchAfter, docs/adr/0018/0051): load each file's current
// adjustments (defaults if it has no sidecar yet), overwrite the preset's
// selected groups, and save. saveAdjustments is namespace-scoped, so ratings,
// colour labels, and snapshots already on disk ride through untouched.
ApplyOutcome applyPresetToPaths(const DevelopPreset& preset, const QStringList& paths) {
    ApplyOutcome outcome;
    for (const QString& path : paths) {
        const GlobalAdjustment before = XmpSidecar::loadAdjustments(path);
        const GlobalAdjustment after = applyGroups(before, preset.values, preset.groups);
        if (XmpSidecar::saveAdjustments(path, after))
            outcome.applied << path;
        else
            outcome.failed.push_back({path, QStringLiteral("cannot write sidecar")});
    }
    return outcome;
}

int applyAsTable(
    const QString& presetName, const ApplyOutcome& outcome, QTextStream& out, QTextStream& err) {
    out << "Applying \"" << presetName << "\" to "
        << (outcome.applied.size() + outcome.failed.size()) << " files...\n";
    for (const QString& path : outcome.applied) {
        out << "Applied: " << path << "\n";
        out.flush(); // progress must appear per file, not at exit (matches export)
    }
    for (const ApplyFailure& f : outcome.failed)
        err << f.path << ": " << f.error << "\n";
    out << outcome.applied.size() << " applied, " << outcome.failed.size() << " failed\n";
    return outcome.failed.empty() ? 0 : 1;
}

int applyAsJson(
    const QString& presetName, const ApplyOutcome& outcome, QTextStream& out, QTextStream& err) {
    QJsonObject root;
    root["preset"] = presetName;
    root["applied"] = QJsonArray::fromStringList(outcome.applied);
    QJsonArray failed;
    for (const ApplyFailure& f : outcome.failed) {
        err << f.path << ": " << f.error << "\n";
        QJsonObject o;
        o["path"] = f.path;
        o["error"] = f.error;
        failed.append(o);
    }
    root["failed"] = failed;
    out << QJsonDocument(root).toJson(QJsonDocument::Compact) << "\n";
    return outcome.failed.empty() ? 0 : 1;
}

} // namespace

int runPresetList(const PresetStore& store, bool json, QTextStream& out) {
    const std::vector<DevelopPreset> presets = store.loadAll();
    return json ? listAsJson(presets, out) : listAsTable(presets, out);
}

int runPresetShow(
    const PresetStore& store, const QString& name, bool json, QTextStream& out, QTextStream& err) {
    const std::optional<DevelopPreset> preset = findPreset(store, name);
    if (!preset)
        return noSuchPreset(store, name, err);
    return json ? showAsJson(*preset, out) : showAsTable(*preset, out);
}

int runPresetApply(
    const PresetStore& store,
    const QString& name,
    const QStringList& paths,
    bool json,
    QTextStream& out,
    QTextStream& err) {
    const std::optional<DevelopPreset> preset = findPreset(store, name);
    if (!preset)
        return noSuchPreset(store, name, err);

    const QString preflightError = preflightImagePaths(paths);
    if (!preflightError.isEmpty()) {
        err << "arraw preset apply: " << preflightError << "\n";
        return 2;
    }

    const ApplyOutcome outcome = applyPresetToPaths(*preset, paths);
    return json ? applyAsJson(preset->name, outcome, out, err)
                : applyAsTable(preset->name, outcome, out, err);
}

int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err) {
    const PresetStore store = defaultPresetStore();
    switch (inv.verb) {
    case PresetVerb::List:
        return runPresetList(store, inv.json, out);
    case PresetVerb::Show:
        return runPresetShow(store, inv.name, inv.json, out, err);
    case PresetVerb::Apply:
        return runPresetApply(store, inv.name, inv.paths, inv.json, out, err);
    }
    return 2; // unreachable: every PresetVerb is handled above
}

} // namespace cli
