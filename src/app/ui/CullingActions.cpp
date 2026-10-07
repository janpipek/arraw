#include "CullingActions.h"

#include "FilmStrip.h"
#include "FilmStripRules.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>

namespace arraw::app {

namespace {

/// Keys of the labels, in the order of ::arraw::colorLabelNames: the colour's initial. The crop
/// mode is on C, so R is free for red (ADR 040).
const QKeyCombination labelKeys[] = {QKeyCombination(Qt::Key_R), QKeyCombination(Qt::Key_Y),
                                     QKeyCombination(Qt::Key_G), QKeyCombination(Qt::Key_B),
                                     QKeyCombination(Qt::Key_P)};

} // namespace

CullingActions::CullingActions(QMainWindow& window, FilmStrip& strip, QMenu& menu)
    : QObject(&window), window_(window), strip_(strip) {
    QMenu* rateMenu = menu.addMenu(tr("&Rating"));
    const auto addRating = [&](const QString& text, int rating, Qt::Key key) {
        QAction* action = rateMenu->addAction(text);
        action->setObjectName(QString("rating%1Action").arg(rating));
        action->setShortcut(QKeySequence(key));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, rating] { strip_.rate(rating); });
        ratingActions_.emplace_back(action, rating);
        return action;
    };
    for (int stars = highestRating; stars >= 1; --stars) {
        addRating(QString(stars, QChar(0x2605)), stars, static_cast<Qt::Key>(Qt::Key_0 + stars));
    }
    rateMenu->addSeparator();
    addRating(tr("&Unrated"), 0, Qt::Key_0);
    rejectAction_ = addRating(tr("Re&ject"), rejectedRating, Qt::Key_X);
    rejectAction_->setObjectName("rejectAction");

    // The menu is a radio group whose items set a label, or none. The keys
    // toggle instead (main's behaviour: the red key on a red shot clears it), so they are
    // separate window actions; the menu only shows them, after a tab.
    QMenu* labelMenu = menu.addMenu(tr("&Label"));
    auto* labelGroup = new QActionGroup(labelMenu);
    labelGroup->setExclusive(true);
    const auto addLabel = [&](const QString& text, std::optional<ColorLabel> label) {
        QAction* action = labelMenu->addAction(text);
        action->setObjectName(label ? QString("label%1Action").arg(static_cast<int>(*label))
                                    : QString("labelNoneAction"));
        action->setCheckable(true);
        action->setActionGroup(labelGroup);
        connect(action, &QAction::triggered, this, [this, label] { strip_.setLabel(label); });
        labelActions_.emplace_back(action, label);
    };
    std::size_t key = 0;
    for (const auto& [label, name] : colorLabelNames) {
        const QKeySequence shortcut(labelKeys[key++]);
        addLabel(tr(labelName(label)) + QLatin1Char('\t') +
                     shortcut.toString(QKeySequence::NativeText),
                 label);
        auto* toggle = new QAction(this);
        toggle->setObjectName(QString("toggleLabel%1Action").arg(static_cast<int>(label)));
        toggle->setShortcut(shortcut);
        connect(toggle, &QAction::triggered, this,
                [this, label = label] { strip_.toggleLabel(label); });
        window.addAction(toggle);
        otherActions_.push_back(toggle);
    }
    labelMenu->addSeparator();
    addLabel(tr("&None"), std::nullopt);

    menu.addSeparator();
    QAction* previous = menu.addAction(tr("&Previous Photo"));
    previous->setObjectName("previousPhotoAction");
    previous->setShortcuts({QKeySequence(Qt::Key_Left), QKeySequence(Qt::CTRL | Qt::Key_Left)});
    connect(previous, &QAction::triggered, this, [this] { strip_.navigate(-1); });
    QAction* next = menu.addAction(tr("&Next Photo"));
    next->setObjectName("nextPhotoAction");
    next->setShortcuts({QKeySequence(Qt::Key_Right), QKeySequence(Qt::CTRL | Qt::Key_Right)});
    connect(next, &QAction::triggered, this, [this] { strip_.navigate(1); });
    // Stepping needs shots shown, not an active one: from no shot it goes to the first or last.
    // The strip refuses at the ends, so these stay enabled.

    connect(&menu, &QMenu::aboutToShow, this, &CullingActions::reflectActiveShot);
    connect(&strip, &FilmStrip::activeChanged, this, &CullingActions::reflectActiveShot);
    reflectActiveShot();

    qApp->installEventFilter(this);
}

CullingActions::~CullingActions() {
    if (qApp != nullptr) {
        qApp->removeEventFilter(this);
    }
}

void CullingActions::setCropMode(bool cropping) {
    cropping_ = cropping;
    reflectActiveShot();
}

void CullingActions::reflectActiveShot() {
    const bool any = strip_.activePrimary().has_value();
    const PhotoMarks marks = strip_.activeMarks();
    for (const auto& [action, rating] : ratingActions_) {
        action->setEnabled(any && !(cropping_ && action == rejectAction_));
        action->setChecked(any && marks.rating == rating);
    }
    for (const auto& [action, label] : labelActions_) {
        action->setEnabled(any);
        // An exclusive group cannot be unchecked item by item; with no active
        // shot nothing is checked, so the group lets go first.
        action->actionGroup()->setExclusive(any);
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
