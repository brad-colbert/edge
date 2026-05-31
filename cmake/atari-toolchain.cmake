# Toolchain file for building the hardware demo (demo/hw_test.cpp) for the
# llvm-mos `atari8-dos` platform — a real Atari 8-bit target whose linker emits
# a loadable .xex (auto-run via the $02E0 run vector).
#
# This is an alternative to the `hw_test` custom target in the top-level
# CMakeLists.txt (which the mos-sim test build provides). Use it when you want a
# dedicated, fully CMake-managed Atari build directory:
#
#   cmake -B build-atari -DCMAKE_TOOLCHAIN_FILE=cmake/atari-toolchain.cmake \
#         -DEDGE_BUILD_DEMO=ON
#   cmake --build build-atari --target hw_test
#
# Note: this target builds the Atari .xex, not the mos-sim unit tests; the two
# toolchains are mutually exclusive within a single configure.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR mos)

set(CMAKE_C_COMPILER   mos-atari8-dos-clang)
set(CMAKE_CXX_COMPILER mos-atari8-dos-clang++)

# The compiler-id probe links a full executable by default; a static library is
# enough to validate the cross compiler for a bare-metal / ROM target.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
