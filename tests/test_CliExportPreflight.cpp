#include "cli/ExportPreflight.h"
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QTemporaryDir>

using cli::planExports;

TEST_CASE("plan derives <outdir>/<stem>.<ext> per input") {
    const auto plan = planExports({"/shoot/a.arw", "/shoot/b.arw"}, "/out",
                                  ExportOptions::Format::JPEG);
    REQUIRE(plan.error.isEmpty());
    REQUIRE(plan.items.size() == 2);
    REQUIRE(plan.items[0].output == QDir("/out").filePath("a.jpg"));
    REQUIRE(plan.items[1].output == QDir("/out").filePath("b.jpg"));
}

TEST_CASE("intra-run stem collision refuses the whole run") {
    const auto plan = planExports({"/x/IMG_0001.arw", "/y/IMG_0001.arw"}, "/out",
                                  ExportOptions::Format::JPEG);
    REQUIRE_FALSE(plan.error.isEmpty());
    REQUIRE(plan.error.contains("IMG_0001"));
    REQUIRE(plan.items.empty());
}

TEST_CASE("a directory input is a usage error") {
    QTemporaryDir dir;
    const auto plan = planExports({dir.path()}, "/out", ExportOptions::Format::JPEG);
    REQUIRE_FALSE(plan.error.isEmpty());
    REQUIRE(plan.error.contains("directory"));
}
