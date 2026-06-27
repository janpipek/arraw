#include "ui/GroupChecklistDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>

GroupChecklistDialog::GroupChecklistDialog(
    const QString& title, GroupSelection available, GroupSelection preselected, QWidget* parent)
    : QDialog(parent),
      available(available) {
    setWindowTitle(title);

    auto* layout = new QVBoxLayout(this);

    // One checkbox per available group, in enum order. Offering only the
    // available groups is what bounds the result (paste narrows, never widens).
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (!hasGroup(available, g))
            continue;
        auto* check = new QCheckBox(developGroupLabel(g), this);
        check->setObjectName(developGroupKey(g));
        check->setChecked(hasGroup(preselected, g));
        layout->addWidget(check);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

GroupSelection GroupChecklistDialog::selectedGroups() const {
    GroupSelection result;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        const auto* check = findChild<QCheckBox*>(developGroupKey(g));
        if (check && check->isChecked())
            result.set(static_cast<size_t>(i));
    }
    return result;
}
