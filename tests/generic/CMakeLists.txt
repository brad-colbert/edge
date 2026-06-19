# Backend-neutral engine tests. Assertions here are about portable engine
# behaviour; they may name the one backend that exists (atari::Mode, the
# display-program builder) as a concrete type, but assert no ANTIC/POKEY/GTIA
# byte. Backend-specific assertions live in ../backends/atari.

add_engine_test(test_pool)
add_engine_test(test_math)
add_engine_test(test_input)
add_engine_test(test_interrupt)
add_engine_test(test_screen)
add_engine_test(test_gfx)
add_engine_test(test_sprites)
add_engine_test(test_sound)
add_engine_test(test_scroll)
add_engine_test(test_mode4_geometry)
add_engine_test(test_tank_playfield)
add_engine_test(test_tank_motion)
add_engine_test(test_tiles)
add_engine_test(test_core)
add_engine_test(test_net_rings)
add_engine_test(test_net_api)
add_engine_test(test_net_capabilities)
