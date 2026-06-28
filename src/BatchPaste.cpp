#include "BatchPaste.h"
#include "io/XmpSidecar.h"

#include <algorithm>
#include <utility>

bool batchTouchesActive(const QVector<BatchPasteRecord>& records, const QString& activePath) {
    return std::any_of(records.cbegin(), records.cend(), [&](const BatchPasteRecord& rec) {
        return rec.path == activePath;
    });
}

void writeBatchAfter(const QVector<BatchPasteRecord>& records) {
    for (const auto& rec : records)
        XmpSidecar::saveAdjustments(rec.path, rec.after);
}

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
    writeBatchAfter(records);
    if (applyActive)
        for (const auto& rec : records)
            if (rec.path == activePath)
                applyActive(rec.after);
}

void BatchAdjustmentCommand::undo() {
    for (const auto& rec : records) {
        XmpSidecar::saveAdjustments(rec.path, rec.before);
        if (applyActive && rec.path == activePath)
            applyActive(rec.before);
    }
}
