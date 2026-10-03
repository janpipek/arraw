#include "CullingActions.h"

#include "FilmStrip.h"
#include "FilmStripRules.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>

namespace arraw::app {

namespace {

/// Keys of the labels, in the order of ::arraw::colorLabelNames.
constexpr Qt::Key labelKeys[] = {Qt::Key_R, Qt::Key_Y, Qt::Key_G, Qt::Key_B, Qt::Key_P};

} // namespace

CullingActions::CullingActions(QMainWindow& window, FilmStrip& strip)
    : QObject(&window), window_(window), strip_(strip) {
    QMenu* menu = window.menuBar()->addMenu(tr("&Image"));

    QMenu* rateMenu = menu->addMenu(tr("&Rating"));
    const auto addRating = [&](const QString& text, int rating, Qt::Key key) {
        QAction* action = rateMenu->addAction(text);
        action->setShortcut(QKeySequence(key));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, rating] { strip_.rate(rating); });
        ratingActions_.emplace_back(action, rating);
    };
    for (int stars = highestRating; stars >= 1; --stars) {
        addRating(QString(stars, QChar(0x2605)), stars, static_cast<Qt::Key>(Qt::Key_0 + stars));
    }
    rateMenu->addSeparator();
    addRating(tr("&Unrated"), 0, Qt::Key_0);
    addRating(tr("Re&ject"), rejectedRating, Qt::Key_X);

    QMenu* labelMenu = menu->addMenu(tr("&Label"));
    std::size_t key = 0;
    for (const auto& [label, name] : colorLabelNames) {
        QAction* action = labelMenu->addAction(tr(labelName(label)));
        action->setShortcut(QKeySequence(labelKeys[key++]));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this,
                [this, label = label] { strip_.toggleLabel(label); });
        labelActions_.emplace_back(action, label);
    }
    labelMenu->addSeparator();
    QAction* none = labelMenu->addAction(tr("&None"));
    connect(none, &QAction::triggered, this, [this] { strip_.clearLabel(); });
    otherActions_.push_back(none);

    menu->addSeparator();
    QAction* previous = menu->addAction(tr("&Previous Photo"));
    previous->setShortcuts({QKeySequence(Qt::Key_Left), QKeySequence(Qt::CTRL | Qt::Key_Left)});
    connect(previous, &QAction::triggered, this, [this] { strip_.navigate(-1); });
    QAction* next = menu->addAction(tr("&Next Photo"));
    next->setShortcuts({QKeySequence(Qt::Key_Right), QKeySequence(Qt::CTRL | Qt::Key_Right)});
    connect(next, &QAction::triggered, this, [this] { strip_.navigate(1); });
    // Stepping needs shots shown, not an active one: from no shot it goes to the first or last.
    // The strip refuses at the ends, so these stay enabled.

    connect(menu, &QMenu::aboutToShow, this, &CullingActions::reflectActiveShot);
    connect(&strip, &FilmStrip::activeChanged, this, &CullingActions::reflectActiveShot);
    reflectActiveShot();

    qApp->installEventFilter(this);
}

CullingActions::~CullingActions() {
    if (qApp != nullptr) {
        qApp->removeEventFilter(this);
    }
}

void CullingActions::reflectActiveShot() {
    const bool any = strip_.activePrimary().has_value();
    const PhotoMarks marks = strip_.activeMarks();
    for (const auto& [action, rating] : ratingActions_) {
        action->setEnabled(any);
        action->setChecked(any && marks.rating == rating);
    }
    for (const auto& [action, label] : labelActions_) {
        action->setEnabled(any);
        action->setChecked(any && marks.label == label);
    }
    for (QAction* action : otherActions_) {
        action->setEnabled(any);
    }
}

bool CullingActions::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && QApplication::activeWindow() == &window_) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::ControlModifier &&
            (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
            // Taken here, the event is neither accepted nor delivered: the focused line edit
            // does not get to claim it for moving by a word, and the shortcut fires.
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace arraw::app
