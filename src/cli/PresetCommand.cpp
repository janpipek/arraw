#include "cli/PresetCommand.h"
#include "develop/DevelopGroup.h"
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

int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err) {
    const PresetStore store = defaultPresetStore();
    switch (inv.verb) {
    case PresetVerb::List:
        return runPresetList(store, inv.json, out);
    case PresetVerb::Show:
        return runPresetShow(store, inv.name, inv.json, out, err);
    case PresetVerb::Apply:
        err << "arraw preset: not yet implemented\n";
        return 2;
    }
    return 2; // unreachable: every PresetVerb is handled above
}

} // namespace cli
