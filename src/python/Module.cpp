// Python.h (pulled in by nanobind) must come before any standard header, so this
// file deliberately breaks the CLAUDE.md include grouping.
// clang-format off
#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/string.h>
// clang-format on

#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImageOrientation.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace nb = nanobind;
using namespace nb::literals;

// No QCoreApplication is created here. Checked empirically (Qt 6.10, Linux
// system Qt, uv-managed CPython 3.14): with none in the process, QImageReader
// still finds the JPEG and TIFF plugins, because their search path comes from
// QLibraryInfo, and read_metadata and load succeed on a JPEG and a TIFF that
// the CLI exported. Writing was not checked; revisit when save is bound, and
// if a Qt install needs the application instance, create it lazily and only
// when QCoreApplication::instance() is null, so a PySide host keeps its own.
NB_MODULE(_arraw, m) {
    m.doc() = "Python bindings for the arraw RAW processing engine.";
    m.attr("__version__") = ARRAW_VERSION;

    nb::class_<arraw::ImageSize>(m, "ImageSize", "Pixel dimensions of an image.")
        .def(nb::init<std::uint32_t, std::uint32_t>(), "width"_a, "height"_a)
        .def_ro("width", &arraw::ImageSize::width)
        .def_ro("height", &arraw::ImageSize::height)
        .def("__repr__",
             [](const arraw::ImageSize& size) {
                 return "ImageSize(width=" + std::to_string(size.width) +
                        ", height=" + std::to_string(size.height) + ")";
             })
        .def(nb::self == nb::self)
        .def("__hash__", [](const arraw::ImageSize& size) {
            return (std::uint64_t{size.width} << 32) | size.height;
        });

    nb::enum_<arraw::ImageOrientation>(m, "ImageOrientation",
                                       "Source orientation, the eight EXIF values.")
        .value("NORMAL", arraw::ImageOrientation::Normal)
        .value("MIRROR_HORIZONTAL", arraw::ImageOrientation::MirrorHorizontal)
        .value("ROTATE_180", arraw::ImageOrientation::Rotate180)
        .value("MIRROR_VERTICAL", arraw::ImageOrientation::MirrorVertical)
        .value("TRANSPOSE", arraw::ImageOrientation::Transpose)
        .value("ROTATE_90", arraw::ImageOrientation::Rotate90)
        .value("TRANSVERSE", arraw::ImageOrientation::Transverse)
        .value("ROTATE_270", arraw::ImageOrientation::Rotate270);

    nb::class_<arraw::ImageMetadata>(m, "ImageMetadata", "What a photograph declares about itself.")
        .def_ro("size", &arraw::ImageMetadata::size)
        .def_ro("orientation", &arraw::ImageMetadata::orientation);

    nb::class_<arraw::ImageBuffer>(m, "ImageBuffer", "Decoded pixels.")
        .def_prop_ro("size", &arraw::ImageBuffer::size);

    m.def(
        "read_metadata",
        [](const std::filesystem::path& path) { return arraw::readImageMetadata(path); }, "path"_a,
        "Read what a file declares about itself, without decoding its pixels.");

    m.def(
        "load", [](const std::filesystem::path& path) { return arraw::loadImage(path); }, "path"_a,
        nb::call_guard<nb::gil_scoped_release>(), "Decode an image file into a buffer.");
}
