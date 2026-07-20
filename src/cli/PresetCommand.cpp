#include "cli/PresetCommand.h"
#include "develop/DevelopGroup.h"
#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cli {

namespace {

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

} // namespace

int runPresetList(const PresetStore& store, bool json, QTextStream& out) {
    const std::vector<DevelopPreset> presets = store.loadAll();
    return json ? listAsJson(presets, out) : listAsTable(presets, out);
}

int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err) {
    const PresetStore store = defaultPresetStore();
    switch (inv.verb) {
    case PresetVerb::List:
        return runPresetList(store, inv.json, out);
    case PresetVerb::Show:
    case PresetVerb::Apply:
        err << "arraw preset: not yet implemented\n";
        return 2;
    }
    return 2; // unreachable: every PresetVerb is handled above
}

} // namespace cli
