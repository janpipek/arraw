#include "HistoryModel.h"

#include "MaskPresentation.h"
#include "SettingPresentation.h"

#include <LocalAdjustmentEdits.h>

#include <QCoreApplication>
#include <QVariant>

namespace arraw::app {

namespace {

/// Words an ordinary edit of exactly one setting.
QString wordSingle(std::string_view key, const DevelopState& after) {
    const FieldDescriptor& descriptor = *findDescriptor(key);
    SettingPresentation const* presentation = nullptr;
    try {
        presentation = &presentationOf(key);
    } catch (const std::out_of_range&) {
        return groupDisplayName(descriptor.group);
    }
    const std::optional<double> value = displayedValue(descriptor, after);
    if (!descriptor.range || !value) {
        return presentation->label;
    }
    QString number = QString::number(*value, 'f', presentation->decimals);
    if (descriptor.range->minimum < 0.0 && *value > 0.0) {
        number.prepend('+');
    }
    return QStringLiteral("%1 %2%3").arg(presentation->label, number, presentation->unit);
}

/// Words one changed delta of a mask: the control's name and the value it now has.
QString wordDelta(const LocalDescriptor& descriptor, float value) {
    const SettingPresentation& presentation = localPresentationOf(descriptor.key);
    QString number = QString::number(value, 'f', presentation.decimals);
    if (value > 0.0F) {
        number.prepend('+');
    }
    return QStringLiteral("%1 %2%3").arg(presentation.label, number, presentation.unit);
}

/// Words an ordinary edit that changed masks and no setting.
QString wordLocal(const DevelopState& before, const DevelopState& after, const LocalChange& local) {
    const auto tr = [](const char* text) {
        return QCoreApplication::translate("arraw::app::HistoryModel", text);
    };
    const std::size_t total = local.added.size() + local.removed.size() + local.changed.size() +
                              (local.reordered ? 1 : 0);
    if (total > 1) {
        return QCoreApplication::translate("arraw::app::HistoryModel", "%n mask changes", nullptr,
                                           static_cast<int>(total));
    }
    if (!local.added.empty()) {
        return tr("Add %1").arg(maskDisplayName(after, local.added.front()));
    }
    if (!local.removed.empty()) {
        return tr("Remove %1").arg(maskDisplayName(before, local.removed.front()));
    }
    if (local.reordered) {
        return tr("Reorder Masks");
    }
    const MaskChange& change = local.changed.front();
    const QString label = maskDisplayName(after, change.id);
    const LocalAdjustment& now = *findLocalAdjustment(after, change.id);
    const int aspects = (change.name ? 1 : 0) + (change.enabled ? 1 : 0) +
                        (change.opacity ? 1 : 0) + (change.invert ? 1 : 0) +
                        (change.shape ? 1 : 0) + (change.strokes ? 1 : 0) +
                        (change.deltas.empty() ? 0 : 1);
    if (aspects == 1) {
        if (change.deltas.size() == 1) {
            const LocalDescriptor& descriptor = *findLocalDescriptor(change.deltas.front());
            return QStringLiteral("%1: %2").arg(
                label, wordDelta(descriptor, now.deltas.*descriptor.member));
        }
        if (!change.deltas.empty()) {
            return QStringLiteral("%1: %2").arg(
                label,
                QCoreApplication::translate("arraw::app::HistoryModel", "%n settings", nullptr,
                                            static_cast<int>(change.deltas.size())));
        }
        if (change.name) {
            return tr("Rename %1").arg(label);
        }
        if (change.enabled) {
            return (now.enabled ? tr("Enable %1") : tr("Disable %1")).arg(label);
        }
        if (change.invert) {
            return tr("Invert %1").arg(label);
        }
        if (change.opacity) {
            return QStringLiteral("%1: %2 %3%")
                .arg(label, tr("Opacity"), QString::number(qRound(now.opacity * 100.0F)));
        }
        if (change.strokes) {
            return tr("Paint %1").arg(label);
        }
        return tr("Move %1").arg(label);
    }
    return tr("Edit %1").arg(label);
}

} // namespace

QString groupDisplayName(SettingGroup group) {
    switch (group) {
    case SettingGroup::Color:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Colour");
    case SettingGroup::Tone:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Tone");
    case SettingGroup::Geometry:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Crop");
    case SettingGroup::Hsl:
        return QCoreApplication::translate("arraw::app::HistoryModel", "HSL / Colour Mix");
    case SettingGroup::BlackAndWhite:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Black & White");
    case SettingGroup::ToneCurve:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Tone Curve");
    case SettingGroup::ColorGrading:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Colour Grading");
    case SettingGroup::Effects:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Effects");
    case SettingGroup::Detail:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Noise Reduction");
    case SettingGroup::Presence:
        return QCoreApplication::translate("arraw::app::HistoryModel", "Presence");
    }
    return {};
}

HistoryModel::HistoryModel(QObject* parent) : QAbstractListModel(parent) {}

void HistoryModel::setHistory(const std::vector<HistoryStep>& history, std::size_t position,
                              const DevelopState& saved) {
    beginResetModel();
    history_ = history;
    position_ = position;
    saved_ = saved;
    endResetModel();
}

void HistoryModel::clear() {
    beginResetModel();
    history_.clear();
    position_ = 0;
    saved_.reset();
    endResetModel();
}

int HistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(history_.size());
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const std::size_t step = indexOfRow(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return textOf(step);
    case CurrentRole:
        return step == position_;
    case RedoableRole:
        return step > position_;
    case SavedRole:
        return saved_ && history_[step].state == *saved_;
    default:
        return {};
    }
}

int HistoryModel::rowOfIndex(std::size_t index) const noexcept {
    return static_cast<int>(history_.size() - 1 - index);
}

std::size_t HistoryModel::indexOfRow(int row) const noexcept {
    return history_.size() - 1 - static_cast<std::size_t>(row);
}

QString HistoryModel::textOf(std::size_t index) const {
    const HistoryStep& step = history_[index];
    const auto tr = [](const char* text) {
        return QCoreApplication::translate("arraw::app::HistoryModel", text);
    };
    switch (step.origin) {
    case EditOrigin::Opened:
        return tr("Opened");
    case EditOrigin::Paste:
        return tr("Paste Settings");
    case EditOrigin::Preset:
        return tr("Preset: %1").arg(QString::fromStdString(step.detail));
    case EditOrigin::Reset: {
        const ChangeDescription change =
            index == 0 ? ChangeDescription{}
                       : describeChange(history_[index - 1].state, step.state);
        if (change.group) {
            return QCoreApplication::translate("arraw::app::HistoryModel", "Reset %1")
                .arg(groupDisplayName(*change.group));
        }
        return tr("Reset");
    }
    case EditOrigin::Crop:
        return tr("Crop");
    case EditOrigin::Edit:
        break;
    }
    if (index == 0) {
        return tr("Edit");
    }
    const ChangeDescription change = describeChange(history_[index - 1].state, step.state);
    if (!change.local.empty()) {
        if (change.keys.empty()) {
            return wordLocal(history_[index - 1].state, step.state, change.local);
        }
        // Settings and masks at once: nothing to name but how many.
        const std::size_t masks = change.local.added.size() + change.local.removed.size() +
                                  change.local.changed.size() + (change.local.reordered ? 1 : 0);
        return QCoreApplication::translate("arraw::app::HistoryModel", "%n changes", nullptr,
                                           static_cast<int>(change.keys.size() + masks));
    }
    if (change.keys.size() == 1) {
        return wordSingle(change.keys.front(), step.state);
    }
    if (change.group) {
        return groupDisplayName(*change.group);
    }
    if (change.keys.empty()) {
        return tr("Edit");
    }
    // Several groups: the plural form needs the count.
    return QCoreApplication::translate("arraw::app::HistoryModel", "%n settings", nullptr,
                                       static_cast<int>(change.keys.size()));
}

} // namespace arraw::app
