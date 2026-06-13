; engine/platform/atari/fujinet_netstream_realtime_abi.s
;
; Stage 9C / 9E: EDGE ABI shim for Netstream carry-flag operations.
;
; Netstream handler exposes 13 symbols that use 6502 carry flag for status:
; - ns_send_byte: carry clear = sent, carry set = full
; - ns_recv_byte: carry clear = success, carry set = empty
;
; This shim wraps those operations to convert carry-flag results into normal
; C-callable return values (0 = success, 1 = failure/full/empty).
;
; EDGE-owned symbols use edge_ns_* prefix to avoid collisions with upstream.
;
; Stage 9E: translated to llvm-mos / GNU-as syntax (.global / .text). The raw
; _ns_* symbols are resolved at link time against the EDGE-owned scaffolded
; handler (fujinet_netstream_handler.S); no .import declarations are needed.
;
; Calling convention (llvm-mos/6502):
; - Return value: A register (0 or 1) or X:A 16-bit
; - Arguments: stack-based or register-based depending on llvm-mos ABI
;
; All these are optional and only linked when EDGE_ATARI_FUJINET_REALTIME_NETSTREAM=ON.

.global _edge_ns_send_byte
.global _edge_ns_recv_byte_packed
.global _edge_ns_bytes_avail
.global _edge_ns_get_status
.global _edge_ns_init_netstream
.global _edge_ns_begin_stream
.global _edge_ns_end_stream

.text

; uint8_t edge_ns_send_byte(uint8_t byte)
; Wrapper for ns_send_byte(byte in A).
; Returns 0 if byte enqueued (carry clear), 1 if TX buffer full (carry set).
_edge_ns_send_byte:
	; A register contains the byte to send (per llvm-mos ABI).
	jsr _ns_send_byte
	bcc .Lsend_sent  ; carry clear = sent
	lda #1           ; carry set = full, return 1
	rts
.Lsend_sent:
	lda #0           ; carry clear = sent, return 0
	rts

; uint16_t edge_ns_recv_byte_packed(void)
; Wrapper for ns_recv_byte.
; Returns packed u16 in X:A:
; - A (low byte): received byte, valid when X == 0
; - X (high byte): status (0 = success, 1 = empty)
; This avoids pointer ABI risk in the RX hot path.
_edge_ns_recv_byte_packed:
	jsr _ns_recv_byte
	bcc .Lrecv_have_byte    ; carry clear = byte returned in A
	lda #0                  ; empty: low byte ignored by contract
	ldx #1                  ; high byte status = empty
	rts
.Lrecv_have_byte:
	ldx #0                  ; high byte status = success
	rts

; uint16_t edge_ns_bytes_avail(void)
; Wrapper for ns_bytes_avail.
; Returns RX buffer byte count as 16-bit value (X:A, high:low).
; No carry flag here; direct passthrough.
_edge_ns_bytes_avail:
	jsr _ns_bytes_avail
	; Assume ns_bytes_avail returns u16 in X:A (big-endian 6502 style).
	rts

; uint8_t edge_ns_get_status(void)
; Wrapper for ns_get_status.
; Returns raw status byte from Netstream.
; No carry flag here; direct passthrough.
_edge_ns_get_status:
	jsr _ns_get_status
	; A contains status byte
	rts

; void edge_ns_init_netstream(const char* host, uint16_t flags, uint16_t port)
; Declared for future integration. Parameter marshaling is not yet proven
; against upstream init expectations, so this remains unused by production code.
_edge_ns_init_netstream:
	; Deferred ABI proof for init marshaling.
	jsr _ns_init_netstream
	rts

; void edge_ns_begin_stream(void)
; Wrapper for ns_begin_stream.
; Starts streaming operation.
_edge_ns_begin_stream:
	jsr _ns_begin_stream
	rts

; void edge_ns_end_stream(void)
; Wrapper for ns_end_stream.
; Ends streaming operation and cleans up.
_edge_ns_end_stream:
	jsr _ns_end_stream
	rts

