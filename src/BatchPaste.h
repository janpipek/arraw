#pragma once

#include "develop/GlobalAdjustment.h"

#include <functional>
#include <QString>
#include <QUndoCommand>
#include <QVector>

struct BatchPasteRecord {
    QString path;
    GlobalAdjustment before;
    GlobalAdjustment after;
};

// True if any record targets the active image. The session History is per-image
// (resets on load, ADR 0038), so a batch belongs on it only when it actually
// touches the open image; an off-image batch auto-saves XMP without a history
// step (see writeBatchAfter).
bool batchTouchesActive(const QVector<BatchPasteRecord>& records, const QString& activePath);

// Persist each record's after-state to its XMP sidecar (the durable effect of a
// batch apply). Used both by BatchAdjustmentCommand::redo and to apply an
// off-image batch directly when it gets no undo step.
void writeBatchAfter(const QVector<BatchPasteRecord>& records);

// Undo command for a batch paste: applies settings to N selected files at once,
// writing each file's XMP sidecar immediately (auto-save on paste).
// For the active file, calls applyActive so the application can update its
// canonical in-memory state. See ADR 0018.
class BatchAdjustmentCommand : public QUndoCommand {
public:
    using ApplyActive = std::function<void(const GlobalAdjustment&)>;

    BatchAdjustmentCommand(
        QString activePath,
        QVector<BatchPasteRecord> records,
        ApplyActive applyActive = {},
        QString text = "Paste Settings");

    void redo() override;
    void undo() override;

private:
    QString activePath;
    QVector<BatchPasteRecord> records;
    ApplyActive applyActive;
    QString textPrefix;
};
