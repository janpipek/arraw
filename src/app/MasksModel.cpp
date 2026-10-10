#include "MasksModel.h"

#include "MaskPresentation.h"

#include <variant>

namespace arraw::app {

namespace {

bool sameIds(const DevelopState& a, const DevelopState& b) {
    if (a.localAdjustments.size() != b.localAdjustments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.localAdjustments.size(); ++index) {
        if (a.localAdjustments[index].id != b.localAdjustments[index].id) {
            return false;
        }
    }
    return true;
}

} // namespace

MasksModel::MasksModel(QObject* parent) : QAbstractListModel(parent) {}

void MasksModel::setState(const DevelopState& state) {
    if (sameIds(state_, state)) {
        const bool changed = state_.localAdjustments != state.localAdjustments;
        state_ = state;
        if (changed && !state_.localAdjustments.empty()) {
            emit dataChanged(index(0), index(rowCount() - 1));
        }
        return;
    }
    beginResetModel();
    state_ = state;
    endResetModel();
}

std::optional<LocalAdjustmentId> MasksModel::idAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return state_.localAdjustments[static_cast<std::size_t>(row)].id;
}

int MasksModel::rowOf(LocalAdjustmentId id) const {
    for (std::size_t row = 0; row < state_.localAdjustments.size(); ++row) {
        if (state_.localAdjustments[row].id == id) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

int MasksModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(state_.localAdjustments.size());
}

QVariant MasksModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const LocalAdjustment& adjustment =
        state_.localAdjustments[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        return maskDisplayName(state_, adjustment.id);
    case Qt::EditRole:
        return QString::fromStdString(adjustment.name);
    case Qt::CheckStateRole:
        return adjustment.enabled ? Qt::Checked : Qt::Unchecked;
    case IdRole:
        return adjustment.id.value;
    case KindRole:
        return static_cast<int>(adjustment.shape.index()) + 1;
    case InvertedRole:
        return adjustment.invert;
    default:
        return {};
    }
}

bool MasksModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const std::optional<LocalAdjustmentId> id = idAt(index.isValid() ? index.row() : -1);
    if (!id) {
        return false;
    }
    if (role == Qt::EditRole) {
        emit renameRequested(*id, value.toString());
        return true;
    }
    if (role == Qt::CheckStateRole) {
        emit enabledRequested(*id, value.toInt() == Qt::Checked);
        return true;
    }
    return false;
}

Qt::ItemFlags MasksModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable;
}

} // namespace arraw::app
