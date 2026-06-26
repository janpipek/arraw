#include "HistoryPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUndoStack>
#include <QVBoxLayout>

HistoryPanel::HistoryPanel(QUndoStack* undoStack, QWidget* parent)
    : QWidget(parent), undoStack(undoStack) {
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
    // A reversed view of the undo stack: most recent edit on top, the base "Load"
    // state pinned at the bottom. (QUndoView can't reverse, hence the hand-rolled
    // list.) Selecting a row rolls the stack to that point via setIndex().
    historyList = new QListWidget(this);
    historyList->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(historyList, /*stretch=*/1);

    connect(historyList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (historyRefreshing || row < 0)
            return;
        // Row 0 is the newest edit (stack at count); the last row is "Load"
        // (stack at 0). See rebuildHistory() for the mapping.
        this->undoStack->setIndex(this->undoStack->count() - row);
    });
    connect(undoStack, &QUndoStack::indexChanged, this, [this] { rebuildHistory(); });
    connect(undoStack, &QUndoStack::cleanChanged, this, [this] { rebuildHistory(); });
    rebuildHistory();

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

void HistoryPanel::rebuildHistory() {
    historyRefreshing = true;
    historyList->clear();
    const int count = undoStack->count();
    // Newest first: command (count-1) down to command 0, then the base state.
    for (int i = count - 1; i >= 0; --i)
        new QListWidgetItem(undoStack->text(i), historyList);
    new QListWidgetItem(tr("Load"), historyList);
    // Stack index 0..count maps to row count-index, so the top row is the fully
    // redone state and the bottom row ("Load") is the as-loaded state.
    historyList->setCurrentRow(count - undoStack->index());
    historyRefreshing = false;
}

int HistoryPanel::selectedRow() const {
    return snapshotList->currentRow() >= 0 && snapshotList->currentItem()
               && snapshotList->currentItem()->isSelected()
        ? snapshotList->currentRow()
        : -1;
}
