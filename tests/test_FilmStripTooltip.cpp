#include "core/ImageMetadata.h"
#include "ui/FilmStripTooltip.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("tooltipText orders cached EXIF rows and omits absent lines", "[filmstrip][tooltip]") {
    ImageMetadata meta;
    meta.rows.append(qMakePair(QString("Date"), QString("2026-02-03 10:11:12")));
    meta.rows.append(qMakePair(QString("Make"), QString("Nikon")));
    meta.rows.append(qMakePair(QString("Model"), QString("Z 8")));
    meta.rows.append(qMakePair(QString("Lens"), QString("NIKKOR Z 24-70mm f/2.8 S")));
    meta.rows.append(qMakePair(QString("ISO"), QString("800")));
    meta.rows.append(qMakePair(QString("Shutter"), QString("1/250 s")));
    meta.rows.append(qMakePair(QString("Aperture"), QString("f/4.0")));
    meta.rows.append(qMakePair(QString("Focal length"), QString("35 mm")));
    meta.rows.append(qMakePair(QString("Active area"), QString("8256 × 5504")));

    REQUIRE(
        tooltipText("DSC_0001.NEF", meta)
        == QString(
            "DSC_0001.NEF\n"
            "2026-02-03 10:11:12\n"
            "Nikon Z 8\n"
            "NIKKOR Z 24-70mm f/2.8 S\n"
            "ISO 800  1/250 s  f/4.0  35 mm\n"
            "8256 × 5504"));

    ImageMetadata sparse;
    sparse.rows.append(qMakePair(QString("Model"), QString("X100VI")));
    sparse.rows.append(qMakePair(QString("ISO"), QString("200")));

    REQUIRE(tooltipText("frame.raf", sparse) == QString("frame.raf\nX100VI\nISO 200"));
}
