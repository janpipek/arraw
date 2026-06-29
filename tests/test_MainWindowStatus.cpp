#include "MainWindowStatus.h"
#include "core/ImageBuffer.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("loadedImageStatusText includes filename and dimensions", "[status]") {
    ImageBuffer fullRes;
    fullRes.width = 4000;
    fullRes.height = 3000;

    CHECK(
        loadedImageStatusText("/photos/IMG_0001.CR3", fullRes, DevelopSession::SidecarState::Loaded)
        == QStringLiteral("IMG_0001.CR3  \u2014  4000 \u00d7 3000"));
}

TEST_CASE("loadedImageStatusText explains malformed sidecars", "[status]") {
    ImageBuffer fullRes;
    fullRes.width = 4000;
    fullRes.height = 3000;

    CHECK(
        loadedImageStatusText("/photos/IMG_0001.CR3", fullRes, DevelopSession::SidecarState::ParseError)
        == QStringLiteral(
            "IMG_0001.CR3  \u2014  4000 \u00d7 3000  \u2014  Sidecar unreadable; defaults "
            "applied"));
}

TEST_CASE("batchExportStatusText reports completed and total files", "[status]") {
    CHECK(batchExportStatusText(2, 5) == "Exported 2 of 5 file(s)");
}
