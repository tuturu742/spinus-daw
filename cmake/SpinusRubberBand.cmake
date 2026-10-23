# Rubber Band (GPL-2.0-or-later; audio clips' pitch and stretch, Spinus Tune's rendering) as the
# target spinus_rubberband.
#
# Linux: the system's library (pkg-config). Windows has no system copy, so Rubber Band's own single-file
# build (single/RubberBandSingle.cpp, its built-in FFT and resampler) is compiled from a pinned release.
#
# Tracktion includes it as <rubberband/rubberband/RubberBandStretcher.h>, as if it were a source checkout;
# Spinus's own code includes <rubberband/RubberBandStretcher.h>. spinus_rubberband_tracktion_include is the
# folder that makes Tracktion's form work.

if (TARGET spinus_rubberband)
    return()
endif()

add_library (spinus_rubberband INTERFACE)

if (WIN32)
    include (FetchContent)
    # The checkout's folder must be named rubberband for Tracktion's include form.
    FetchContent_Declare (spinus_rubberband_source
        URL https://breakfastquay.com/files/releases/rubberband-4.0.0.tar.bz2
        URL_HASH SHA256=af050313ee63bc18b35b2e064e5dce05b276aaf6d1aa2b8a82ced1fe2f8028e9
        SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/rubberband-checkout/rubberband")
    FetchContent_MakeAvailable (spinus_rubberband_source)
    set (rubberbandRoot "${CMAKE_BINARY_DIR}/_deps/rubberband-checkout/rubberband")

    add_library (spinus_rubberband_single STATIC "${rubberbandRoot}/single/RubberBandSingle.cpp")
    target_include_directories (spinus_rubberband_single PUBLIC "${rubberbandRoot}")
    target_compile_definitions (spinus_rubberband_single PUBLIC _USE_MATH_DEFINES NOMINMAX)
    target_link_libraries (spinus_rubberband INTERFACE spinus_rubberband_single)
    set (spinus_rubberband_tracktion_include "${CMAKE_BINARY_DIR}/_deps/rubberband-checkout")
else()
    find_package (PkgConfig REQUIRED)
    pkg_check_modules (SPINUS_RUBBERBAND REQUIRED IMPORTED_TARGET rubberband>=3.0)
    target_link_libraries (spinus_rubberband INTERFACE PkgConfig::SPINUS_RUBBERBAND)

    # A generated header forwards Tracktion's form to the installed one.
    set (spinus_rubberband_tracktion_include "${CMAKE_BINARY_DIR}/generated/rubberband")
    file (CONFIGURE OUTPUT "${spinus_rubberband_tracktion_include}/rubberband/rubberband/RubberBandStretcher.h"
          CONTENT "#pragma once\n#include <rubberband/RubberBandStretcher.h>\n" @ONLY)
endif()
