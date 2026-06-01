# Atari-backend tests. These assert Atari hardware specifics: platform capability
# profiles, ANTIC display-program bytes / 4K-crossing reloads / DMA bits, and the
# raster (DLI) delivery walker.

add_engine_test(test_platform)
add_engine_test(test_atari_display)
add_engine_test(test_atari_scroll)
add_engine_test(test_atari_interrupt)
add_engine_test(test_atari_core)
