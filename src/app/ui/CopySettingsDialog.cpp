#include "CopySettingsDialog.h"

#include "CopySections.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace arraw::app {

CopySettingsDialog::CopySettingsDialog(std::span<const CopySection> checked, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Copy Settings"));

    auto* layout = new QVBoxLayout(this);
    for (const CopySection section : copyableSections) {
        // A checkbox reads '&' as a mnemonic, so the label is escaped here only.
        QString label = copySectionLabel(section);
        label.replace('&', "&&");
        auto* box = new QCheckBox(label, this);
        box->setObjectName(
            QString::fromUtf8(copySectionNames[static_cast<std::size_t>(section)].data()));
        box->setChecked(std::ranges::find(checked, section) != checked.end());
        connect(box, &QCheckBox::toggled, this, &CopySettingsDialog::updateAccept);
        layout->addWidget(box);
        boxes_.push_back(box);
    }

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* all = buttons_->addButton(tr("Check All"), QDialogButtonBox::ActionRole);
    all->setObjectName("checkAll");
    auto* none = buttons_->addButton(tr("Check None"), QDialogButtonBox::ActionRole);
    none->setObjectName("checkNone");
    connect(all, &QPushButton::clicked, this, [this] { setAll(true); });
    connect(none, &QPushButton::clicked, this, [this] { setAll(false); });
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons_);
    updateAccept();
}

std::vector<CopySection> CopySettingsDialog::sections() const {
    std::vector<CopySection> chosen;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        if (boxes_[i]->isChecked()) {
            chosen.push_back(copyableSections[i]);
        }
    }
    return chosen;
}

void CopySettingsDialog::updateAccept() {
    const bool any = std::ranges::any_of(boxes_, &QCheckBox::isChecked);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(any);
}

void CopySettingsDialog::setAll(bool checked) {
    for (QCheckBox* box : boxes_) {
        box->setChecked(checked);
    }
}

} // namespace arraw::app
