#pragma once
#include "Snapshot.h"
#include <vector>
#include <QWidget>

class QListWidget;
class QPushButton;
class QUndoStack;

/**
 * Left-dock panel pairing the develop History with named Snapshots (docs/adr/0033).
 *
 * The History list is a QUndoView over MainWindow's shared develop QUndoStack —
 * the session-only, linear list of every edit step, click-to-revisit. The
 * Snapshots list shows the photo's persisted A/B states; double-click (or
 * Restore) applies one as an undoable develop step, while Add/Rename/Delete edit
 * the persisted list directly. The panel owns no canonical state: DevelopSession
 * owns the snapshots and MainWindow drives every mutation.
 */
class HistoryPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(HistoryPanel)
public:
    explicit HistoryPanel(QUndoStack* undoStack, QWidget* parent = nullptr);

    // Replace the displayed snapshot names (load / add / rename / delete).
    void setSnapshots(const std::vector<Snapshot>& snapshots);

signals:
    void addRequested();
    void restoreRequested(int index);
    void renameRequested(int index, const QString& name);
    void deleteRequested(int index);

private:
    int selectedRow() const;

    // Repopulate the History list from the stack: newest edit at the top, the
    // base "Load" state at the bottom, the current position highlighted.
    void rebuildHistory();

    QUndoStack* undoStack;
    QListWidget* snapshotList;
    QListWidget* historyList;
    QPushButton* addButton;
    QPushButton* restoreButton;
    QPushButton* deleteButton;
    bool refreshing = false;        // guards snapshot itemChanged during refills
    bool historyRefreshing = false; // guards history currentRowChanged during refills
};
