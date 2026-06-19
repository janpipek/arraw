#include "BatchPaste.h"
#include "XmpSidecar.h"

#include <utility>

BatchAdjustmentCommand::BatchAdjustmentCommand(
    QString activePath, QVector<BatchPasteRecord> records, ApplyActive applyActive)
    : activePath(std::move(activePath)),
      records(std::move(records)),
      applyActive(std::move(applyActive)) {
    const qsizetype n = this->records.size();
    setText(QString("Paste Settings (%1 %2)").arg(n).arg(n == 1 ? "file" : "files"));
}

void BatchAdjustmentCommand::redo() {
    for (const auto& rec : records) {
        XmpSidecar::saveAdjustments(rec.path, rec.after);
        if (applyActive && rec.path == activePath)
            applyActive(rec.after);
    }
}

void BatchAdjustmentCommand::undo() {
    for (const auto& rec : records) {
        XmpSidecar::saveAdjustments(rec.path, rec.before);
        if (applyActive && rec.path == activePath)
            applyActive(rec.before);
    }
}
