#include "TestApp.h"
#include "develop/DevelopGroup.h"
#include "ui/GroupChecklistDialog.h"

#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>

namespace {

QCheckBox* box(const GroupChecklistDialog& d, DevelopGroup g) {
    return d.findChild<QCheckBox*>(developGroupKey(g));
}

GroupSelection groups(std::initializer_list<DevelopGroup> gs) {
    GroupSelection s;
    for (DevelopGroup g : gs)
        s.set(static_cast<size_t>(g));
    return s;
}

} // namespace

TEST_CASE("Checklist offers a checkbox only for available groups", "[groupchecklist]") {
    testApp();
    GroupChecklistDialog
        d("Copy Settings",
          groups({DevelopGroup::Tone, DevelopGroup::Colour}),
          groups({DevelopGroup::Tone, DevelopGroup::Colour}));

    CHECK(box(d, DevelopGroup::Tone) != nullptr);
    CHECK(box(d, DevelopGroup::Colour) != nullptr);
    CHECK(box(d, DevelopGroup::WhiteBalance) == nullptr);
    CHECK(box(d, DevelopGroup::Geometry) == nullptr);
}

TEST_CASE("Preselected groups start checked, others unchecked", "[groupchecklist]") {
    testApp();
    GroupChecklistDialog d("Copy Settings", allGroups(), groups({DevelopGroup::Tone}));

    CHECK(box(d, DevelopGroup::Tone)->isChecked());
    CHECK_FALSE(box(d, DevelopGroup::Colour)->isChecked());
    CHECK_FALSE(box(d, DevelopGroup::WhiteBalance)->isChecked());
}

TEST_CASE("selectedGroups reflects the ticked boxes", "[groupchecklist]") {
    testApp();
    GroupChecklistDialog d("Copy Settings", allGroups(), allGroups());
    REQUIRE(d.selectedGroups() == allGroups());

    box(d, DevelopGroup::Geometry)->setChecked(false);
    box(d, DevelopGroup::Hsl)->setChecked(false);

    GroupSelection expected = allGroups();
    expected.reset(static_cast<size_t>(DevelopGroup::Geometry));
    expected.reset(static_cast<size_t>(DevelopGroup::Hsl));
    CHECK(d.selectedGroups() == expected);
}

TEST_CASE("Paste can only narrow: result never exceeds the available set", "[groupchecklist]") {
    testApp();
    // Mimic paste bounded by a clipboard that copied only Tone + Colour, even if
    // the caller naively preselects everything.
    const GroupSelection copied = groups({DevelopGroup::Tone, DevelopGroup::Colour});
    GroupChecklistDialog d("Paste Settings", copied, allGroups());

    // No checkbox exists outside the copied set, so the selection is bounded.
    CHECK((d.selectedGroups() & ~copied).none());
    CHECK(d.selectedGroups() == copied); // preselect intersected down to available
}
