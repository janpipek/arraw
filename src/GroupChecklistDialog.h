#pragma once

#include "DevelopGroup.h"

#include <QDialog>

/**
 * Checklist dialog for choosing which Develop Groups travel together.
 *
 * GroupChecklistDialog is used by Copy Settings, Paste Settings, and Save Preset.
 * It presents one checkbox per available group and returns a selection bounded
 * by that availability, which lets paste narrow but never widen the copied set.
 * Checkbox objectNames are developGroupKey values for headless tests.
 */
class GroupChecklistDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(GroupChecklistDialog)
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
