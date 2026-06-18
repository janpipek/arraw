#include "AdjustmentPanel.h"
#include "BatchPaste.h"
#include "XmpSidecar.h"

#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

TEST_CASE("BatchAdjustmentCommand text pluralises for multiple files", "[batchpaste]") {
    QVector<BatchPasteRecord> records(3);
    BatchAdjustmentCommand cmd(nullptr, QString{}, records);
    REQUIRE(cmd.text() == "Paste Settings (3 files)");
}

TEST_CASE("BatchAdjustmentCommand text uses singular for one file", "[batchpaste]") {
    QVector<BatchPasteRecord> records(1);
    BatchAdjustmentCommand cmd(nullptr, QString{}, records);
    REQUIRE(cmd.text() == "Paste Settings (1 file)");
}

TEST_CASE("BatchAdjustmentCommand redo writes after-state XMP for each record", "[batchpaste]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    const QString raw1 = dir.filePath("IMG_0001.CR3");
    const QString raw2 = dir.filePath("IMG_0002.CR3");

    GlobalAdjustment before1, before2;
    GlobalAdjustment after1, after2;
    after1.exposure = 1.0f;
    after2.exposure = -0.5f;

    QVector<BatchPasteRecord> records = {{raw1, before1, after1}, {raw2, before2, after2}};
    BatchAdjustmentCommand cmd(nullptr, QString{}, records);
    cmd.redo();

    CHECK(XmpSidecar::loadAdjustments(raw1).exposure == after1.exposure);
    CHECK(XmpSidecar::loadAdjustments(raw2).exposure == after2.exposure);
}

TEST_CASE("BatchAdjustmentCommand undo restores before-state XMP for each record", "[batchpaste]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    const QString raw1 = dir.filePath("IMG_0001.CR3");
    const QString raw2 = dir.filePath("IMG_0002.CR3");

    GlobalAdjustment before1, before2, after1, after2;
    before1.exposure = 0.3f;
    before2.exposure = -0.7f;
    after1.exposure = 1.5f;
    after2.exposure = 2.0f;

    QVector<BatchPasteRecord> records = {{raw1, before1, after1}, {raw2, before2, after2}};
    BatchAdjustmentCommand cmd(nullptr, QString{}, records);
    cmd.redo();
    cmd.undo();

    CHECK(XmpSidecar::loadAdjustments(raw1).exposure == before1.exposure);
    CHECK(XmpSidecar::loadAdjustments(raw2).exposure == before2.exposure);
}

TEST_CASE("BatchAdjustmentCommand redo/undo update the active file's AdjustmentPanel", "[batchpaste]") {
    testApp();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    const QString activeRaw = dir.filePath("active.CR3");
    const QString otherRaw  = dir.filePath("other.CR3");

    GlobalAdjustment activeBefore, activeAfter;
    activeBefore.exposure = 0.1f;
    activeAfter.exposure  = 2.0f;

    AdjustmentPanel panel;
    panel.setParams(activeBefore);

    QVector<BatchPasteRecord> records = {
        {activeRaw, activeBefore, activeAfter},
        {otherRaw,  GlobalAdjustment{}, GlobalAdjustment{}},
    };
    BatchAdjustmentCommand cmd(&panel, activeRaw, records);

    cmd.redo();
    CHECK(panel.params().exposure == activeAfter.exposure);

    cmd.undo();
    CHECK(panel.params().exposure == activeBefore.exposure);
}
