#pragma once
#include "develop/Spot.h"
#include <vector>
#include <QWidget>

class QListWidget;
class QPushButton;

/**
 * Panel for managing Spot Removal entries (docs/adr/0017).
 *
 * Each Spot describes a clone-based pixel replacement applied before the shader.
 * SpotRemovalPanel owns the editable list copy and selection UI; ImageViewport
 * edits spot geometry on the image, and DevelopSession owns the active image's
 * canonical spots plus the clean/spotted buffers.
 */
class SpotRemovalPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SpotRemovalPanel)
public:
    explicit SpotRemovalPanel(QWidget* parent = nullptr);

    const std::vector<Spot>& spots() const { return spotEntries; }

    // Replace the whole list (XMP load / undo / redo). Emits changed, not committed.
    void setSpots(const std::vector<Spot>& spots);

    // Update spot[idx] geometry live (viewport drag, no undo entry). Emits changed.
    void updateSpot(int idx, const Spot& updated);

    // Commit an in-progress drag of spot[idx] to its current geometry.
    // Snapshots before → after and emits committed for undo.
    void commitSpotEdit(int idx, const Spot& updated);

    // Add a spot from viewport placement, snapshot for undo, emit committed.
    void addSpot(const Spot& spot);

    int activeIndex() const;

public slots:
    void deleteActive();

signals:
    void changed(const std::vector<Spot>&);
    void committed(std::vector<Spot> before, std::vector<Spot> after);

private:
    void rebuildList();

    QListWidget* spotList;
    QPushButton* deleteButton;
    std::vector<Spot> spotEntries;
    std::vector<Spot> committedState; // baseline for the next undo step
};
