#include "ui/SpotRemovalPanel.h"
#include "develop/Spot.h"

#include <algorithm>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

SpotRemovalPanel::SpotRemovalPanel(QWidget* parent)
    : QWidget(parent) {
    auto* col = new QVBoxLayout(this);

    spotList = new QListWidget(this);
    spotList->setMaximumHeight(100);
    col->addWidget(spotList);

    auto* buttons = new QHBoxLayout;
    deleteButton = new QPushButton("Delete", this);
    buttons->addStretch();
    buttons->addWidget(deleteButton);
    col->addLayout(buttons);
    col->addStretch();

    connect(deleteButton, &QPushButton::clicked, this, &SpotRemovalPanel::deleteActive);
    connect(spotList, &QListWidget::currentRowChanged, this, [this](int) {
        deleteButton->setEnabled(activeIndex() >= 0);
    });

    deleteButton->setEnabled(false);
}

void SpotRemovalPanel::setSpots(const std::vector<Spot>& spots) {
    spotEntries = spots;
    committedState = spots;
    rebuildList();
    emit changed(spotEntries);
}

void SpotRemovalPanel::addSpot(const Spot& spot) {
    auto before = spotEntries;
    spotEntries.push_back(spot);
    committedState = spotEntries;
    rebuildList();
    spotList->setCurrentRow(static_cast<int>(spotEntries.size()) - 1);
    emit changed(spotEntries);
    emit committed(std::move(before), spotEntries);
}

void SpotRemovalPanel::updateSpot(int idx, const Spot& updated) {
    if (idx < 0 || idx >= static_cast<int>(spotEntries.size()))
        return;
    spotEntries[idx] = updated;
    emit changed(spotEntries);
}

void SpotRemovalPanel::commitSpotEdit(int idx, const Spot& updated) {
    if (idx < 0 || idx >= static_cast<int>(spotEntries.size()))
        return;
    auto before = committedState;
    spotEntries[idx] = updated;
    committedState = spotEntries;
    emit changed(spotEntries);
    emit committed(std::move(before), spotEntries);
}

int SpotRemovalPanel::activeIndex() const {
    return spotList->currentRow();
}

void SpotRemovalPanel::deleteActive() {
    const int idx = activeIndex();
    if (idx < 0 || idx >= static_cast<int>(spotEntries.size()))
        return;
    auto before = spotEntries;
    spotEntries.erase(spotEntries.begin() + idx);
    committedState = spotEntries;
    rebuildList();
    emit changed(spotEntries);
    emit committed(std::move(before), spotEntries);
}

void SpotRemovalPanel::rebuildList() {
    const int cur = spotList->currentRow();
    spotList->clear();
    for (int i = 0; i < static_cast<int>(spotEntries.size()); ++i)
        spotList->addItem(QString("Spot %1").arg(i + 1));
    const int next = spotEntries.empty()
                         ? -1
                         : std::clamp(cur, 0, static_cast<int>(spotEntries.size()) - 1);
    spotList->setCurrentRow(next);
    deleteButton->setEnabled(activeIndex() >= 0);
}
