#pragma once

#include "core/SystemInfo.h"

#include <QDialog>

// Help > System Info (docs/adr/0057): renders a sysinfo::Info snapshot as
// grouped label/value rows, with a Copy to Clipboard button for bug reports.
// Never touches a QRhi or the filesystem itself — the caller resolves those
// facts and passes a ready-made Info in.
class SystemInfoDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SystemInfoDialog)
public:
    explicit SystemInfoDialog(const sysinfo::Info& info, QWidget* parent = nullptr);
};
