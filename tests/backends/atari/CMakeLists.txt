# Atari-backend tests. These assert Atari hardware specifics: platform capability
# profiles, ANTIC display-program bytes / 4K-crossing reloads / DMA bits, and the
# raster (DLI) delivery walker.

add_engine_test(test_platform)
add_engine_test(test_vbxe)
add_engine_test(test_vbxe_integration)
add_engine_test(test_atari_display)
add_engine_test(test_atari_scroll)
add_engine_test(test_atari_interrupt)
add_engine_test(test_atari_core)
add_engine_test(test_atari_net_seam)
add_engine_test(test_atari_fujinet_session_spec)
add_engine_test(test_atari_fujinet_netstream_scaffold)
add_engine_test(test_netstream_abi_smoke)
add_engine_test(test_netstream_state_size)

# Stage 8E.5 / 8F: session_send_nb behavior test.
# OFF-mode: tests OFF stubs (Unsupported). No special includes needed.
# ON-mode: tests pre-connection guards; also validates network_write symbol
#          is reachable after Stage 8F wiring.
#          Compile is always valid on host; link requires mos toolchain
#          (fujinet-lib archive is Atari/mos-target only).
if(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
	# Compile-only object target — always built when ON, host or mos.
	add_library(test_atari_fujinet_session_send_nb_obj OBJECT
		test_atari_fujinet_session_send_nb.cpp)
	target_include_directories(test_atari_fujinet_session_send_nb_obj PRIVATE
		${CMAKE_SOURCE_DIR}
		${EDGE_FUJINETLIB_INCLUDE_DIR})
	target_compile_definitions(test_atari_fujinet_session_send_nb_obj PRIVATE
		EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)

	# Linked executable only under mos toolchain.
	if(CMAKE_CXX_COMPILER MATCHES "mos")
		add_executable(test_atari_fujinet_session_send_nb
			test_atari_fujinet_session_send_nb.cpp)
		target_include_directories(test_atari_fujinet_session_send_nb PRIVATE
			${CMAKE_SOURCE_DIR}
			${EDGE_FUJINETLIB_INCLUDE_DIR})
		target_link_libraries(test_atari_fujinet_session_send_nb PRIVATE
			${EDGE_FUJINETLIB_LIBRARY})
		add_test(NAME test_atari_fujinet_session_send_nb
			COMMAND test_atari_fujinet_session_send_nb)
	endif()
else()
	# OFF-mode: standard test, no special includes.
	add_engine_test(test_atari_fujinet_session_send_nb)
endif()

if(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
	# Stage 8D smoke: validate fujinet-lib header visibility/signatures through
	# the adapter only. Keep this narrow and independent from unrelated Atari
	# layout/raster tests.
	add_library(edge_fujinetlib_session_smoke_obj OBJECT
		fujinetlib_session_smoke.cpp)
	target_include_directories(edge_fujinetlib_session_smoke_obj PRIVATE
		${CMAKE_SOURCE_DIR}
		${EDGE_FUJINETLIB_INCLUDE_DIR})

	# Optional link smoke executable. Build this only for mos toolchains,
	# because host toolchains may not link an Atari-target archive.
	if(CMAKE_CXX_COMPILER MATCHES "mos")
		add_executable(edge_fujinetlib_session_smoke
			fujinetlib_session_smoke.cpp)
		target_include_directories(edge_fujinetlib_session_smoke PRIVATE
			${CMAKE_SOURCE_DIR}
			${EDGE_FUJINETLIB_INCLUDE_DIR})
		target_link_libraries(edge_fujinetlib_session_smoke PRIVATE
			${EDGE_FUJINETLIB_LIBRARY})
	endif()
endif()
