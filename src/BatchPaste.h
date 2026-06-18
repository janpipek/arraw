#pragma once

#include "ImagePipeline.h"

#include <QUndoCommand>
#include <QVector>

class AdjustmentPanel;

struct BatchPasteRecord {
    QString path;
    GlobalAdjustment before;
    GlobalAdjustment after;
};

// Undo command for a batch paste: applies settings to N selected files at once,
// writing each file's XMP sidecar immediately (auto-save on paste).
// For the active file, also updates the in-memory AdjustmentPanel. See ADR 0018.
class BatchAdjustmentCommand : public QUndoCommand {
public:
    BatchAdjustmentCommand(
        AdjustmentPanel* panel,
        QString activePath,
        QVector<BatchPasteRecord> records);

    void redo() override;
    void undo() override;

private:
    AdjustmentPanel* panel;
    QString activePath;
    QVector<BatchPasteRecord> records;
};
