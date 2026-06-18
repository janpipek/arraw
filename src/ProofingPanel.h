#pragma once
#include "ColorManagement.h"
#include <QWidget>

class QGroupBox;
class QComboBox;
class QCheckBox;

// Soft-proofing controls (view state — never stored in the XMP sidecar).
// All settings persist via QSettings.
class ProofingPanel : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ProofingPanel)
public:
    explicit ProofingPanel(QWidget* parent = nullptr);

    bool proofingEnabled() const;
    void setProofingEnabled(bool on); // `S` key toggle

    QString profilePath() const;
    QString profileName() const; // for the status-bar indicator
    ProofIntent intent() const;
    bool blackPointCompensation() const;
    bool gamutWarning() const;

signals:
    void proofingChanged();

private:
    void browseForProfile();
    void selectProfile(const QString& path);
    void persistAndNotify();

    QGroupBox* group;
    QComboBox* profileBox;
    QComboBox* intentBox;
    QCheckBox* bpcCheck;
    QCheckBox* warnCheck;
};
