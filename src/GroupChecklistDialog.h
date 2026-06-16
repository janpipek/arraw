#pragma once

#include "DevelopGroup.h"

#include <QDialog>

// The "pick which Develop Groups travel" checklist behind Copy Settings, Paste
// Settings, and Save Preset (Milestone 8). One checkbox per *available* group,
// so the returned selection is structurally bounded by `available` — that is how
// paste "narrows but never widens" the copied set. Checkbox objectNames are the
// group keys (developGroupKey), for headless testing.
class GroupChecklistDialog : public QDialog {
    Q_OBJECT
public:
    GroupChecklistDialog(
        const QString& title,
        GroupSelection available,
        GroupSelection preselected,
        QWidget* parent = nullptr);

    // The currently ticked groups; always a subset of `available`.
    GroupSelection selectedGroups() const;

private:
    GroupSelection available;
};
