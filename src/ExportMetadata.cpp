#include "ExportMetadata.h"

#ifdef ARRAW_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif

#include <utility>

namespace {

#ifdef ARRAW_HAS_EXIV2
std::string utf8(const QString& value) {
    return value.toUtf8().toStdString();
}

void setLangAlt(Exiv2::XmpData& xmp, const std::string& key, const QString& value) {
    if (value.isEmpty())
        return;
    xmp[key] = "lang=\"x-default\" " + utf8(value);
}

void setXmpArray(
    Exiv2::XmpData& xmp, const std::string& key, const QStringList& values, Exiv2::TypeId type) {
    if (values.isEmpty())
        return;
    Exiv2::XmpArrayValue array(type);
    for (const QString& value : values) {
        if (!value.isEmpty())
            array.read(utf8(value));
    }
    if (array.count() > 0)
        xmp[key].setValue(&array);
}

void applyDescriptiveXmp(Exiv2::XmpData& xmp, const UserMetadata& metadata) {
    setLangAlt(xmp, "Xmp.dc.title", metadata.title);
    setLangAlt(xmp, "Xmp.dc.description", metadata.caption);
    setXmpArray(xmp, "Xmp.dc.subject", metadata.keywords, Exiv2::xmpBag);
    setXmpArray(xmp, "Xmp.dc.creator", QStringList{metadata.creator}, Exiv2::xmpSeq);
    setLangAlt(xmp, "Xmp.dc.rights", metadata.copyright);

    if (metadata.rating != 0)
        xmp["Xmp.xmp.Rating"] = std::to_string(metadata.rating);
    if (metadata.label != ColourLabel::None)
        xmp["Xmp.xmp.Label"] = utf8(colourLabelToString(metadata.label));
}

template<typename Predicate>
void eraseExifIf(Exiv2::ExifData& exif, Predicate predicate) {
    for (auto it = exif.begin(); it != exif.end();) {
        if (predicate(*it))
            it = exif.erase(it);
        else
            ++it;
    }
}

bool isGpsTag(const Exiv2::Exifdatum& datum) {
    return datum.groupName() == "GPSInfo";
}

bool isStaleDimensionTag(const Exiv2::Exifdatum& datum) {
    const std::string key = datum.key();
    return key == "Exif.Image.ImageWidth" || key == "Exif.Image.ImageLength"
           || key == "Exif.Photo.PixelXDimension" || key == "Exif.Photo.PixelYDimension";
}

void applySourceExif(
    Exiv2::Image& output, const QString& sourcePath, const ExportMetadataSelection& selection) {
    auto source = Exiv2::ImageFactory::open(utf8(sourcePath));
    if (!source)
        throw Exiv2::Error(Exiv2::ErrorCode::kerInputDataReadFailed);

    source->readMetadata();
    Exiv2::ExifData exif = source->exifData();
    if (!selection.includeCaptureInfo)
        eraseExifIf(exif, [](const Exiv2::Exifdatum& datum) { return !isGpsTag(datum); });
    if (!selection.includeLocation)
        eraseExifIf(exif, isGpsTag);

    if (selection.includeCaptureInfo) {
        eraseExifIf(exif, isStaleDimensionTag);
        Exiv2::ExifThumb(exif).erase();
        exif["Exif.Image.Orientation"] = uint16_t(1);
        exif["Exif.Image.Software"] = "arraw";
    }
    output.setExifData(exif);
}
#endif

} // namespace

ExportMetadataResult embedExportMetadata(
    const QString& outputPath,
    const QString& sourcePath,
    const UserMetadata& metadata,
    const ExportMetadataSelection& selection) {
#ifndef ARRAW_HAS_EXIV2
    Q_UNUSED(outputPath)
    Q_UNUSED(sourcePath)
    Q_UNUSED(metadata)
    Q_UNUSED(selection)
    return {ExportMetadataStatus::SkippedNoBackend, {}};
#else
    try {
        auto output = Exiv2::ImageFactory::open(utf8(outputPath));
        if (!output)
            return {ExportMetadataStatus::Failed, QStringLiteral("exiv2 could not open output")};

        output->readMetadata();
        if (selection.includeCaptureInfo || selection.includeLocation)
            applySourceExif(*output, sourcePath, selection);
        if (selection.includeDescriptive)
            applyDescriptiveXmp(output->xmpData(), metadata);
        output->writeMetadata();
        return {ExportMetadataStatus::Embedded, {}};
    } catch (const Exiv2::Error& error) {
        return {ExportMetadataStatus::Failed, QString::fromStdString(error.what())};
    } catch (const std::exception& error) {
        return {ExportMetadataStatus::Failed, QString::fromStdString(error.what())};
    }
#endif
}
