#include "SpotRemovalPanel.h"

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
    connect(spotList, &QListWidget::currentRowChanged, this,
            [this](int) { deleteButton->setEnabled(activeIndex() >= 0); });

    deleteButton->setEnabled(false);
}

void SpotRemovalPanel::setSpots(const std::vector<Spot>& spots) {
    m_spots = spots;
    m_committedState = spots;
    rebuildList();
    emit changed(m_spots);
}

void SpotRemovalPanel::addSpot(const Spot& spot) {
    auto before = m_spots;
    m_spots.push_back(spot);
    m_committedState = m_spots;
    rebuildList();
    spotList->setCurrentRow(static_cast<int>(m_spots.size()) - 1);
    emit changed(m_spots);
    emit committed(std::move(before), m_spots);
}

void SpotRemovalPanel::updateSpot(int idx, const Spot& updated) {
    if (idx < 0 || idx >= static_cast<int>(m_spots.size()))
        return;
    m_spots[idx] = updated;
    emit changed(m_spots);
}

void SpotRemovalPanel::commitSpotEdit(int idx, const Spot& updated) {
    if (idx < 0 || idx >= static_cast<int>(m_spots.size()))
        return;
    auto before = m_committedState;
    m_spots[idx] = updated;
    m_committedState = m_spots;
    emit changed(m_spots);
    emit committed(std::move(before), m_spots);
}

int SpotRemovalPanel::activeIndex() const {
    return spotList->currentRow();
}

void SpotRemovalPanel::deleteActive() {
    const int idx = activeIndex();
    if (idx < 0 || idx >= static_cast<int>(m_spots.size()))
        return;
    auto before = m_spots;
    m_spots.erase(m_spots.begin() + idx);
    m_committedState = m_spots;
    rebuildList();
    emit changed(m_spots);
    emit committed(std::move(before), m_spots);
}

void SpotRemovalPanel::rebuildList() {
    const int cur = spotList->currentRow();
    spotList->clear();
    for (int i = 0; i < static_cast<int>(m_spots.size()); ++i)
        spotList->addItem(QString("Spot %1").arg(i + 1));
    const int next = std::clamp(cur, 0, static_cast<int>(m_spots.size()) - 1);
    spotList->setCurrentRow(m_spots.empty() ? -1 : next);
    deleteButton->setEnabled(activeIndex() >= 0);
}
