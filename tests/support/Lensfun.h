#pragma once

#include <LensDatabase.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

/// Gate for the `[lensfun]` tests, which need a build with lensfun (ADR 050).
///
/// Without it they skip, so that a development build configured with
/// `ARRAW_WITH_LENSFUN=OFF` or without the library still passes `just test`. CI sets
/// `ARRAW_REQUIRE_LENSFUN=1`, and then a build without lensfun fails them instead, as
/// `ARRAW_REQUIRE_CLI` does for the Python tests.
#define ARRAW_REQUIRE_LENSFUN_OR_SKIP()                                                            \
    do {                                                                                           \
        if (!::arraw::lensfunAvailable()) {                                                        \
            const char* required_ = std::getenv("ARRAW_REQUIRE_LENSFUN");                          \
            if (required_ != nullptr && *required_ != '\0' && *required_ != '0') {                 \
                FAIL("ARRAW_REQUIRE_LENSFUN is set, but this build has no lensfun");               \
            }                                                                                      \
            SKIP("this build has no lensfun");                                                     \
        }                                                                                          \
    } while (false)
