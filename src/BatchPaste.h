#pragma once

#include "ImagePipeline.h"

#include <functional>
#include <QString>
#include <QUndoCommand>
#include <QVector>

struct BatchPasteRecord {
    QString path;
    GlobalAdjustment before;
    GlobalAdjustment after;
};

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
