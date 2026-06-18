#include "BatchPaste.h"
#include "AdjustmentPanel.h"
#include "XmpSidecar.h"

BatchAdjustmentCommand::BatchAdjustmentCommand(
    AdjustmentPanel* panel, QString activePath, QVector<BatchPasteRecord> records)
    : panel(panel), activePath(std::move(activePath)), records(std::move(records)) {
    const qsizetype n = this->records.size();
    setText(QString("Paste Settings (%1 %2)").arg(n).arg(n == 1 ? "file" : "files"));
}

void BatchAdjustmentCommand::redo() {
    for (const auto& rec : records) {
        XmpSidecar::saveAdjustments(rec.path, rec.after);
        if (panel && rec.path == activePath)
            panel->setParams(rec.after);
    }
}

void BatchAdjustmentCommand::undo() {
    for (const auto& rec : records) {
        XmpSidecar::saveAdjustments(rec.path, rec.before);
        if (panel && rec.path == activePath)
            panel->setParams(rec.before);
    }
}
