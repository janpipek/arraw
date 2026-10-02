#include "DevelopPanel.h"

#include "SettingPresentation.h"
#include "SettingSlider.h"

#include <SettingDescriptors.h>

#include <QGroupBox>
#include <QVBoxLayout>

#include <type_traits>

namespace arraw::app {

DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent) {
    auto* group = new QGroupBox(tr("Tone"), this);
    auto* groupLayout = new QVBoxLayout(group);
    for (const std::string_view key : toneKeys()) {
        auto* row = new SettingSlider(key, group);
        groupLayout->addWidget(row);
        rows_.push_back(row);
        connect(row, &SettingSlider::editStarted, this, [this, row] {
            // One edit at a time: another row's pending edit ends before this one begins.
            for (SettingSlider* other : rows_) {
                if (other != row) {
                    other->finishPendingEdit();
                }
            }
            emit editStarted();
        });
        connect(row, &SettingSlider::editFinished, this, &DevelopPanel::editFinished);
        connect(row, &SettingSlider::valueEdited, this,
                [this, row](double value) { applyEdit(*row, value); });
    }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(group);
    layout->addStretch(1);
    showState(shown_, true);
}

void DevelopPanel::showState(const DevelopState& state, bool raw) {
    shown_ = state;
    for (SettingSlider* row : rows_) {
        const FieldDescriptor& descriptor = *findDescriptor(row->key());
        row->setVisible(raw || descriptor.applies != Applicability::RawOnly);
        row->setValue(visitField(descriptor, shown_.settings, [](const auto& field) -> double {
            if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(field)>>) {
                return static_cast<double>(field);
            } else {
                return 0.0;
            }
        }));
    }
}

void DevelopPanel::finishPendingEdit() {
    for (SettingSlider* row : rows_) {
        row->finishPendingEdit();
    }
}

void DevelopPanel::applyEdit(const SettingSlider& row, double value) {
    DevelopState next = shown_;
    visitField(*findDescriptor(row.key()), next.settings, [value](auto& field) {
        using Field = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<Field, float> || std::is_same_v<Field, double>) {
            field = static_cast<Field>(value);
        }
    });
    emit stateEdited(next);
}

} // namespace arraw::app
