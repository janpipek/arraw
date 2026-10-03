#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QAbstractItemModel;
class QSortFilterProxyModel;
class QTabWidget;
class QTableView;

namespace arraw::app {

struct DebugLog;

/// @brief Window listing what the application has logged: its diagnostics and Qt's messages.
///
/// One tab each, sharing a filter that matches any column. Rows stay in the
/// log while the window is closed, so opening it late still shows how the
/// session began.
class DebugWindow final : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(DebugWindow)
public:
    /// @brief Makes the window, showing a log.
    /// @param log Log to show; must outlive the window.
    /// @param parent Window it belongs to; it stays a window of its own.
    explicit DebugWindow(DebugLog& log, QWidget* parent = nullptr);

private:
    /// @brief Adds a tab showing a model through a filter.
    /// @param model Rows to show.
    /// @param title Tab's title.
    void addTab(QAbstractItemModel& model, const QString& title);

    /// @brief Copies the current tab's visible rows, tab-separated, to the clipboard.
    void copyVisibleRows() const;

    /// @brief Empties the current tab's log.
    void clearCurrent();

    DebugLog& log_;
    QTabWidget* tabs_ = nullptr;
    QList<QSortFilterProxyModel*> filters_;
};

} // namespace arraw::app
