#include "PyBindings.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImageOrientation.h>

#include <nanobind/ndarray.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace arraw::python {

namespace {

/// @brief Converts an encoding to its Python form.
/// @return A NamedEncoding member, or a CameraNative object.
nb::object encodingToPython(const ColorEncoding& encoding) {
    return std::visit([](const auto& value) { return nb::cast(value); }, encoding);
}

/// @brief Views the samples of a buffer as an array, without copying.
/// @tparam T Sample type of the buffer.
/// @param buffer Buffer to view.
/// @param owner Python object that keeps the buffer alive.
template <class T> nb::ndarray<nb::numpy> viewSamples(ImageBuffer& buffer, nb::handle owner) {
    const std::size_t channels = channelCount(buffer.format());
    const std::size_t shape[3] = {buffer.size().height, buffer.size().width, channels};
    assert(buffer.rowStride() % sizeof(T) == 0 && "row stride must be a whole number of samples");
    // Strides are in elements; rowStride() is in bytes.
    const std::int64_t strides[3] = {static_cast<std::int64_t>(buffer.rowStride() / sizeof(T)),
                                     static_cast<std::int64_t>(channels), 1};
    return nb::ndarray<nb::numpy>(buffer.samples<T>().data(), 3, shape, owner, strides,
                                  nb::dtype<T>());
}

/// @brief Views the pixels of a Python-owned buffer as a NumPy array.
nb::ndarray<nb::numpy> pixelsOf(nb::handle self) {
    auto& buffer = nb::cast<ImageBuffer&>(self);
    switch (bytesPerChannel(buffer.format())) {
    case sizeof(std::uint8_t):
        return viewSamples<std::uint8_t>(buffer, self);
    case sizeof(std::uint16_t):
        return viewSamples<std::uint16_t>(buffer, self);
    default:
        return viewSamples<float>(buffer, self);
    }
}

} // namespace

void bindImage(nb::module_& m) {
    nb::class_<ImageSize>(m, "ImageSize", "Pixel dimensions of an image.")
        .def(nb::init<std::uint32_t, std::uint32_t>(), "width"_a, "height"_a)
        .def_ro("width", &ImageSize::width)
        .def_ro("height", &ImageSize::height)
        .def("__repr__",
             [](const ImageSize& size) {
                 return "ImageSize(width=" + std::to_string(size.width) +
                        ", height=" + std::to_string(size.height) + ")";
             })
        .def(nb::self == nb::self)
        .def("__hash__",
             [](const ImageSize& size) { return (std::uint64_t{size.width} << 32) | size.height; });

    nb::enum_<ImageOrientation>(m, "ImageOrientation", "Source orientation, the eight EXIF values.")
        .value("NORMAL", ImageOrientation::Normal)
        .value("MIRROR_HORIZONTAL", ImageOrientation::MirrorHorizontal)
        .value("ROTATE_180", ImageOrientation::Rotate180)
        .value("MIRROR_VERTICAL", ImageOrientation::MirrorVertical)
        .value("TRANSPOSE", ImageOrientation::Transpose)
        .value("ROTATE_90", ImageOrientation::Rotate90)
        .value("TRANSVERSE", ImageOrientation::Transverse)
        .value("ROTATE_270", ImageOrientation::Rotate270);

    nb::enum_<PixelFormat>(m, "PixelFormat", "Channel layout and sample type of a buffer.")
        .value("RGB_U8", PixelFormat::RgbU8)
        .value("RGBA_U8", PixelFormat::RgbaU8)
        .value("RGB_U16", PixelFormat::RgbU16)
        .value("RGBA_U16", PixelFormat::RgbaU16)
        .value("RGB_F32", PixelFormat::RgbF32)
        .value("RGBA_F32", PixelFormat::RgbaF32);

    nb::enum_<NamedEncoding>(m, "NamedEncoding",
                             "Colour encoding with fixed primaries and transfer.")
        .value("LINEAR_REC2020", NamedEncoding::LinearRec2020)
        .value("SRGB", NamedEncoding::Srgb)
        .value("DISPLAY_P3", NamedEncoding::DisplayP3)
        .value("ADOBE_RGB", NamedEncoding::AdobeRgb)
        .value("REC2020_GAMMA22", NamedEncoding::Rec2020Gamma22,
               "Rec.2020 primaries, each channel sign(v) * |v|^(1/2.2): the curve input that "
               "sample() hands back; not an output encoding.");

    nb::class_<CameraNative>(m, "CameraNative",
                             "A camera's native colour encoding; opaque in this version.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const CameraNative&) { return std::string("CameraNative(...)"); });
    m.attr("CameraNative").attr("__hash__") = nb::none();

    nb::class_<ImageMetadata>(m, "ImageMetadata", "What a photograph declares about itself.")
        .def_ro("size", &ImageMetadata::size)
        .def_ro("orientation", &ImageMetadata::orientation)
        .def_prop_ro(
            "encoding",
            [](const ImageMetadata& metadata) { return encodingToPython(metadata.encoding); },
            "NamedEncoding, or an opaque CameraNative.")
        .def(nb::self == nb::self)
        .def("__repr__", [](const ImageMetadata& metadata) {
            return "ImageMetadata(size=" + reprValue(metadata.size) +
                   ", orientation=" + reprValue(metadata.orientation) + ", encoding=" +
                   std::string(nb::repr(encodingToPython(metadata.encoding)).c_str()) + ")";
        });
    m.attr("ImageMetadata").attr("__hash__") = nb::none();

    nb::class_<ImageBuffer>(m, "ImageBuffer", "Decoded pixels; not constructible from Python.")
        .def_prop_ro("size", &ImageBuffer::size)
        .def_prop_ro("format", &ImageBuffer::format)
        .def_prop_ro("encoding",
                     [](const ImageBuffer& buffer) { return encodingToPython(buffer.encoding()); })
        .def_prop_ro("orientation", &ImageBuffer::orientation)
        .def_prop_ro("pixels", &pixelsOf,
                     "Writable NumPy view of shape (height, width, channels), without a copy; "
                     "it keeps the buffer alive.")
        .def("__repr__", [](const ImageBuffer& buffer) {
            return "ImageBuffer(size=" + reprValue(buffer.size()) +
                   ", format=" + reprValue(buffer.format()) + ")";
        });

    m.def(
        "read_metadata",
        [](const std::filesystem::path& path) {
            PythonLog log;
            return withoutGil([&] { return readImageMetadata(path, log); });
        },
        "path"_a, "Read what a file declares about itself, without decoding its pixels.");

    m.def(
        "load",
        [](const std::filesystem::path& path, bool halfSize) {
            PythonLog log;
            return withoutGil([&] { return loadImage(path, log, {.halfSize = halfSize}); });
        },
        "path"_a, "half_size"_a = false,
        "Decode an image file into a buffer. With half_size a RAW is decoded at half its width "
        "and height, without demosaicing, and the buffer is that much smaller than "
        "read_metadata says; other files ignore it.");

    m.def(
        "read_embedded_preview",
        [](const std::filesystem::path& path, std::uint32_t maxEdge) -> std::optional<ImageBuffer> {
            PythonLog log;
            return withoutGil([&] { return readEmbeddedPreview(path, maxEdge, log); });
        },
        "path"_a, "max_edge"_a,
        "Read the preview a file embeds, upright, in sRGB and no longer than max_edge on its "
        "longer edge (0 keeps the largest at its own size). None if the file has no preview, "
        "or cannot be read, which is also a message on the 'arraw' logger.");
}

} // namespace arraw::python
