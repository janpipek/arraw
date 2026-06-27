#include "develop/LocalAdjustment.h"
#include "ui/HistoryPanel.h"
#include "ui/LocalAdjustmentPanel.h"
#include "TestApp.h"

#include <catch2/catch_test_macros.hpp>

#include <QListWidget>
#include <QUndoCommand>
#include <QUndoStack>

namespace {
// Stand-in for MainWindow's LocalAdjustmentCommand: it labels itself exactly the
// same way (localChangeLabel) but applies nothing — there is no DevelopSession in
// this widget-level test.
class MaskCmd : public QUndoCommand {
public:
    MaskCmd(std::vector<LocalAdjustment> b, std::vector<LocalAdjustment> a)
        : before(std::move(b)), after(std::move(a)) {
        setText(localChangeLabel(before, after));
    }
    void undo() override {}
    void redo() override {}

private:
    std::vector<LocalAdjustment> before;
    std::vector<LocalAdjustment> after;
};
} // namespace

// Regression: adding a mask must produce a distinct, descriptive History row —
// not vanish, and not read the generic "Adjust Local". Reproduces MainWindow's
// committed -> push(LocalAdjustmentCommand) wiring end to end.
TEST_CASE("adding a mask shows a descriptive row in History", "[historylocal]") {
    testApp();
    QUndoStack stack;
    LocalAdjustmentPanel panel;
    HistoryPanel history(&stack);

    QObject::connect(&panel, &LocalAdjustmentPanel::committed,
        [&](const std::vector<LocalAdjustment>& b, const std::vector<LocalAdjustment>& a) {
            stack.push(new MaskCmd(b, a));
        });

    panel.addLinearMask();

    REQUIRE(stack.count() == 1);
    CHECK(stack.text(0) == "Add Linear Mask");

    auto* list = history.findChild<QListWidget*>("historyList");
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 2); // the edit, plus the "Load" base row
    CHECK(list->item(0)->text() == "Add Linear Mask");
    CHECK(list->item(1)->text() == "Load");
}
