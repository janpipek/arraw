#include "HistoryPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUndoView>
#include <QVBoxLayout>

HistoryPanel::HistoryPanel(QUndoStack* undoStack, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    layout->addWidget(new QLabel(tr("Snapshots"), this));

    snapshotList = new QListWidget(this);
    snapshotList->setSelectionMode(QAbstractItemView::SingleSelection);
    // Double-click restores; F2 renames. Keep double-click off the edit triggers
    // so the two gestures don't collide.
    snapshotList->setEditTriggers(QAbstractItemView::EditKeyPressed);
    layout->addWidget(snapshotList);

    auto* buttons = new QHBoxLayout;
    addButton = new QPushButton(tr("Add"), this);
    addButton->setToolTip(tr("Save the current develop state as a snapshot"));
    restoreButton = new QPushButton(tr("Restore"), this);
    deleteButton = new QPushButton(tr("Delete"), this);
    buttons->addWidget(addButton);
    buttons->addWidget(restoreButton);
    buttons->addWidget(deleteButton);
    layout->addLayout(buttons);

    layout->addWidget(new QLabel(tr("History"), this));
    auto* historyView = new QUndoView(undoStack, this);
    layout->addWidget(historyView, /*stretch=*/1);

    auto syncButtons = [this] {
        const bool hasSelection = selectedRow() >= 0;
        restoreButton->setEnabled(hasSelection);
        deleteButton->setEnabled(hasSelection);
    };
    syncButtons();

    connect(addButton, &QPushButton::clicked, this, &HistoryPanel::addRequested);
    connect(restoreButton, &QPushButton::clicked, this, [this] {
        if (selectedRow() >= 0)
            emit restoreRequested(selectedRow());
    });
    connect(deleteButton, &QPushButton::clicked, this, [this] {
        if (selectedRow() >= 0)
            emit deleteRequested(selectedRow());
    });
    connect(snapshotList, &QListWidget::itemSelectionChanged, this, syncButtons);
    connect(snapshotList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit restoreRequested(snapshotList->row(item));
    });
    connect(snapshotList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (refreshing)
            return;
        emit renameRequested(snapshotList->row(item), item->text());
    });
}

void HistoryPanel::setSnapshots(const std::vector<Snapshot>& snapshots) {
    const int previous = selectedRow();
    refreshing = true;
    snapshotList->clear();
    for (const auto& snap : snapshots) {
        auto* item = new QListWidgetItem(snap.name, snapshotList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if (previous >= 0 && previous < snapshotList->count())
        snapshotList->setCurrentRow(previous);
    refreshing = false;
    restoreButton->setEnabled(selectedRow() >= 0);
    deleteButton->setEnabled(selectedRow() >= 0);
}

int HistoryPanel::selectedRow() const {
    return snapshotList->currentRow() >= 0 && snapshotList->currentItem()
               && snapshotList->currentItem()->isSelected()
        ? snapshotList->currentRow()
        : -1;
}
