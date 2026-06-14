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

# Stage 9H: RX ring behavioral self-test, run under mos-sim. Compiles the
# EDGE-owned handler + ABI shim with PRIVATE test hooks (EDGE_NETSTREAM_TEST_HOOKS)
# and exercises _ns_recv_byte / _ns_bytes_avail (via the shim) end to end. The
# handler routines are pure-RAM, so they execute under the simulator (SMC works in
# sim RAM). Requires the realtime option ON and the mos toolchain (the default
# unit-test build is mos-sim). Self-contained: no upstream netstream.s, no CA65.
if(EDGE_ATARI_FUJINET_REALTIME_NETSTREAM AND CMAKE_CXX_COMPILER MATCHES "mos")
	add_executable(test_netstream_rx_ring
		test_netstream_rx_ring.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_rx_ring PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_rx_ring PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_rx_ring
		COMMAND mos-sim $<TARGET_FILE:test_netstream_rx_ring>)

	# Stage 9I: TX ring behavioral self-test (same gating/pattern as RX).
	add_executable(test_netstream_tx_ring
		test_netstream_tx_ring.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_tx_ring PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_tx_ring PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_tx_ring
		COMMAND mos-sim $<TARGET_FILE:test_netstream_tx_ring>)

	# Stage 9J: begin/end lifecycle behavioral self-test (same gating/pattern).
	add_executable(test_netstream_lifecycle
		test_netstream_lifecycle.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_lifecycle PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_lifecycle PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_lifecycle
		COMMAND mos-sim $<TARGET_FILE:test_netstream_lifecycle>)

	# Stage 9L: status/error model (serialErrors[4]) behavioral self-test.
	add_executable(test_netstream_status
		test_netstream_status.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_status PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_status PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_status
		COMMAND mos-sim $<TARGET_FILE:test_netstream_status>)

	# Stage 9M.1: SwapIrqVector involution self-test (uncalled IRQ dispatcher /
	# vector swap; no hardware registers touched).
	add_executable(test_netstream_swap_vector
		test_netstream_swap_vector.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_swap_vector PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_swap_vector PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_swap_vector
		COMMAND mos-sim $<TARGET_FILE:test_netstream_swap_vector>)

	# Stage 9M.4: DetectPALViaVCOUNT bounded-return smoke (uncalled detection routine).
	add_executable(test_netstream_detect_pal
		test_netstream_detect_pal.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_detect_pal PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_detect_pal PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_detect_pal
		COMMAND mos-sim $<TARGET_FILE:test_netstream_detect_pal>)

	# Stage 9N: serial vector install/restore self-test (OS vectors only, no POKEY).
	add_executable(test_netstream_vectors
		test_netstream_vectors.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_vectors PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_vectors PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_vectors
		COMMAND mos-sim $<TARGET_FILE:test_netstream_vectors>)

	# Stage 9O.4: serial-IRQ arm/disarm guard self-test (disarm-without-arm no-op).
	add_executable(test_netstream_irq_arm
		test_netstream_irq_arm.cpp
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S)
	set_source_files_properties(
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_realtime_abi.s
		${CMAKE_SOURCE_DIR}/engine/platform/atari/fujinet_netstream_handler.S
		PROPERTIES
		LANGUAGE C
		COMPILE_OPTIONS "-x;assembler-with-cpp")
	target_include_directories(test_netstream_irq_arm PRIVATE
		${CMAKE_SOURCE_DIR}
		${CMAKE_SOURCE_DIR}/engine/platform/atari)
	target_compile_definitions(test_netstream_irq_arm PRIVATE
		EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=1
		EDGE_NETSTREAM_TEST_HOOKS=1)
	add_test(NAME test_netstream_irq_arm
		COMMAND mos-sim $<TARGET_FILE:test_netstream_irq_arm>)
endif()
