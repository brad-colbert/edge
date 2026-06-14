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
        LANGUAGES CXX C)

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

option(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB
       "Enable Atari FujiNet session adapter scaffolding against external fujinetlib-llvm"
       OFF)

set(EDGE_FUJINETLIB_ROOT
    ""
    CACHE PATH
    "Root path to fujinetlib-llvm checkout (optional if include/library are set explicitly)")
set(EDGE_FUJINETLIB_INCLUDE_DIR
    ""
    CACHE PATH
    "Path to fujinetlib include directory (contains fujinet-network.h)")
set(EDGE_FUJINETLIB_LIBRARY
    ""
    CACHE FILEPATH
    "Path to libfujinet.a")

set(EDGE_FUJINET_VALIDATE_HOST
    "192.168.1.205"
    CACHE STRING
    "Host used by demo/fujinet_session_validate runtime validation demo")
set(EDGE_FUJINET_VALIDATE_PORT
    "9000"
    CACHE STRING
    "Port used by demo/fujinet_session_validate runtime validation demo")

if(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
    if(EDGE_FUJINETLIB_ROOT)
        if(NOT EDGE_FUJINETLIB_INCLUDE_DIR)
            set(EDGE_FUJINETLIB_INCLUDE_DIR "${EDGE_FUJINETLIB_ROOT}/src/include")
        endif()
        if(NOT EDGE_FUJINETLIB_LIBRARY)
            set(EDGE_FUJINETLIB_LIBRARY "${EDGE_FUJINETLIB_ROOT}/build/libfujinet.a")
        endif()
    endif()

    if(NOT EDGE_FUJINETLIB_INCLUDE_DIR OR NOT EDGE_FUJINETLIB_LIBRARY)
        message(FATAL_ERROR
            "EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON requires EDGE_FUJINETLIB_ROOT or both EDGE_FUJINETLIB_INCLUDE_DIR and EDGE_FUJINETLIB_LIBRARY")
    endif()

    if(NOT EXISTS "${EDGE_FUJINETLIB_INCLUDE_DIR}/fujinet-network.h")
        message(FATAL_ERROR
            "Missing fujinet-network.h under EDGE_FUJINETLIB_INCLUDE_DIR='${EDGE_FUJINETLIB_INCLUDE_DIR}'")
    endif()
    if(NOT EXISTS "${EDGE_FUJINETLIB_INCLUDE_DIR}/fujinet-network-atari.h")
        message(FATAL_ERROR
            "Missing fujinet-network-atari.h under EDGE_FUJINETLIB_INCLUDE_DIR='${EDGE_FUJINETLIB_INCLUDE_DIR}'")
    endif()
    if(NOT EXISTS "${EDGE_FUJINETLIB_LIBRARY}")
        message(FATAL_ERROR
            "EDGE_FUJINETLIB_LIBRARY does not exist: '${EDGE_FUJINETLIB_LIBRARY}'")
    endif()

    add_compile_definitions(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=1)
endif()

option(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM
       "Enable Atari FujiNet realtime adapter scaffolding (EDGE-owned translated Netstream handler; no external CA65 dependency)"
       OFF)

set(EDGE_NETSTREAM_ROOT
    ""
    CACHE PATH
    "Root path to fujinet-atari-netstream checkout (optional if explicit handler/cfg/include are set)")
set(EDGE_NETSTREAM_SOURCE_DIR
    ""
    CACHE PATH
    "Path to directory containing netstream.s handler source")
set(EDGE_NETSTREAM_INCLUDE_DIR
    ""
    CACHE PATH
    "Path to netstream handler include directory (contains sio.inc, hardware.inc, etc.)")
set(EDGE_NETSTREAM_CFG
    ""
    CACHE FILEPATH
    "Path to atari_netstream.cfg linker config (optional for EDGE linking)")

if(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM)
    if(EDGE_NETSTREAM_ROOT)
        if(NOT EDGE_NETSTREAM_SOURCE_DIR)
            set(EDGE_NETSTREAM_SOURCE_DIR "${EDGE_NETSTREAM_ROOT}/handler")
        endif()
        if(NOT EDGE_NETSTREAM_INCLUDE_DIR)
            set(EDGE_NETSTREAM_INCLUDE_DIR "${EDGE_NETSTREAM_ROOT}/handler/include")
        endif()
        if(NOT EDGE_NETSTREAM_CFG)
            set(EDGE_NETSTREAM_CFG "${EDGE_NETSTREAM_ROOT}/examples/common/atari_netstream.cfg")
        endif()
    endif()

    if(EDGE_NETSTREAM_SOURCE_DIR AND NOT EXISTS "${EDGE_NETSTREAM_SOURCE_DIR}/netstream.s")
        message(FATAL_ERROR
            "EDGE_NETSTREAM_SOURCE_DIR set but missing netstream.s: '${EDGE_NETSTREAM_SOURCE_DIR}/netstream.s'")
    endif()

    if(EDGE_NETSTREAM_INCLUDE_DIR)
        if(NOT EXISTS "${EDGE_NETSTREAM_INCLUDE_DIR}/sio.inc" OR
           NOT EXISTS "${EDGE_NETSTREAM_INCLUDE_DIR}/hardware.inc")
            message(FATAL_ERROR
                "EDGE_NETSTREAM_INCLUDE_DIR set but missing expected includes (sio.inc, hardware.inc): '${EDGE_NETSTREAM_INCLUDE_DIR}'")
        endif()
    endif()

    add_compile_definitions(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1)

    # Stage 9C: ABI shim to convert carry-flag results to normal C returns.
    # Stage 9E: also assembles the EDGE-owned scaffolded Netstream handler, which
    # provides the raw _ns_* symbols the shim resolves against (no external CA65
    # netstream.s, no cl65/ca65/ld65). Both are llvm-mos / GNU-as syntax.
    # Only compiled/assembled when ON.
    if(CMAKE_CXX_COMPILER MATCHES "mos")
        # Atari-target assembly compilation only (not host mos-sim).
        add_library(edge_netstream_abi_shim OBJECT
            engine/platform/atari/fujinet_netstream_realtime_abi.s
            engine/platform/atari/fujinet_netstream_handler.S)
        # Route the .s/.S through the C compiler driver (project enables C, not
        # ASM) but force assembly with `-x assembler-with-cpp` so the files are
        # preprocessed and assembled, not parsed as C. The handler #includes the
        # constants layer, hence the platform include dir below.
        set_source_files_properties(
            engine/platform/atari/fujinet_netstream_realtime_abi.s
            engine/platform/atari/fujinet_netstream_handler.S
            PROPERTIES
            LANGUAGE C  # ASM via compiler integration
            COMPILE_OPTIONS "-x;assembler-with-cpp"
        )
        target_include_directories(edge_netstream_abi_shim PRIVATE
            ${CMAKE_SOURCE_DIR}/engine/platform/atari)
    else()
        # Host toolchain cannot compile 6502 assembly; skip shim.
        # Tests will compile (C++) but link will fail if symbols are used.
    endif()
endif()

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

# fujinetlib — prebuilt static library + headers, used by the fujinet_net_test
# demo. Built with the same mos-atari8-dos toolchain (see that repo's
# CMakeLists.txt). Override FUJINETLIB_DIR if the checkout lives elsewhere.
set(FUJINETLIB_DIR ""
    CACHE PATH "Path to the fujinetlib-llvm checkout")
set(FUJINETLIB_INC ${FUJINETLIB_DIR}/src/include)
set(FUJINETLIB_A   ${FUJINETLIB_DIR}/build/libfujinet.a)

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

    # P/M multiplexer demo — independent .xex (no charset header; OS ROM font).
    add_executable(multiplex_demo demo/multiplex_demo.cpp)
    target_compile_options(multiplex_demo PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(multiplex_demo PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(multiplex_demo PROPERTIES OUTPUT_NAME "multiplex_demo.xex")
    target_link_options(multiplex_demo PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:multiplex_demo>/multiplex_demo.map")

    # Arena demo — native ANTIC/GTIA build (constexpr in-source charset).
    add_executable(arena_native demo/arena/arena_native.cpp)
    target_compile_options(arena_native PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra)
    target_include_directories(arena_native PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(arena_native PROPERTIES OUTPUT_NAME "arena_native.xex")
    target_link_options(arena_native PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:arena_native>/arena_native.map")

    # Arena demo — VBXE Tier 2 full-overlay build (separate source): pixel sprites
    # over a VBXE overlay canvas. Run with a VBXE FX core, BASIC disabled.
    add_executable(arena_vbxe demo/arena/arena_vbxe.cpp)
    target_compile_options(arena_vbxe PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra)
    target_include_directories(arena_vbxe PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(arena_vbxe PROPERTIES OUTPUT_NAME "arena_vbxe.xex")
    target_link_options(arena_vbxe PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:arena_vbxe>/arena_vbxe.map")

    # Fujinet UDP position-streaming smoke test — links the prebuilt fujinetlib.
    # -O2 (not -Os): -Os breaks real-time performance with the template-heavy
    # engine headers (same as the arena demos).
    if(EXISTS "${FUJINETLIB_INC}/fujinet-network.h" AND EXISTS "${FUJINETLIB_A}")
        add_executable(fujinet_net_test demo/fujinet_net_test/fujinet_net_test.cpp)
        target_compile_options(fujinet_net_test PRIVATE
            -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra)
        target_include_directories(fujinet_net_test PRIVATE
            ${CMAKE_SOURCE_DIR} ${FUJINETLIB_INC})
        target_link_libraries(fujinet_net_test PRIVATE ${FUJINETLIB_A})
        set_target_properties(fujinet_net_test PROPERTIES OUTPUT_NAME "fujinet_net_test.xex")
        target_link_options(fujinet_net_test PRIVATE
            "LINKER:--Map=$<TARGET_FILE_DIR:fujinet_net_test>/fujinet_net_test.map")
    else()
        message(STATUS
            "Skipping fujinet_net_test: set FUJINETLIB_DIR to a valid fujinetlib-llvm checkout")
    endif()

    # Stage 8H runtime validation demo (session lane only).
    # Build only when fujinet-lib session wiring is enabled and the toolchain is
    # Atari/llvm-mos compatible.
    if(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB AND CMAKE_CXX_COMPILER MATCHES "mos")
        add_executable(fujinet_session_validate
            demo/fujinet_session_validate/fujinet_session_validate.cpp)
        target_compile_options(fujinet_session_validate PRIVATE
            -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
        target_compile_definitions(fujinet_session_validate PRIVATE
            EDGE_FUJINET_VALIDATE_HOST="${EDGE_FUJINET_VALIDATE_HOST}"
            EDGE_FUJINET_VALIDATE_PORT=${EDGE_FUJINET_VALIDATE_PORT})
        target_include_directories(fujinet_session_validate PRIVATE
            ${CMAKE_SOURCE_DIR} ${EDGE_FUJINETLIB_INCLUDE_DIR})
        target_link_libraries(fujinet_session_validate PRIVATE ${EDGE_FUJINETLIB_LIBRARY})
        set_target_properties(fujinet_session_validate PROPERTIES
            OUTPUT_NAME "fujinet_session_validate.xex")
        target_link_options(fujinet_session_validate PRIVATE
            "LINKER:--Map=$<TARGET_FILE_DIR:fujinet_session_validate>/fujinet_session_validate.map")
    else()
        message(STATUS
            "Skipping fujinet_session_validate executable: requires EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON and a mos toolchain")
    endif()

    # Dual-lane EDGE net API usage demo (stubbed seam; no real transport wiring).
    add_executable(net_dual_lane_demo demo/net_dual_lane/net_dual_lane.cpp)
    target_compile_options(net_dual_lane_demo PRIVATE
        -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra)
    target_include_directories(net_dual_lane_demo PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(net_dual_lane_demo PROPERTIES OUTPUT_NAME "net_dual_lane_demo.xex")
    target_link_options(net_dual_lane_demo PRIVATE
        "LINKER:--Map=$<TARGET_FILE_DIR:net_dual_lane_demo>/net_dual_lane_demo.map")
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

    # ── Arena demo — native ANTIC/GTIA build (arena_native.xex) ──────────
    #   cmake --build build --target arena_native   # -> build/arena_native.xex
    # Constexpr in-source charset, so no generated charset header dependency.
    set(ARENA_NATIVE_XEX ${CMAKE_BINARY_DIR}/arena_native.xex)
    set(ARENA_NATIVE_MAP ${CMAKE_BINARY_DIR}/arena_native.map)
    add_custom_command(
        OUTPUT ${ARENA_NATIVE_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/arena/arena_native.cpp
                -o ${ARENA_NATIVE_XEX}
                -Wl,--Map=${ARENA_NATIVE_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/arena/arena_native.cpp
                ${CMAKE_SOURCE_DIR}/demo/arena/arena_shared.h
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${ARENA_NATIVE_MAP}
        COMMENT "Building Atari XEX demo (arena_native.xex)"
        VERBATIM)
    add_custom_target(arena_native DEPENDS ${ARENA_NATIVE_XEX})

    # ── Arena demo — VBXE Tier 2 full-overlay build (arena_vbxe.xex) ──────
    #   cmake --build build --target arena_vbxe   # -> build/arena_vbxe.xex
    # Separate source sharing arena_shared.h: pixel sprites over a VBXE overlay
    # canvas (ANTIC playfield off). Run with a VBXE FX core, BASIC disabled.
    set(ARENA_VBXE_XEX ${CMAKE_BINARY_DIR}/arena_vbxe.xex)
    set(ARENA_VBXE_MAP ${CMAKE_BINARY_DIR}/arena_vbxe.map)
    add_custom_command(
        OUTPUT ${ARENA_VBXE_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/arena/arena_vbxe.cpp
                -o ${ARENA_VBXE_XEX}
                -Wl,--Map=${ARENA_VBXE_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/arena/arena_vbxe.cpp
                ${CMAKE_SOURCE_DIR}/demo/arena/arena_shared.h
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${ARENA_VBXE_MAP}
        COMMENT "Building Atari XEX demo (arena_vbxe.xex)"
        VERBATIM)
    add_custom_target(arena_vbxe DEPENDS ${ARENA_VBXE_XEX})

    # ── Fujinet UDP position-streaming smoke test (fujinet_net_test.xex) ──
    #   cmake --build build --target fujinet_net_test  # -> build/fujinet_net_test.xex
    # Links the prebuilt fujinetlib static lib (passed as a command-line input)
    # and adds its include dir. -O2 (not -Os): -Os breaks real-time performance
    # with the template-heavy engine headers (same as the arena demos).
    if(EXISTS "${FUJINETLIB_INC}/fujinet-network.h" AND EXISTS "${FUJINETLIB_A}")
        set(FNTEST_XEX ${CMAKE_BINARY_DIR}/fujinet_net_test.xex)
        set(FNTEST_MAP ${CMAKE_BINARY_DIR}/fujinet_net_test.map)
        add_custom_command(
            OUTPUT ${FNTEST_XEX}
            COMMAND ${MOS_ATARI8_CXX}
                    -std=c++20 -fno-exceptions -fno-rtti -O2 -Wall -Wextra
                    -I${CMAKE_SOURCE_DIR} -I${FUJINETLIB_INC}
                    ${CMAKE_SOURCE_DIR}/demo/fujinet_net_test/fujinet_net_test.cpp
                    ${FUJINETLIB_A}
                    -o ${FNTEST_XEX}
                    -Wl,--Map=${FNTEST_MAP}
            DEPENDS ${CMAKE_SOURCE_DIR}/demo/fujinet_net_test/fujinet_net_test.cpp
                    ${FUJINETLIB_A}
                    ${EDGE_ENGINE_HEADERS}
            BYPRODUCTS ${FNTEST_MAP}
            COMMENT "Building Atari XEX demo (fujinet_net_test.xex)"
            VERBATIM)
        add_custom_target(fujinet_net_test DEPENDS ${FNTEST_XEX})
    else()
        message(STATUS
            "Skipping fujinet_net_test custom target: set FUJINETLIB_DIR to a valid fujinetlib-llvm checkout")
    endif()

    # ── Stage 8H session-lane runtime validation demo ─────────────────────
    #   cmake --build build --target fujinet_session_validate
    #     -> build/fujinet_session_validate.xex
    # Gated to optional fujinet-lib integration and mos-atari8-dos compiler.
    if(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB
       AND EXISTS "${EDGE_FUJINETLIB_INCLUDE_DIR}/fujinet-network.h"
       AND EXISTS "${EDGE_FUJINETLIB_LIBRARY}")
        set(FN_SESSION_VALIDATE_XEX ${CMAKE_BINARY_DIR}/fujinet_session_validate.xex)
        set(FN_SESSION_VALIDATE_MAP ${CMAKE_BINARY_DIR}/fujinet_session_validate.map)
        add_custom_command(
            OUTPUT ${FN_SESSION_VALIDATE_XEX}
            COMMAND ${MOS_ATARI8_CXX}
                    -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                    -DEDGE_FUJINET_VALIDATE_HOST=\"${EDGE_FUJINET_VALIDATE_HOST}\"
                    -DEDGE_FUJINET_VALIDATE_PORT=${EDGE_FUJINET_VALIDATE_PORT}
                    -DEDGE_ATARI_FUJINET_SESSION_FUJINETLIB=1
                    -I${CMAKE_SOURCE_DIR} -I${EDGE_FUJINETLIB_INCLUDE_DIR}
                    ${CMAKE_SOURCE_DIR}/demo/fujinet_session_validate/fujinet_session_validate.cpp
                    ${EDGE_FUJINETLIB_LIBRARY}
                    -o ${FN_SESSION_VALIDATE_XEX}
                    -Wl,--Map=${FN_SESSION_VALIDATE_MAP}
            DEPENDS ${CMAKE_SOURCE_DIR}/demo/fujinet_session_validate/fujinet_session_validate.cpp
                    ${CMAKE_SOURCE_DIR}/demo/fujinet_session_validate/README.md
                    ${EDGE_FUJINETLIB_LIBRARY}
                    ${EDGE_ENGINE_HEADERS}
            BYPRODUCTS ${FN_SESSION_VALIDATE_MAP}
            COMMENT "Building Atari XEX demo (fujinet_session_validate.xex)"
            VERBATIM)
        add_custom_target(fujinet_session_validate DEPENDS ${FN_SESSION_VALIDATE_XEX})
    else()
        message(STATUS
            "Skipping fujinet_session_validate custom target: requires EDGE_ATARI_FUJINET_SESSION_FUJINETLIB=ON and valid EDGE_FUJINETLIB include/library paths")
    endif()

    # Stage 9E Atari-only ABI/link smoke for the EDGE-owned Netstream scaffold.
    # Build-only smoke: verifies symbol/link compatibility between:
    # - EDGE ABI shim assembly (fujinet_netstream_realtime_abi.s)
    # - EDGE-owned scaffolded handler (fujinet_netstream_handler.S)
    # - minimal C++ symbol reference source
    # This is fully self-contained: it does NOT reference any external
    # fujinet-atari-netstream checkout, and uses no cl65/ca65/ld65.
    # This target is intentionally excluded from host test runs.
    if(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM
       AND EXISTS "${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S")
        set(NETSTREAM_ABI_LINK_SMOKE_XEX
            ${CMAKE_BINARY_DIR}/netstream_abi_link_smoke.xex)
        set(NETSTREAM_ABI_LINK_SMOKE_MAP
            ${CMAKE_BINARY_DIR}/netstream_abi_link_smoke.map)
        add_custom_command(
            OUTPUT ${NETSTREAM_ABI_LINK_SMOKE_XEX}
            COMMAND ${MOS_ATARI8_CXX}
                    -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                    -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
                    -I${CMAKE_SOURCE_DIR}
                    -I${CMAKE_SOURCE_DIR}/engine/platform/atari
                    ${CMAKE_SOURCE_DIR}/tests/backends/atari/test_netstream_abi_atari_link_smoke.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    -o ${NETSTREAM_ABI_LINK_SMOKE_XEX}
                    -Wl,--Map=${NETSTREAM_ABI_LINK_SMOKE_MAP}
            DEPENDS ${CMAKE_SOURCE_DIR}/tests/backends/atari/test_netstream_abi_atari_link_smoke.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_constants.inc
                    ${EDGE_ENGINE_HEADERS}
            BYPRODUCTS ${NETSTREAM_ABI_LINK_SMOKE_MAP}
            COMMENT "Building Atari Netstream ABI link smoke (netstream_abi_link_smoke.xex)"
            VERBATIM)
        add_custom_target(netstream_abi_link_smoke
            DEPENDS ${NETSTREAM_ABI_LINK_SMOKE_XEX})

        # Stage 9O.3a Altirra/hardware validation probe (.xex). Runs the REAL
        # _ns_begin_stream/_ns_end_stream (which write the OS-vector page + PACTL and
        # so cannot run under mos-sim) and snapshots PACTL / serial vectors / VIMIRQ /
        # POKMSK / nsVideoStd into page 6 ($0600/$0610/$0620) for debugger inspection.
        # Stage 9P adds an IRQ-fenced post-begin send -> SEROUT-prime snapshot at $0640,
        # read from the test-only RAM mirror -- so the probe is built WITH
        # EDGE_NETSTREAM_TEST_HOOKS to expose ns_test_last_prime_byte / ns_test_prime_count
        # and the TX/gate state symbols. Altirra-only; intentionally NOT a CTest.
        set(NETSTREAM_VEC_PROBE_XEX ${CMAKE_BINARY_DIR}/netstream_vectors_altirra_probe.xex)
        set(NETSTREAM_VEC_PROBE_MAP ${CMAKE_BINARY_DIR}/netstream_vectors_altirra_probe.map)
        add_custom_command(
            OUTPUT ${NETSTREAM_VEC_PROBE_XEX}
            COMMAND ${MOS_ATARI8_CXX}
                    -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                    -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
                    -DEDGE_NETSTREAM_TEST_HOOKS=1
                    -I${CMAKE_SOURCE_DIR}
                    -I${CMAKE_SOURCE_DIR}/engine/platform/atari
                    ${CMAKE_SOURCE_DIR}/tests/backends/atari/netstream_vectors_altirra_probe.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    -o ${NETSTREAM_VEC_PROBE_XEX}
                    -Wl,--Map=${NETSTREAM_VEC_PROBE_MAP}
            DEPENDS ${CMAKE_SOURCE_DIR}/tests/backends/atari/netstream_vectors_altirra_probe.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_constants.inc
                    ${EDGE_ENGINE_HEADERS}
            BYPRODUCTS ${NETSTREAM_VEC_PROBE_MAP}
            COMMENT "Building Atari Netstream vector probe (netstream_vectors_altirra_probe.xex)"
            VERBATIM)
        add_custom_target(netstream_vectors_altirra_probe
            DEPENDS ${NETSTREAM_VEC_PROBE_XEX})

        # Stage 9Q.2 Altirra/hardware validation probe (.xex) for the REAL init path.
        # Runs edge_ns_init_netstream (DCB fill + SIOV + DSTATS check) -- which cannot run
        # under mos-sim -- and snapshots the init result / DSTATS / nsFinal* / payload
        # prefix / pre+post DCB into page 6 for debugger inspection (Mode A no-FujiNet,
        # Mode B FujiNet present). Built WITH EDGE_NETSTREAM_TEST_HOOKS to read nsPayloadBuf.
        # Altirra-only; intentionally NOT a CTest. Keeps the 9P vectors probe untouched.
        set(NETSTREAM_INIT_PROBE_XEX ${CMAKE_BINARY_DIR}/netstream_init_altirra_probe.xex)
        set(NETSTREAM_INIT_PROBE_MAP ${CMAKE_BINARY_DIR}/netstream_init_altirra_probe.map)
        add_custom_command(
            OUTPUT ${NETSTREAM_INIT_PROBE_XEX}
            COMMAND ${MOS_ATARI8_CXX}
                    -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                    -DEDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
                    -DEDGE_NETSTREAM_TEST_HOOKS=1
                    -I${CMAKE_SOURCE_DIR}
                    -I${CMAKE_SOURCE_DIR}/engine/platform/atari
                    ${CMAKE_SOURCE_DIR}/tests/backends/atari/netstream_init_altirra_probe.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    -o ${NETSTREAM_INIT_PROBE_XEX}
                    -Wl,--Map=${NETSTREAM_INIT_PROBE_MAP}
            DEPENDS ${CMAKE_SOURCE_DIR}/tests/backends/atari/netstream_init_altirra_probe.cpp
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
                    ${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_constants.inc
                    ${EDGE_ENGINE_HEADERS}
            BYPRODUCTS ${NETSTREAM_INIT_PROBE_MAP}
            COMMENT "Building Atari Netstream init probe (netstream_init_altirra_probe.xex)"
            VERBATIM)
        add_custom_target(netstream_init_altirra_probe
            DEPENDS ${NETSTREAM_INIT_PROBE_XEX})
    else()
        message(STATUS
            "Skipping netstream_abi_link_smoke custom target: requires EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON")
    endif()

    # ── Dual-lane EDGE net API usage demo (net_dual_lane_demo.xex) ─────────
    #   cmake --build build --target net_dual_lane_demo  # -> build/net_dual_lane_demo.xex
    # Keeps transport fully stubbed: demonstrates API usage only.
    set(NET_DUAL_LANE_XEX ${CMAKE_BINARY_DIR}/net_dual_lane_demo.xex)
    set(NET_DUAL_LANE_MAP ${CMAKE_BINARY_DIR}/net_dual_lane_demo.map)
    add_custom_command(
        OUTPUT ${NET_DUAL_LANE_XEX}
        COMMAND ${MOS_ATARI8_CXX}
                -std=c++20 -fno-exceptions -fno-rtti -Os -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}
                ${CMAKE_SOURCE_DIR}/demo/net_dual_lane/net_dual_lane.cpp
                -o ${NET_DUAL_LANE_XEX}
                -Wl,--Map=${NET_DUAL_LANE_MAP}
        DEPENDS ${CMAKE_SOURCE_DIR}/demo/net_dual_lane/net_dual_lane.cpp
                ${EDGE_ENGINE_HEADERS}
        BYPRODUCTS ${NET_DUAL_LANE_MAP}
        COMMENT "Building Atari XEX demo (net_dual_lane_demo.xex)"
        VERBATIM)
    add_custom_target(net_dual_lane_demo DEPENDS ${NET_DUAL_LANE_XEX})
else()
    message(STATUS "mos-atari8-dos-clang++ not found; 'atari_hw_test'/'atari_scroll_test'/'atari_vbxe_bringup'/'net_dual_lane_demo'/'fujinet_session_validate' demo targets unavailable")
endif()
