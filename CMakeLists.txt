cmake_minimum_required(VERSION 3.20)

# Single source of truth for the version is engine/version.h (see CHANGELOG.md).
# Parse the macros here so PROJECT_VERSION never drifts from the header.
file(STRINGS engine/version.h _edge_ver_major REGEX "EDGE_VERSION_MAJOR")
file(STRINGS engine/version.h _edge_ver_minor REGEX "EDGE_VERSION_MINOR")
file(STRINGS engine/version.h _edge_ver_patch REGEX "EDGE_VERSION_PATCH")
string(REGEX MATCH "[0-9]+" _edge_ver_major "${_edge_ver_major}")
string(REGEX MATCH "[0-9]+" _edge_ver_minor "${_edge_ver_minor}")
string(REGEX MATCH "[0-9]+" _edge_ver_patch "${_edge_ver_patch}")

project(edge
        VERSION ${_edge_ver_major}.${_edge_ver_minor}.${_edge_ver_patch}
        LANGUAGES CXX)

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
    "Binary 1K charset used by the atari_hw_test demo")
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
    add_executable(atari_hw_test demo/atari_hw_test.cpp ${USER_CHARSET_HEADER})
    target_compile_options(atari_hw_test PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(atari_hw_test PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_hw_test PROPERTIES OUTPUT_NAME "atari_hw_test.xex")
    # Emit a linker map (memory layout + final symbol/function addresses) to aid
    # hardware debugging on Altirra/Fujisan. This is a link-step byproduct only;
    # the build stays -Os and no -g/DWARF is added.
    target_link_options(atari_hw_test PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_hw_test>/atari_hw_test.map")
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
#   cmake --build build --target atari_hw_test   # -> build/atari_hw_test.xex
#
# (Alternatively, configure a dedicated build directory with
#  cmake/atari-toolchain.cmake and the add_executable target it enables.)
find_program(MOS_ATARI8_CXX mos-atari8-dos-clang++)
if(MOS_ATARI8_CXX)
    set(HW_TEST_XEX ${CMAKE_BINARY_DIR}/atari_hw_test.xex)
    # Linker map (memory layout + final symbol/function addresses) emitted
    # alongside the .xex to aid hardware debugging on Altirra/Fujisan. Link-step
    # byproduct only — the build stays -Os and no -g/DWARF is added.
    set(HW_TEST_MAP ${CMAKE_BINARY_DIR}/atari_hw_test.map)
    add_custom_command(
        OUTPUT ${HW_TEST_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/atari_hw_test.cpp
                -o ${HW_TEST_XEX}
                -Wl,--Map=${HW_TEST_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/atari_hw_test.cpp
                ${USER_CHARSET_HEADER}
        BYPRODUCTS ${HW_TEST_MAP}
        COMMENT "Building Atari XEX demo (atari_hw_test.xex)"
        VERBATIM)
    add_custom_target(atari_hw_test DEPENDS ${HW_TEST_XEX})
else()
    message(STATUS "mos-atari8-dos-clang++ not found; 'atari_hw_test' demo target unavailable")
endif()
