# The Python extension module _arraw and, beside it, the pure-Python package.
find_package(Python 3.12 COMPONENTS Interpreter Development.Module REQUIRED)

# nanobind ships its CMake files inside the Python package, so ask the
# interpreter that will load the module where they are.
execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_VARIABLE ARRAW_NANOBIND_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ARRAW_NANOBIND_RESULT)
if(NOT ARRAW_NANOBIND_RESULT EQUAL 0)
    message(FATAL_ERROR "nanobind is not installed for ${Python_EXECUTABLE}; "
            "run `uv sync` or `pip install nanobind`.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${ARRAW_NANOBIND_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_arraw NB_STATIC Module.cpp BindImage.cpp BindExif.cpp BindSettings.cpp BindPhoto.cpp BindShots.cpp BindSession.cpp)
arraw_enable_warnings(_arraw)
target_link_libraries(_arraw PRIVATE arraw)
target_compile_definitions(_arraw PRIVATE ARRAW_VERSION="${PROJECT_VERSION}")

if(SKBUILD)
    # scikit-build-core packages the wheel from the install tree; the .py files
    # come from wheel.packages in pyproject.toml.
    install(TARGETS _arraw LIBRARY DESTINATION arraw)
else()
    # In the dev tree the package is assembled in <build>/python/arraw, so that
    # PYTHONPATH=<build>/python is enough to import it.
    set_target_properties(_arraw PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python/arraw")
    file(GLOB ARRAW_PYTHON_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/arraw/*.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/arraw/py.typed")
    set(ARRAW_PYTHON_COPIES)
    foreach(file IN LISTS ARRAW_PYTHON_FILES)
        get_filename_component(name "${file}" NAME)
        set(copy "${CMAKE_BINARY_DIR}/python/arraw/${name}")
        add_custom_command(OUTPUT "${copy}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "${copy}"
                DEPENDS "${file}"
                VERBATIM)
        list(APPEND ARRAW_PYTHON_COPIES "${copy}")
    endforeach()
    add_custom_target(arraw-python-files ALL DEPENDS ${ARRAW_PYTHON_COPIES})
    add_dependencies(_arraw arraw-python-files)

    # Not part of the default build: `cmake --build <tree> --target arraw-stubs`
    # regenerates the committed type stubs (src/python/arraw/_arraw.pyi, which the
    # wheel packages; the dev tree has no use for them) after the bindings change.
    nanobind_add_stub(arraw-stubs
            MODULE arraw._arraw
            OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/arraw/_arraw.pyi"
            PATTERN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/arraw/stubs.pattern"
            PYTHON_PATH "${CMAKE_BINARY_DIR}/python"
            DEPENDS _arraw arraw-python-files
            EXCLUDE_FROM_ALL)
endif()
