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

# Diagnostic mode for the atari_vbxe_bringup demo: 0 = colour bars, 1 = solid
# fill, 2 = VRAM round-trip probe. Passed to the compiler as -DVBXE_BRINGUP_MODE.
set(EDGE_VBXE_BRINGUP_MODE 0
    CACHE STRING
    "atari_vbxe_bringup diagnostic mode (0=bars, 1=solid, 2=probe)")

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

    # Scroll validation demo — separate, independent .xex (no charset header).
    add_executable(atari_scroll_test demo/atari_scroll_test.cpp)
    target_compile_options(atari_scroll_test PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(atari_scroll_test PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_scroll_test PROPERTIES OUTPUT_NAME "atari_scroll_test.xex")
    target_link_options(atari_scroll_test PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_scroll_test>/atari_scroll_test.map")

    # VBXE Phase 4a bring-up smoke test — independent .xex (no charset header).
    add_executable(atari_vbxe_bringup demo/vbxe_bringup.cpp)
    target_compile_options(atari_vbxe_bringup PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_compile_definitions(atari_vbxe_bringup PRIVATE
        VBXE_BRINGUP_MODE=${EDGE_VBXE_BRINGUP_MODE})
    target_include_directories(atari_vbxe_bringup PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_vbxe_bringup PROPERTIES OUTPUT_NAME "atari_vbxe_bringup.xex")
    target_link_options(atari_vbxe_bringup PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_vbxe_bringup>/atari_vbxe_bringup.map")

    # VBXE bitmap-drawing (gfx) demo — independent .xex.
    add_executable(atari_vbxe_gfx demo/vbxe_gfx.cpp)
    target_compile_options(atari_vbxe_gfx PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(atari_vbxe_gfx PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_vbxe_gfx PROPERTIES OUTPUT_NAME "atari_vbxe_gfx.xex")
    target_link_options(atari_vbxe_gfx PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_vbxe_gfx>/atari_vbxe_gfx.map")

    # VBXE 80-column text-mode demo — independent .xex.
    add_executable(atari_vbxe_text demo/vbxe_text.cpp)
    target_compile_options(atari_vbxe_text PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(atari_vbxe_text PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_vbxe_text PROPERTIES OUTPUT_NAME "atari_vbxe_text.xex")
    target_link_options(atari_vbxe_text PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_vbxe_text>/atari_vbxe_text.map")

    # VBXE sprites-over-bitmap demo — independent .xex.
    add_executable(atari_vbxe_sprites demo/vbxe_sprites_over_bitmap.cpp)
    target_compile_options(atari_vbxe_sprites PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(atari_vbxe_sprites PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(atari_vbxe_sprites PROPERTIES OUTPUT_NAME "atari_vbxe_sprites.xex")
    target_link_options(atari_vbxe_sprites PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:atari_vbxe_sprites>/atari_vbxe_sprites.map")
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
    # The header-only engine is consumed by #include, so the custom-command demo
    # builds (unlike compiled targets) don't auto-track header changes. Glob the
    # engine headers and add them to each demo's DEPENDS so editing any engine
    # header re-links the affected .xex.
    file(GLOB_RECURSE EDGE_ENGINE_HEADERS CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/engine/*.h)

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
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${HW_TEST_MAP}
        COMMENT "Building Atari XEX demo (atari_hw_test.xex)"
        VERBATIM)
    add_custom_target(atari_hw_test DEPENDS ${HW_TEST_XEX})

    # ── Scroll validation demo (atari_scroll_test.xex) ──────────────────
    # Same mos-atari8-dos toolchain, built explicitly and independently of the
    # hw_test demo and the mos-sim test suite:
    #
    #   cmake --build build --target atari_scroll_test   # -> build/atari_scroll_test.xex
    set(SCROLL_TEST_XEX ${CMAKE_BINARY_DIR}/atari_scroll_test.xex)
    set(SCROLL_TEST_MAP ${CMAKE_BINARY_DIR}/atari_scroll_test.map)
    add_custom_command(
        OUTPUT ${SCROLL_TEST_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/atari_scroll_test.cpp
                -o ${SCROLL_TEST_XEX}
                -Wl,--Map=${SCROLL_TEST_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/atari_scroll_test.cpp
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${SCROLL_TEST_MAP}
        COMMENT "Building Atari XEX demo (atari_scroll_test.xex)"
        VERBATIM)
    add_custom_target(atari_scroll_test DEPENDS ${SCROLL_TEST_XEX})

    # ── VBXE Phase 4a bring-up smoke test (atari_vbxe_bringup.xex) ───────
    #   cmake -B build -DEDGE_VBXE_BRINGUP_MODE=2 ...   # 0=bars, 1=solid, 2=probe
    #   cmake --build build --target atari_vbxe_bringup  # -> build/atari_vbxe_bringup.xex
    set(VBXE_BRINGUP_XEX ${CMAKE_BINARY_DIR}/atari_vbxe_bringup.xex)
    set(VBXE_BRINGUP_MAP ${CMAKE_BINARY_DIR}/atari_vbxe_bringup.map)
    # The Make generator doesn't re-run a custom command when only its command
    # line (the -DVBXE_BRINGUP_MODE value) changes. file(GENERATE) rewrites this
    # stamp only when the mode actually changes, so depending on it rebuilds the
    # .xex exactly when needed.
    set(VBXE_BRINGUP_MODE_STAMP ${CMAKE_BINARY_DIR}/atari_vbxe_bringup_mode.stamp)
    file(GENERATE OUTPUT ${VBXE_BRINGUP_MODE_STAMP}
         CONTENT "${EDGE_VBXE_BRINGUP_MODE}")
    add_custom_command(
        OUTPUT ${VBXE_BRINGUP_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -DVBXE_BRINGUP_MODE=${EDGE_VBXE_BRINGUP_MODE}
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/vbxe_bringup.cpp
                -o ${VBXE_BRINGUP_XEX}
                -Wl,--Map=${VBXE_BRINGUP_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/vbxe_bringup.cpp
                ${EDGE_ENGINE_HEADERS}
                ${VBXE_BRINGUP_MODE_STAMP}
        BYPRODUCTS ${VBXE_BRINGUP_MAP}
        COMMENT "Building Atari XEX demo (atari_vbxe_bringup.xex, mode ${EDGE_VBXE_BRINGUP_MODE})"
        VERBATIM)
    add_custom_target(atari_vbxe_bringup DEPENDS ${VBXE_BRINGUP_XEX})

    # ── VBXE bitmap-drawing (gfx) demo (atari_vbxe_gfx.xex) ──────────────
    #   cmake --build build --target atari_vbxe_gfx   # -> build/atari_vbxe_gfx.xex
    set(VBXE_GFX_XEX ${CMAKE_BINARY_DIR}/atari_vbxe_gfx.xex)
    set(VBXE_GFX_MAP ${CMAKE_BINARY_DIR}/atari_vbxe_gfx.map)
    add_custom_command(
        OUTPUT ${VBXE_GFX_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/vbxe_gfx.cpp
                -o ${VBXE_GFX_XEX}
                -Wl,--Map=${VBXE_GFX_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/vbxe_gfx.cpp
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${VBXE_GFX_MAP}
        COMMENT "Building Atari XEX demo (atari_vbxe_gfx.xex)"
        VERBATIM)
    add_custom_target(atari_vbxe_gfx DEPENDS ${VBXE_GFX_XEX})

    # ── VBXE 80-column text-mode demo (atari_vbxe_text.xex) ──────────────
    #   cmake --build build --target atari_vbxe_text   # -> build/atari_vbxe_text.xex
    set(VBXE_TEXT_XEX ${CMAKE_BINARY_DIR}/atari_vbxe_text.xex)
    set(VBXE_TEXT_MAP ${CMAKE_BINARY_DIR}/atari_vbxe_text.map)
    add_custom_command(
        OUTPUT ${VBXE_TEXT_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/vbxe_text.cpp
                -o ${VBXE_TEXT_XEX}
                -Wl,--Map=${VBXE_TEXT_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/vbxe_text.cpp
                ${CMAKE_SOURCE_DIR}/demo/PREPPIEPC_FNT.h
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${VBXE_TEXT_MAP}
        COMMENT "Building Atari XEX demo (atari_vbxe_text.xex)"
        VERBATIM)
    add_custom_target(atari_vbxe_text DEPENDS ${VBXE_TEXT_XEX})

    # ── VBXE sprites-over-bitmap demo (atari_vbxe_sprites.xex) ───────────
    #   cmake --build build --target atari_vbxe_sprites  # -> build/atari_vbxe_sprites.xex
    set(VBXE_SPRITES_XEX ${CMAKE_BINARY_DIR}/atari_vbxe_sprites.xex)
    set(VBXE_SPRITES_MAP ${CMAKE_BINARY_DIR}/atari_vbxe_sprites.map)
    add_custom_command(
        OUTPUT ${VBXE_SPRITES_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/vbxe_sprites_over_bitmap.cpp
                -o ${VBXE_SPRITES_XEX}
                -Wl,--Map=${VBXE_SPRITES_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/vbxe_sprites_over_bitmap.cpp
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${VBXE_SPRITES_MAP}
        COMMENT "Building Atari XEX demo (atari_vbxe_sprites.xex)"
        VERBATIM)
    add_custom_target(atari_vbxe_sprites DEPENDS ${VBXE_SPRITES_XEX})
else()
    message(STATUS "mos-atari8-dos-clang++ not found; 'atari_hw_test'/'atari_scroll_test'/'atari_vbxe_bringup' demo targets unavailable")
endif()
