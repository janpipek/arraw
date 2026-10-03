#pragma once

#include <Diagnostics.h>

#include <QAbstractTableModel>
#include <QDateTime>
#include <QMetaObject>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iterator>
#include <mutex>
#include <utility>

namespace arraw::app {

/// @brief A table of log entries, oldest first, that any thread may add to.
///
/// New entries wait and come in as rows together, flushInterval after the
/// first of them, on the model's own thread. That keeps a flood of messages
/// from becoming a flood of row insertions, and breaks the loop a view would
/// otherwise make with Qt's own logging: painting a new row logs, which adds
/// a row, which paints.
///
/// Holds at most rowLimit rows and drops the oldest beyond that, so a chatty
/// session cannot grow it without end. Subclasses say what the columns are.
/// @tparam Entry One row's worth of data.
template <typename Entry> class LogModel : public QAbstractTableModel {
public:
    /// Most rows kept.
    static constexpr std::size_t rowLimit = 10'000;
    /// How long new entries wait to come in as rows.
    static constexpr std::chrono::milliseconds flushInterval{100};

    using QAbstractTableModel::QAbstractTableModel;

    /// @brief Removes every row, and every entry still waiting to become one.
    void clear() {
        {
            const std::scoped_lock lock(pendingMutex_);
            pending_.clear();
        }
        beginResetModel();
        entries_.clear();
        endResetModel();
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries_.size());
    }

protected:
    /// @brief Adds an entry, to come in as the newest row shortly.
    ///
    /// Safe from any thread. Qt drops the flush if the model is gone by then.
    /// @param entry Entry to add.
    void append(Entry entry) {
        bool first = false;
        {
            const std::scoped_lock lock(pendingMutex_);
            if (pending_.size() >= rowLimit) {
                pending_.pop_front();
            }
            pending_.push_back(std::move(entry));
            first = !flushScheduled_;
            flushScheduled_ = true;
        }
        if (first) {
            // Queued even from the model's thread, so that no row comes in
            // while a view is in the middle of painting.
            QMetaObject::invokeMethod(
                this, [this] { QTimer::singleShot(flushInterval, this, [this] { flush(); }); },
                Qt::QueuedConnection);
        }
    }

    /// @brief Entry shown in a row.
    [[nodiscard]] const Entry& entryAt(int row) const {
        return entries_[static_cast<std::size_t>(row)];
    }

private:
    /// @brief Turns the waiting entries into rows; on the model's thread only.
    void flush() {
        std::deque<Entry> arrived;
        {
            const std::scoped_lock lock(pendingMutex_);
            arrived.swap(pending_);
            flushScheduled_ = false;
        }
        if (arrived.empty()) {
            return;
        }
        const std::size_t overflow =
            entries_.size() + arrived.size() > rowLimit
                ? std::min(entries_.size(), entries_.size() + arrived.size() - rowLimit)
                : 0;
        if (overflow > 0) {
            beginRemoveRows({}, 0, static_cast<int>(overflow) - 1);
            entries_.erase(entries_.begin(),
                           entries_.begin() + static_cast<std::ptrdiff_t>(overflow));
            endRemoveRows();
        }
        const int first = static_cast<int>(entries_.size());
        beginInsertRows({}, first, first + static_cast<int>(arrived.size()) - 1);
        std::move(arrived.begin(), arrived.end(), std::back_inserter(entries_));
        endInsertRows();
    }

    std::deque<Entry> entries_;
    std::mutex pendingMutex_;
    std::deque<Entry> pending_;   ///< Entries not yet rows; guarded by pendingMutex_.
    bool flushScheduled_ = false; ///< Whether a flush is on its way; guarded by pendingMutex_.
};

/// @brief One diagnostic the application recorded, and when.
struct DiagnosticEntry {
    QDateTime time;        ///< When it was recorded.
    Diagnostic diagnostic; ///< What was recorded.
};

/// @brief Diagnostics the application has recorded, as a table.
///
/// Kept structured, so the notice and the subject stay columns of their own
/// rather than parts of a sentence.
class DiagnosticModel final : public LogModel<DiagnosticEntry> {
public:
    /// @brief Columns of the table.
    enum Column { TimeColumn, SeverityColumn, NoticeColumn, SubjectColumn, MessageColumn };

    using LogModel::LogModel;

    /// @brief Adds a diagnostic, stamped with the current time, as the newest row.
    /// @param diagnostic Diagnostic to add.
    void append(const Diagnostic& diagnostic);

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
};

/// @brief One message that went through Qt's message handler, and when.
struct QtMessageEntry {
    QDateTime time;   ///< When it was logged.
    QtMsgType type;   ///< Qt's kind of message.
    QString category; ///< Logging category, such as "qt.rhi.general" or "default".
    QString text;     ///< The message.
};

/// @brief Messages that went through Qt's message handler, as a table.
class QtMessageModel final : public LogModel<QtMessageEntry> {
public:
    /// @brief Columns of the table.
    enum Column { TimeColumn, LevelColumn, CategoryColumn, MessageColumn };

    using LogModel::LogModel;

    /// @brief Adds a message, stamped with the current time, as the newest row.
    /// @param type Qt's kind of message.
    /// @param category Logging category.
    /// @param text The message.
    void append(QtMsgType type, const QString& category, const QString& text);

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
};

/// @brief Everything the debug window shows, kept for as long as the application runs.
///
/// arraw's diagnostics and Qt's own messages apart: the first structured, the
/// second whatever Qt and its plugins say, as text.
struct DebugLog {
    DiagnosticModel diagnostics; ///< arraw's diagnostics.
    QtMessageModel qtMessages;   ///< Qt's messages, other than the diagnostics' own echo.
};

} // namespace arraw::app
