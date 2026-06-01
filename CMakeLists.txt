cmake_minimum_required(VERSION 3.20)

project(edge CXX)

# The engine is header-only (DECISIONS.md ADR-012); it is consumed as an
# include directory. Game code and tests include <engine/...>.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce the C++ subset from CONSTRAINTS.md: no exceptions, no RTTI.
add_compile_options(-fno-exceptions -fno-rtti -Os -Wall -Wextra)

# So that <engine/pool.h> etc. resolve from the repo root.
include_directories(${CMAKE_SOURCE_DIR})

option(EDGE_BUILD_DEMO
       "Build the Atari .xex demo via add_executable (atari8-dos toolchain only)"
       OFF)

set(EDGE_DEMO_CHARSET_BIN
    ${CMAKE_SOURCE_DIR}/demo/TEST.FNT
    CACHE FILEPATH
    "Binary 1K charset used by the hw_test demo")
set(USER_CHARSET_HEADER ${CMAKE_SOURCE_DIR}/demo/user_charset.h)
set(GENERATE_CHARSET_HEADER_SCRIPT
    ${CMAKE_SOURCE_DIR}/cmake/generate_charset_header.cmake)

add_custom_command(
    OUTPUT ${USER_CHARSET_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${EDGE_DEMO_CHARSET_BIN}
            -DOUTPUT=${USER_CHARSET_HEADER}
            -DSYMBOL=kUserCharsetBytes
            -DNAMESPACE=demo::assets
            -P ${GENERATE_CHARSET_HEADER_SCRIPT}
    DEPENDS ${EDGE_DEMO_CHARSET_BIN}
            ${GENERATE_CHARSET_HEADER_SCRIPT}
    COMMENT "Generating demo/user_charset.h from ${EDGE_DEMO_CHARSET_BIN}"
    VERBATIM)

if(EDGE_BUILD_DEMO)
    # Dedicated Atari build (configured with cmake/atari-toolchain.cmake): the
    # compiler already targets atari8-dos, so a normal executable links to a
    # loadable .xex. The test suite is not built in this configure.
    add_executable(hw_test demo/hw_test.cpp ${USER_CHARSET_HEADER})
    target_compile_options(hw_test PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(hw_test PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(hw_test PROPERTIES OUTPUT_NAME "hw_test.xex")
    return()
endif()

enable_testing()
add_subdirectory(tests)

# ── Atari XEX demo target — separate from the mos-sim test suite ──────────
#
# The main build is configured with the mos-sim toolchain (cmake/mos-sim-
# toolchain.cmake) for the unit tests. The hardware demo must instead be built
# for the Atari 8-bit DOS target, which emits a loadable .xex. Rather than force
# a second toolchain into this configure, we invoke that compiler directly via a
# custom command, so the demo coexists with the test build. It is NOT part of
# ALL (the mos-sim build never touches it); build it explicitly:
#
#   cmake --build build --target hw_test      # -> build/hw_test.xex
#
# (Alternatively, configure a dedicated build directory with
#  cmake/atari-toolchain.cmake and the add_executable target it enables.)
find_program(MOS_ATARI8_CXX mos-atari8-dos-clang++)
if(MOS_ATARI8_CXX)
    set(HW_TEST_XEX ${CMAKE_BINARY_DIR}/hw_test.xex)
    add_custom_command(
        OUTPUT ${HW_TEST_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/hw_test.cpp
                -o ${HW_TEST_XEX}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/hw_test.cpp
                ${USER_CHARSET_HEADER}
        COMMENT "Building Atari XEX demo (hw_test.xex)"
        VERBATIM)
    add_custom_target(hw_test DEPENDS ${HW_TEST_XEX})
else()
    message(STATUS "mos-atari8-dos-clang++ not found; 'hw_test' demo target unavailable")
endif()
