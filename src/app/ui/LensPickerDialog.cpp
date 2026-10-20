#include "LensPickerDialog.h"

#include "LensSupport.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace arraw::app {

namespace {

/// @brief Item role that holds a lens's maker.
constexpr int makerRole = Qt::UserRole;

/// @brief Item role that holds a lens's model.
constexpr int modelRole = Qt::UserRole + 1;

/// @brief Adds a lens to the list.
void addLens(QListWidget& list, const LensIdentity& lens, bool close) {
    QString text = lensDisplayName(lens);
    if (close) {
        text += QObject::tr("  (fits as well)");
    }
    auto* item = new QListWidgetItem(text);
    item->setData(makerRole, QString::fromStdString(lens.maker));
    item->setData(modelRole, QString::fromStdString(lens.model));
    list.addItem(item);
}

} // namespace

LensPickerDialog::LensPickerDialog(const LensDatabase& database, std::optional<std::string> mount,
                                   std::vector<LensIdentity> candidates, QWidget* parent)
    : QDialog(parent), database_(database), mount_(std::move(mount)),
      candidates_(std::move(candidates)) {
    setWindowTitle(tr("Choose Lens"));
    resize(520, 420);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        mount_ ? tr("Lenses that fit the camera's mount (%1). Search by words of the name.")
                     .arg(QString::fromStdString(*mount_))
               : tr("All lenses of the database. Search by words of the name."),
        this));
    search_ = new QLineEdit(this);
    search_->setObjectName("lensSearch");
    search_->setPlaceholderText(tr("Search lenses"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    list_ = new QListWidget(this);
    list_->setObjectName("lensList");
    layout->addWidget(list_, 1);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons_);

    connect(search_, &QLineEdit::textChanged, this, &LensPickerDialog::showMatches);
    connect(list_, &QListWidget::itemSelectionChanged, this,
            [this] { buttons_->button(QDialogButtonBox::Ok)->setEnabled(chosen().has_value()); });
    connect(list_, &QListWidget::itemActivated, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    showMatches({});
    search_->setFocus();
}

void LensPickerDialog::showMatches(const QString& search) {
    list_->clear();
    const std::string words = search.trimmed().toStdString();
    if (words.empty()) {
        for (const LensIdentity& lens : candidates_) {
            addLens(*list_, lens, true);
        }
    }
    const std::vector<LensEntry> lenses = database_.lenses(words, mount_);
    std::size_t listed = 0;
    for (const LensEntry& entry : lenses) {
        const LensIdentity lens{entry.maker, entry.model};
        if (words.empty() && std::ranges::find(candidates_, lens) != candidates_.end()) {
            continue; // Listed first.
        }
        if (listed++ == maximumListed) {
            break;
        }
        addLens(*list_, lens, false);
    }
    if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(chosen().has_value());
}

std::optional<LensIdentity> LensPickerDialog::chosen() const {
    const QListWidgetItem* item = list_->currentItem();
    if (item == nullptr || !item->isSelected()) {
        return std::nullopt;
    }
    return LensIdentity{item->data(makerRole).toString().toStdString(),
                        item->data(modelRole).toString().toStdString()};
}

} // namespace arraw::app
