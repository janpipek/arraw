#include "develop/UserMetadata.h"
#include "ui/InfoPanel.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>

#include <QLineEdit>
#include <QPlainTextEdit>

TEST_CASE("InfoPanel commits descriptive User Metadata without losing culling marks", "[infopanel]") {
    testApp();
    InfoPanel panel;
    UserMetadata initial;
    initial.rating = 4;
    initial.label = ColourLabel::Blue;
    panel.setUserMetadata(initial);

    int commits = 0;
    UserMetadata committed;
    UserMetadataPresence changedFields;
    QObject::connect(
        &panel,
        &InfoPanel::userMetadataCommitted,
        &panel,
        [&](const UserMetadata& m, const UserMetadataPresence& fields) {
            ++commits;
            committed = m;
            changedFields = fields;
        });

    auto* title = panel.findChild<QLineEdit*>("titleEdit");
    auto* caption = panel.findChild<QPlainTextEdit*>("captionEdit");
    auto* keywords = panel.findChild<QLineEdit*>("keywordsEdit");
    REQUIRE(title != nullptr);
    REQUIRE(caption != nullptr);
    REQUIRE(keywords != nullptr);

    title->setText("A title");
    caption->setPlainText("A caption");
    keywords->setText("family, travel");
    panel.flushPendingEdits();

    REQUIRE(commits == 1);
    CHECK(committed.rating == 4);
    CHECK(committed.label == ColourLabel::Blue);
    CHECK(committed.title == "A title");
    CHECK(committed.caption == "A caption");
    CHECK(committed.keywords == QStringList{"family", "travel"});
    CHECK(changedFields.title);
    CHECK(changedFields.caption);
    CHECK(changedFields.keywords);
    CHECK_FALSE(changedFields.creator);
    CHECK_FALSE(changedFields.copyright);
}
