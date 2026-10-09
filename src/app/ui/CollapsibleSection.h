#pragma once

#include <QString>
#include <QWidget>

class QPushButton;

namespace arraw::app {

/// @brief A titled group of controls that opens and closes, remembering which it was.
///
/// The title row (an arrow, ▸ closed and ▾ open, and the title) is one button over the whole
/// width: a click or Space on it flips the section. It takes the focus by Tab only, so a click
/// leaves the keys with the photograph (ADR 040). The controls go in body(), which the section
/// hides when closed. Whether it is open is kept in QSettings under the section's stable
/// identifier, not its translated title, and read back on construction; the default is open
/// (ADR 048).
class CollapsibleSection : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(CollapsibleSection)
public:
    /// @brief Builds the section, open unless the settings say it was closed.
    /// @param id Stable identifier the open state is kept under; not shown.
    /// @param title Text of the title row.
    /// @param parent Owning widget.
    CollapsibleSection(const QString& id, const QString& title, QWidget* parent = nullptr);

    /// @brief Gives the widget the controls are children of; give it a layout.
    [[nodiscard]] QWidget* body() const noexcept {
        return body_;
    }

    /// @brief Gives the title as it was passed, without the arrow.
    [[nodiscard]] QString title() const {
        return title_;
    }

    /// @brief Gives the identifier the open state is kept under.
    [[nodiscard]] QString id() const {
        return id_;
    }

    /// @brief Tells whether the controls are shown.
    [[nodiscard]] bool isOpen() const noexcept {
        return open_;
    }

    /// @brief Opens or closes the section and remembers it in the settings.
    void setOpen(bool open);

    /// @brief Gives the settings key the open state of a section is kept under.
    [[nodiscard]] static QString settingsKey(const QString& id);

    /// @brief Gives a width that does not depend on being open.
    ///
    /// The dock is as wide as the widest content needs, and a section closed at start must not
    /// let it be narrower than the same section open.
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    /// @brief The section was opened or closed.
    void toggled(bool open);

private:
    /// @brief Shows the arrow and the title on the row.
    void updateHeader();

    QString id_;
    QString title_;
    QPushButton* header_ = nullptr;
    QWidget* body_ = nullptr;
    bool open_ = true;
};

} // namespace arraw::app
