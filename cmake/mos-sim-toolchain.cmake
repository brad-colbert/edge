# Toolchain file for building the engine tests against the llvm-mos `mos-sim`
# platform — a generic 6502 simulator target with stdout + exit-code support.
#
# Configure with:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mos-sim-toolchain.cmake

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR mos)

set(CMAKE_C_COMPILER   mos-sim-clang)
set(CMAKE_CXX_COMPILER mos-sim-clang++)

# The compiler-id probe links a full executable by default; for a bare-metal /
# simulator target a static library is enough to validate the compiler.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
