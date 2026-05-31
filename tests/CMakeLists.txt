# Engine unit tests. Each test is an executable built for the mos-sim platform
# and run under the `mos-sim` simulator; a 0 exit code means the test passed.

function(add_engine_test name)
    add_executable(${name} ${name}.cpp)
    add_test(
        NAME ${name}
        COMMAND mos-sim $<TARGET_FILE:${name}>
    )
endfunction()

add_engine_test(test_pool)
add_engine_test(test_math)
add_engine_test(test_input)
add_engine_test(test_platform)
add_engine_test(test_interrupt)
add_engine_test(test_screen)
add_engine_test(test_sprites)
add_engine_test(test_sound)
add_engine_test(test_scroll)
add_engine_test(test_tiles)
add_engine_test(test_core)
