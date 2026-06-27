#include "ui/ProofingPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

ProofingPanel::ProofingPanel(QWidget* parent)
    : QWidget(parent) {
    QSettings settings;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    group = new QGroupBox("Soft Proofing");
    group->setCheckable(true);
    group->setChecked(settings.value("proof/enabled", false).toBool());
    root->addWidget(group);

    auto* form = new QFormLayout(group);

    profileBox = new QComboBox;
    // Some ICC descriptions are very long; don't let them dictate the panel
    // width. Cap the closed combo to a fixed character budget (it elides) and
    // let it shrink — the full name still shows in the dropdown.
    profileBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profileBox->setMinimumContentsLength(10);
    profileBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // Proof targets: anything printable or displayable found on the system.
    for (const IccProfileInfo& info : scanSystemProfiles())
        profileBox->addItem(info.description, info.path);
    auto* browseBtn = new QPushButton("…");
    browseBtn->setFixedWidth(28);
    browseBtn->setToolTip("Browse for an ICC profile");
    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget(profileBox, 1);
    profileRow->addWidget(browseBtn);
    form->addRow("Profile:", profileRow);

    intentBox = new QComboBox;
    intentBox->addItems({"Perceptual", "Relative Colorimetric"});
    intentBox->setCurrentIndex(settings.value("proof/intent", 0).toInt());
    form->addRow("Intent:", intentBox);

    bpcCheck = new QCheckBox("Black point compensation");
    bpcCheck->setChecked(settings.value("proof/bpc", true).toBool());
    form->addRow(bpcCheck);

    warnCheck = new QCheckBox("Gamut warning");
    warnCheck->setChecked(settings.value("proof/gamutWarning", false).toBool());
    form->addRow(warnCheck);

    selectProfile(settings.value("proof/profile").toString());

    connect(group, &QGroupBox::toggled, this, &ProofingPanel::persistAndNotify);
    connect(
        profileBox,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &ProofingPanel::persistAndNotify);
    connect(
        intentBox,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        &ProofingPanel::persistAndNotify);
    connect(bpcCheck, &QCheckBox::toggled, this, &ProofingPanel::persistAndNotify);
    connect(warnCheck, &QCheckBox::toggled, this, &ProofingPanel::persistAndNotify);
    connect(browseBtn, &QPushButton::clicked, this, &ProofingPanel::browseForProfile);
}

bool ProofingPanel::proofingEnabled() const {
    return group->isChecked() && !profilePath().isEmpty();
}

void ProofingPanel::setProofingEnabled(bool on) {
    group->setChecked(on); // toggled() fires persistAndNotify
}

QString ProofingPanel::profilePath() const {
    return profileBox->currentData().toString();
}

QString ProofingPanel::profileName() const {
    return profileBox->currentText();
}

ProofIntent ProofingPanel::intent() const {
    return intentBox->currentIndex() == 0 ? ProofIntent::Perceptual
                                          : ProofIntent::RelativeColorimetric;
}

bool ProofingPanel::blackPointCompensation() const {
    return bpcCheck->isChecked();
}

bool ProofingPanel::gamutWarning() const {
    return warnCheck->isChecked();
}

void ProofingPanel::browseForProfile() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Select ICC Profile", QString(), "ICC Profiles (*.icc *.icm)");
    if (!path.isEmpty())
        selectProfile(path);
}

// Select by path; profiles outside the system scan (browsed or persisted)
// are appended to the combo first.
void ProofingPanel::selectProfile(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    int idx = profileBox->findData(path);
    if (idx < 0) {
        profileBox->addItem(QFileInfo(path).baseName(), path);
        idx = profileBox->count() - 1;
    }
    profileBox->setCurrentIndex(idx);
}

void ProofingPanel::persistAndNotify() {
    QSettings s;
    s.setValue("proof/enabled", group->isChecked());
    s.setValue("proof/profile", profilePath());
    s.setValue("proof/intent", intentBox->currentIndex());
    s.setValue("proof/bpc", bpcCheck->isChecked());
    s.setValue("proof/gamutWarning", warnCheck->isChecked());
    emit proofingChanged();
}
