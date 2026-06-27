#include "BatchPaste.h"
#include "io/XmpSidecar.h"

#include <utility>

BatchAdjustmentCommand::BatchAdjustmentCommand(
    QString activePath, QVector<BatchPasteRecord> records, ApplyActive applyActive, QString text)
    : activePath(std::move(activePath)),
      records(std::move(records)),
      applyActive(std::move(applyActive)),
      textPrefix(std::move(text)) {
    const qsizetype n = this->records.size();
    setText(QString("%1 (%2 %3)").arg(textPrefix).arg(n).arg(n == 1 ? "file" : "files"));
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
