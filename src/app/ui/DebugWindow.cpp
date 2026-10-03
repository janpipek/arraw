#include "DebugWindow.h"

#include "DebugLog.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <memory>

namespace arraw::app {

DebugWindow::DebugWindow(DebugLog& log, QWidget* parent) : QWidget(parent, Qt::Window), log_(log) {
    setWindowTitle(tr("Debug Log"));
    resize(900, 400);

    auto* filter = new QLineEdit(this);
    filter->setPlaceholderText(tr("Filter"));
    filter->setClearButtonEnabled(true);
    auto* copy = new QPushButton(tr("Copy"), this);
    copy->setToolTip(tr("Copy the rows shown, tab-separated"));
    auto* clear = new QPushButton(tr("Clear"), this);

    auto* bar = new QHBoxLayout;
    bar->addWidget(filter, 1);
    bar->addWidget(copy);
    bar->addWidget(clear);

    tabs_ = new QTabWidget(this);
    addTab(log_.diagnostics, tr("Diagnostics"));
    addTab(log_.qtMessages, tr("Qt Messages"));

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(tabs_);

    connect(filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (QSortFilterProxyModel* proxy : filters_) {
            proxy->setFilterFixedString(text);
        }
    });
    connect(copy, &QPushButton::clicked, this, &DebugWindow::copyVisibleRows);
    connect(clear, &QPushButton::clicked, this, &DebugWindow::clearCurrent);
}

void DebugWindow::addTab(QAbstractItemModel& model, const QString& title) {
    auto* proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(&model);
    proxy->setFilterKeyColumn(-1); // Any column.
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    filters_.append(proxy);

    auto* view = new QTableView(tabs_);
    view->setModel(proxy);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setWordWrap(false);
    view->setShowGrid(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 4);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->resizeColumnsToContents();
    view->scrollToBottom();

    // Follow new rows while the view is at the bottom; leave it alone once
    // someone has scrolled up to read.
    auto* scroll = view->verticalScrollBar();
    auto following = std::make_shared<bool>(true);
    connect(scroll, &QScrollBar::valueChanged, view,
            [scroll, following](int value) { *following = value == scroll->maximum(); });
    connect(proxy, &QAbstractItemModel::rowsInserted, view, [view, following] {
        if (*following) {
            view->scrollToBottom();
        }
    });

    tabs_->addTab(view, title);
}

void DebugWindow::copyVisibleRows() const {
    const QSortFilterProxyModel* proxy = filters_.value(tabs_->currentIndex());
    if (!proxy) {
        return;
    }
    QStringList lines;
    for (int row = 0; row < proxy->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < proxy->columnCount(); ++column) {
            cells.append(proxy->index(row, column).data().toString());
        }
        lines.append(cells.join(u'\t'));
    }
    QApplication::clipboard()->setText(lines.join(u'\n'));
}

void DebugWindow::clearCurrent() {
    // Tabs in the order the constructor adds them.
    if (tabs_->currentIndex() == 0) {
        log_.diagnostics.clear();
    } else {
        log_.qtMessages.clear();
    }
}

} // namespace arraw::app
