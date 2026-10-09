#include "PyBindings.h"

// No QCoreApplication is created here. Checked empirically (Qt 6.10, Linux
// system Qt, uv-managed CPython 3.14): with none in the process, QImageReader
// and QImageWriter still find the JPEG and TIFF plugins, because their search
// path comes from QLibraryInfo. read_metadata, load and save all succeed on
// JPEG, PNG and TIFF (8 and 16 bit), so nothing is created. Should a Qt install
// ever need the application instance, create it lazily and only when
// QCoreApplication::instance() is null, so a PySide host keeps its own.
NB_MODULE(_arraw, m) {
    m.doc() = "Python bindings for the arraw RAW processing engine.";
    m.attr("__version__") = ARRAW_VERSION;

    // Order matters: later classes name earlier ones in their signatures.
    arraw::python::bindImage(m);
    arraw::python::bindExif(m);
    arraw::python::bindSettings(m);
    arraw::python::bindPhoto(m);
    arraw::python::bindShots(m);
    arraw::python::bindSession(m);
}
