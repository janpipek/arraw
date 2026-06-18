#pragma once

#include <QDialog>

class QProgressBar;
class QLabel;
class QPushButton;

// Modal progress dialog for long batch operations (export, etc.). Shows a
// progress bar, a per-item filename label, and a Cancel button. The caller
// polls wasCancelled() between items and aborts the loop when true.
class BatchProgressDialog : public QDialog {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(BatchProgressDialog)
public:
    explicit BatchProgressDialog(int total, QWidget* parent = nullptr);

    // Update the label to show which file is being processed.
    void setCurrentFile(const QString& filename);

    // Advance the progress bar. n is the number of completed items (0-based).
    void setValue(int n);

    bool wasCancelled() const { return cancelled; }

signals:
    // Emitted the moment Cancel is clicked, so a caller waiting on a background
    // task can abort it promptly (wasCancelled() only reports the state after).
    void cancelRequested();

private:
    QProgressBar* bar;
    QLabel* fileLabel;
    bool cancelled = false;
};
