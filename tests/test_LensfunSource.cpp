#include "pipeline/LensCorrection.h"
#include "pipeline/LensfunSource.h"
#include "pipeline/LoadResult.h"
#include "pipeline/RawProcessor.h"
#include "TestApp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

using Catch::Matchers::WithinAbs;

static QString miniDbDir() {
    return QString(ARRAW_FIXTURE_DIR) + "/lensfun-mini";
}

static LensQuery miniQuery() {
    LensQuery q;
    q.cameraMaker = "ArrawTest";
    q.cameraModel = "TestCam";
    q.lensModel = "TestLens 50mm F2.8";
    q.focal = 50.0f;
    q.aperture = 2.8f;
    q.distance = 1000.0f;
    q.width = 600;
    q.height = 400;
    return q;
}

static QString copyMiniDbToBundledDir() {
    testApp();
    const QString dbDir = QCoreApplication::applicationDirPath() + "/lensfun/db";
    REQUIRE(QDir().mkpath(dbDir));
    const QString dest = dbDir + "/mini.xml";
    QFile::remove(dest);
    REQUIRE(QFile::copy(miniDbDir() + "/mini.xml", dest));
    return dbDir;
}

TEST_CASE("resolveLensfunModel matches the fixture lens and fills the curves", "[lensfun]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    const auto model = resolveLensfunModel(miniDbDir(), miniQuery());
    REQUIRE(model.has_value());
    REQUIRE(model->hasDistortion);
    REQUIRE(model->hasTCA);
    REQUIRE(model->hasVignetting);
    REQUIRE(model->source == LensCorrectionModel::Source::Lensfun);
    REQUIRE(model->lensName.contains("TestLens"));

    constexpr int last = RadialCurve::N - 1;

    // Vignetting: identity at the centre, brightening (> 1) at the corner.
    REQUIRE_THAT(model->vignette.sample(0.0f), WithinAbs(1.0, 0.05));
    REQUIRE(model->vignette.lut[last] > 1.05f);

    // Distortion: a real radial scale that departs from identity toward the edge.
    REQUIRE(std::abs(model->distortion.lut[last] - 1.0f) > 1e-3f);

    // TCA: red and blue scale apart from each other near the edge, both close to 1.
    REQUIRE(std::abs(model->tcaR.lut[last] - model->tcaB.lut[last]) > 1e-5f);
    REQUIRE_THAT(model->tcaR.lut[last], WithinAbs(1.0, 0.02));
}

TEST_CASE("resolveLensfunModel returns nullopt when no lens matches", "[lensfun]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    LensQuery q = miniQuery();
    q.lensModel = "Totally Unknown Glass 123mm";
    REQUIRE_FALSE(resolveLensfunModel(miniDbDir(), q).has_value());
}

TEST_CASE("resolveLensfunModel returns nullopt for a bad database path", "[lensfun]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    REQUIRE_FALSE(resolveLensfunModel(miniDbDir() + "/does-not-exist", miniQuery()).has_value());
}

TEST_CASE("resolveLensfunModel returns nullopt for an empty query", "[lensfun]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    REQUIRE_FALSE(resolveLensfunModel(miniDbDir(), LensQuery{}).has_value());
}

TEST_CASE("resolveLensfunModel discovers a bundled database next to the executable", "[lensfun]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    const QString dbDir = copyMiniDbToBundledDir();
    INFO("bundled lensfun DB: " << dbDir.toStdString());

    const auto model = resolveLensfunModel(QString(), miniQuery());
    REQUIRE(model.has_value());
    REQUIRE(model->source == LensCorrectionModel::Source::Lensfun);
    REQUIRE(model->lensName.contains("TestLens"));
}

// Hidden ([.]) — needs the system lensfun database and is machine-specific. Run
// explicitly: arraw_tests "[.realdb]". Validates the real reference rig resolves.
TEST_CASE("system lensfun DB corrects the reference Sony rig", "[.][realdb]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");

    LensQuery q;
    q.cameraMaker = "SONY";
    q.cameraModel = "ILCE-6700";
    q.lensModel = "56mm F1.4 DC DN | Contemporary 018";
    q.focal = 56.0f;
    q.aperture = 3.5f;
    q.distance = 1000.0f;
    q.width = 6192;
    q.height = 4128;

    const auto model = resolveLensfunModel("/usr/share/lensfun/version_1", q);
    REQUIRE(model.has_value());
    INFO("matched lens: " << model->lensName.toStdString());
    REQUIRE(model->lensName.contains("56mm"));
    REQUIRE((model->hasDistortion || model->hasVignetting || model->hasTCA));
}

// Hidden ([.]) end-to-end: decode a real RAW through RawProcessor, confirm the lens
// profile resolves at load and that correction visibly changes the buffer.
// Run explicitly: arraw_tests "[.realfile]".
TEST_CASE("RawProcessor resolves and applies a lens profile end-to-end", "[.][realfile]") {
    if (!lensfunAvailable())
        SKIP("built without lensfun");
    const QString raw = "/home/jan/Documents/foto/05-vlkancice/_A678886.ARW";
    if (!QFileInfo::exists(raw))
        SKIP("reference RAW not present");

    const LoadResult result = RawProcessor::load(raw);
    REQUIRE(result.error.isEmpty());
    REQUIRE(result.fullRes.valid());

    INFO("resolved lens: " << result.lensModel.lensName.toStdString());
    REQUIRE(
        (result.lensModel.hasDistortion || result.lensModel.hasVignetting
         || result.lensModel.hasTCA));

    const LensCorrectionToggles all{.distortion = true, .vignetting = true, .ca = true};
    const ImageBuffer corrected = applyLensCorrection(result.fullRes, result.lensModel, all);
    REQUIRE(corrected.data != result.fullRes.data); // correction actually changed pixels
}
