# Engine unit tests. Each test is an executable built for the mos-sim platform
# and run under the `mos-sim` simulator; a 0 exit code means the test passed.
#
# Tests are split into two groups (API_DECOUPLING_REFACTOR_PLAN.md):
#   generic/        — backend-neutral engine behaviour (no Atari hardware asserts)
#   backends/atari/ — Atari-specific assertions (ANTIC bytes, POKEY/GTIA, DMA bits)

function(add_engine_test name)
    add_executable(${name} ${name}.cpp)
    add_test(
        NAME ${name}
        COMMAND mos-sim $<TARGET_FILE:${name}>
    )
endfunction()

add_subdirectory(generic)
add_subdirectory(backends/atari)
