; engine/platform/atari/fujinet_netstream_handler.S
;
; Stage 9E: EDGE-owned, scaffolded translation of the upstream FujiNet Netstream
; concurrent handler into llvm-mos / GNU-as assembly.
;
; Incremental translation. Exports the full upstream `_ns_*` symbol surface so the
; EDGE ABI shim (fujinet_netstream_realtime_abi.s) links cleanly under llvm-mos.
; Implemented so far:
;   - 9F: real BSS buffers + low-risk diagnostic/status getters.
;   - 9H: RX ring path -- _ns_recv_byte (RX consumer) and _ns_bytes_avail, with
;         the upstream self-modifying-code read pointer preserved (Option A).
;   - 9I: TX enqueue -- _ns_send_byte (pure RAM ring enqueue; index-based, no SMC).
;         The upstream idle-prime SEROUT write is deliberately NOT done here; every
;         accepted byte is buffered and transmission is deferred (see _ns_send_byte).
;   - 9J: hardware-free lifecycle -- _ns_begin_stream / _ns_end_stream initialize and
;         reset ALL RX/TX/status RAM state (shared ns_init_ring_state) and toggle the
;         serialConcurrentNum active flag. No hardware/OS/vector work (see below).
; Still INERT / deferred (later stages):
;   - _ns_init_netstream
;   - IRQ handlers + vector swap; POKEY/SKCTL/AUDCTL/PACTL/SEROUT; SIO/DCB enable
;   - DetectPALViaVCOUNT; idle-prime SEROUT + outBuf; serialErrors[4] overflow model
;   - packet framing
;
; SMC note (Option A, per Stage 9D): the RX pointers inReadPtr (in _ns_recv_byte)
; and inPtr (in the test-only producer) are kept as self-modifying absolute
; operand bytes living in .text -- the two operand bytes of `lda abs`/`sta abs`
; double as the 16-bit pointer variable (mutated in place by inc). They are NOT
; .bss. The RX buffer bounds (inBuf*/inBufEnd*) are promoted to plain .bss
; variables because their upstream SMC homes are in the not-yet-translated IRQ
; handler -- a deliberate, documented deviation.
;
; Source dialect notes (vs upstream CA65 netstream.s):
;   .export X = Y   -> a label `X` aliasing the impl, plus `.global X`
;   .import         -> not needed (GNU-as resolves externs at link)
;   .proc/.endproc  -> plain labels
;   .code / .bss    -> .text / .bss (.section)
;   $XX numeric     -> 0xXX  (see fujinet_netstream_constants.inc)
;
; This file uses #include, so it MUST be preprocessed (built as a .S source or
; with an explicit -x assembler-with-cpp).

#include "fujinet_netstream_constants.inc"

;==========================================================================
; Exported C-visible symbols (match upstream surface, all 13).
.global _ns_begin_stream
.global _ns_end_stream
.global _ns_get_version
.global _ns_get_base
.global _ns_send_byte
.global _ns_recv_byte
.global _ns_bytes_avail
.global _ns_get_status
.global _ns_get_video_std
.global _ns_init_netstream
.global _ns_get_final_flags
.global _ns_get_final_audf3
.global _ns_get_final_audf4

.text

;==========================================================================
; _ns_begin_stream  (upstream: NS_BeginConcurrent_Impl) -- Stage 9J, HARDWARE-FREE.
;
; Initializes ALL RX/TX/status RAM state to empty-ready and marks the stream
; active. It does NOT do any of upstream's hardware/OS bring-up -- deferred to the
; IRQ/POKEY stage:
;   - DetectPALViaVCOUNT (VCOUNT read + spin)
;   - POKEY/SKCTL/AUDCTL/AUDF3/AUDF4 programming
;   - PACTL motor assert
;   - serial IRQ vector save/install (VSERIN..VSEROC) + VIMIRQ swap
;   - POKMSK/IRQEN enable; SEROUT idle-prime
; No critical section is needed yet: no IRQ is installed, so nothing mutates this
; state concurrently. (The IRQ stage will add sei-guarded vector/HW sections.)
_ns_begin_stream:
    jsr ns_init_ring_state      ; RX+TX+status+serialOutIdle -> empty-ready
    lda #0
    sta nsVideoStd              ; NTSC default; real PAL detection deferred
    lda #1
    sta serialConcurrentNum     ; mark active (NOT used to gate the byte API)
    rts

;==========================================================================
; _ns_end_stream  (upstream: NS_EndConcurrent_Impl) -- Stage 9J, HARDWARE-FREE.
;
; Leaves the handler in a safe, initialized-EMPTY state (not half-initialized):
; full ring re-init identical to begin, but marked inactive. Does NOT touch any
; hardware/OS state (IRQEN/POKMSK, vectors, PACTL) -- deferred to the IRQ stage.
; The byte API is not gated on serialConcurrentNum, so after end recv/bytes_avail
; simply read empty.
_ns_end_stream:
    jsr ns_init_ring_state      ; RX+TX+status+serialOutIdle -> empty-ready
    lda #0
    sta serialConcurrentNum     ; mark inactive
    rts

;==========================================================================
; ns_init_ring_state (internal, not exported) -- pure-RAM reset shared by
; _ns_begin_stream / _ns_end_stream. Sets every RX/TX/status field to the
; empty-ready state; leaves serialConcurrentNum and nsVideoStd to the caller.
; Touches only handler BSS + the SMC pointer operands -- no hardware/OS/vectors.
ns_init_ring_state:
    ; --- RX ring ---
    lda #NS_INPUT_BUFSIZE
    sta serialInSize
    sta serialInSpaceLo         ; empty: free space == capacity
    lda #0
    sta serialInSize+1
    sta serialInSpaceHi
    lda #<nsInputBuffer
    ldy #>nsInputBuffer
    sta inBufLo
    sta inReadPtr               ; patch SMC operand (RX consumer ptr; always present)
    sty inBufHi
    sty inReadPtr+1
#if defined(EDGE_NETSTREAM_TEST_HOOKS)
    ; inPtr is the RX PRODUCER pointer. It only exists when a producer exists --
    ; currently the test-only ns_test_rx_push (a stand-in for the deferred input
    ; IRQ). Non-test builds have no producer, so inPtr is absent and is not
    ; initialized here. When the input-IRQ handler lands it will own inPtr and this
    ; guard goes away.
    sta inPtr
    sty inPtr+1
#endif
    clc
    adc serialInSize            ; A = <nsInputBuffer + 128
    sta inBufEndLo
    tya
    adc serialInSize+1
    sta inBufEndHi
    ; --- TX ring ---
    lda #0
    sta outLevel
    sta serialOutHead
    sta outIndex
    lda #0xff
    sta serialOutIdle           ; idle (consumed later by the deferred idle-prime)
    ; --- status ---
    lda #0
    sta nsStatus
    rts

;==========================================================================
; _ns_get_version  (upstream: NS_GetVersion_Impl)
; Returns A = handler version. Preserve the upstream value (1).
_ns_get_version:
    lda #0x01
    rts

;==========================================================================
; _ns_get_base  (upstream: NS_GetBase_Impl)
; Upstream returns A:X = &NS_BeginStream (the start of its API jump table).
; EDGE intentionally returns 0 here -- NON-PARITY by design: EDGE exposes each
; routine as its own exported _ns_* symbol and has no API jump table, so an
; upstream-style "base" pointer is not meaningful to EDGE callers. Revisit only
; if a real consumer needs it.
_ns_get_base:
    lda #0
    ldx #0
    rts

;==========================================================================
; _ns_send_byte  (upstream: NS_SendByte_Impl) -- Stage 9I, pure TX enqueue.
; Input:  A = byte. Output: C=0 enqueued, C=1 TX full (not sent).
;
; TX producer. php/sei/plp critical section preserved (outLevel is shared with the
; future output-IRQ consumer; the count RMW must be atomic once IRQs are live).
; The carry status is set AFTER plp (the 9H fix: plp reloads the entry carry, so
; setting it earlier would be discarded).
;
; STAGED SEMANTIC DIFFERENCE vs upstream: upstream, when serial output is idle,
; primes the FIRST byte directly into POKEY SEROUT to kick off transmission.
; Stage 9I deliberately does NOT do that -- it enqueues EVERY accepted byte into
; the RAM ring and touches no POKEY/SEROUT. Real idle-prime + serialOutIdle + the
; output-IRQ drain are deferred to the IRQ/POKEY stage. Bytes are truthfully
; buffered (carry clear); actual transmission is deferred. Zeroed BSS is already a
; valid empty ring (outLevel=0, serialOutHead=0), so TX needs no explicit init.
_ns_send_byte:
    php
    sei
    pha                     ; save byte
    lda outLevel
    cmp #0x80               ; capacity = 128
    beq .Lsend_full
    ; enqueue at nsOutputBuffer[serialOutHead]
    pla                     ; A = byte
    ldx serialOutHead
    sta nsOutputBuffer,x
    inx
    txa
    and #0x7f               ; wrap producer index
    sta serialOutHead
    inc outLevel
    plp                     ; end critical section, THEN set carry
    clc                     ; enqueued
    rts
.Lsend_full:
    pla                     ; discard byte (not sent)
    plp                     ; end critical section, THEN set carry
    sec                     ; full
    rts

;==========================================================================
; _ns_recv_byte  (upstream: NS_RecvByte_Impl) -- Stage 9H, faithful translation.
; Output: A = byte, C=0 success / C=1 empty.
;
; RX consumer. php/sei/plp critical section preserved (serialInSpace is shared
; with the future input-IRQ producer; the 16-bit RMW must be atomic once IRQs are
; live -- sei touches only the CPU I-flag, no prohibited hardware). The read
; pointer is self-modifying code (Option A): `lda 0xffff` whose operand bytes are
; inReadPtr, advanced in place. Until ns_test_init_rx / begin initializes state,
; serialInSpace == serialInSize == 0 (zeroed BSS) -> the empty branch is taken
; and the SMC load at 0xffff is never reached: inert-safe when uninitialized.
_ns_recv_byte:
    php
    sei
    ; empty when free space == capacity (nothing buffered)
    lda serialInSpaceLo
    cmp serialInSize
    bne .Lrecv_not_empty
    lda serialInSpaceHi
    cmp serialInSize+1
    beq .Lrecv_empty
.Lrecv_not_empty:
    lda 0xffff              ; SMC: A = *inReadPtr
inReadPtr = . - 2
    pha                     ; hold the byte across pointer bookkeeping
    ; advance read pointer (16-bit inc of the SMC operand)
    inc inReadPtr
    bne .Lrecv_adv_done
    inc inReadPtr+1
.Lrecv_adv_done:
    ; wrap inReadPtr == inBufEnd -> inBuf
    lda inReadPtr
    cmp inBufEndLo
    bne .Lrecv_no_wrap
    lda inReadPtr+1
    cmp inBufEndHi
    bne .Lrecv_no_wrap
    lda inBufLo
    sta inReadPtr
    lda inBufHi
    sta inReadPtr+1
.Lrecv_no_wrap:
    ; free one byte of space (16-bit inc)
    inc serialInSpaceLo
    bne .Lrecv_space_done
    inc serialInSpaceHi
.Lrecv_space_done:
    pla                     ; A = received byte
    plp                     ; end critical section FIRST (see note), then set carry
    clc                     ; success
    rts
.Lrecv_empty:
    plp                     ; end critical section FIRST, then set carry
    sec                     ; empty
    rts
; NOTE (deliberate deviation from upstream literal sequence): upstream exits with
; `clc/sec; plp; rts`, but on a standard 6502 `plp` reloads the carry flag from the
; value `php` pushed at entry -- discarding the status carry. (Upstream gets away
; with it for routines whose result is in A/X, e.g. bytes_avail, but it breaks the
; carry contract for recv/send.) EDGE reorders to `plp` (restore the I-flag) then
; `clc/sec` so the empty/success status carry actually reaches the caller. A is
; preserved across plp.

;==========================================================================
; _ns_bytes_avail  (upstream: NS_BytesAvail_Impl) -- Stage 9H, faithful.
; Output: A = low, X = high (count of bytes currently buffered in RX).
; avail = serialInSize - serialInSpace (16-bit). Critical section preserved.
_ns_bytes_avail:
    php
    sei
    lda serialInSize
    sec
    sbc serialInSpaceLo
    tay
    lda serialInSize+1
    sbc serialInSpaceHi
    tax
    tya
    plp
    rts

;==========================================================================
; _ns_get_status  (upstream: NS_GetStatus_Impl)
; Output: A = sticky status, X = 0 (clobbered). CLEARS the sticky byte on read.
; Translated verbatim from upstream, including the IRQ-critical section. The
; php/sei/plp is inert here -- it only touches the CPU I-flag, none of the
; prohibited hardware -- and matches upstream so it is correct once the IRQ
; handler (a later stage) starts writing nsStatus. Until then nsStatus is always
; 0 (nothing writes it), so this returns 0.
_ns_get_status:
    php
    sei
    lda nsStatus            ; upstream serialErrors[0]
    ldx #0
    stx nsStatus            ; clear-on-read
    plp
    rts

;==========================================================================
; _ns_get_video_std  (upstream: NS_GetVideoStd_Impl)
; Output: A = 0 NTSC / 1 PAL. Reads translated state (0 until init writes it).
_ns_get_video_std:
    lda nsVideoStd
    rts

;==========================================================================
; _ns_init_netstream  (upstream: NS_InitNetstream_Impl)
; Output: C=0 success / C=1 failure (upstream contract).
; Scaffold: always failure (C=1) -- nothing is wired, and arg marshaling under
; the llvm-mos calling convention is deferred. Keeping carry=fail lets the
; future EDGE init wrapper surface success/failure (do NOT collapse to void).
_ns_init_netstream:
    sec
    rts

;==========================================================================
; _ns_get_final_flags / _ns_get_final_audf3 / _ns_get_final_audf4
; (upstream: NS_GetFinalFlags_Impl / AUDF3 / AUDF4)
; Output: A = cached value. Reads translated state (0 until init writes it).
_ns_get_final_flags:
    lda nsFinalFlags
    rts

_ns_get_final_audf3:
    lda nsFinalAudf3
    rts

_ns_get_final_audf4:
    lda nsFinalAudf4
    rts

;==========================================================================
; PRIVATE test-only hooks. Compiled only when EDGE_NETSTREAM_TEST_HOOKS is defined
; (the mos-sim behavioral tests). These are NOT part of the edge_ns_* ABI, NOT
; declared in fujinet_netstream_realtime_abi.h, and NOT called by production code.
; They mimic the still-deferred input/output IRQ producer/consumer so the rings can
; be driven under the simulator. (Buffer init is now done by _ns_begin_stream, so
; the former ns_test_init_rx/ns_test_init_tx helpers were removed in Stage 9J.)
#if defined(EDGE_NETSTREAM_TEST_HOOKS)
.global ns_test_rx_push
.global ns_test_tx_drain

; uint8_t ns_test_rx_push(uint8_t byte)
; Mimics the future input-IRQ producer (minus the `lda serin` hardware read and
; the overflow-status bit): stores byte at inPtr, advances+wraps, consumes one
; free-space slot. Returns A=0 pushed, A=1 full.
ns_test_rx_push:
    pha                        ; save byte arg
    lda serialInSpaceLo
    ora serialInSpaceHi
    bne .Lpush_have_space
    pla                        ; full: discard byte
    lda #1
    rts
.Lpush_have_space:
    pla                        ; A = byte
    sta 0xffff                 ; SMC: *inPtr = byte
inPtr = . - 2
    inc inPtr                  ; advance producer ptr (16-bit)
    bne .Lpush_adv_done
    inc inPtr+1
.Lpush_adv_done:
    lda inPtr                  ; wrap inPtr == inBufEnd -> inBuf
    cmp inBufEndLo
    bne .Lpush_no_wrap
    lda inPtr+1
    cmp inBufEndHi
    bne .Lpush_no_wrap
    lda inBufLo
    sta inPtr
    lda inBufHi
    sta inPtr+1
.Lpush_no_wrap:
    lda serialInSpaceLo        ; consume one free slot (16-bit dec)
    bne .Lpush_dec_lo
    dec serialInSpaceHi
.Lpush_dec_lo:
    dec serialInSpaceLo
    lda #0                     ; pushed
    rts

; uint16_t ns_test_tx_drain(void)
; Mimics the future output-IRQ consumer minus the `sta serout` hardware write:
; pulls the next byte from the TX ring. Returns A=byte (low), X=status (high);
; status 0 = byte returned, 1 = TX empty.
ns_test_tx_drain:
    lda outLevel
    bne .Ldrain_have
    lda #0
    ldx #1                     ; empty
    rts
.Ldrain_have:
    dec outLevel
    ldx outIndex
    lda nsOutputBuffer,x
    pha                        ; hold byte
    inx
    txa
    and #0x7f                  ; wrap consumer index
    sta outIndex
    pla                        ; A = byte
    ldx #0                     ; ok
    rts
#endif // EDGE_NETSTREAM_TEST_HOOKS

;==========================================================================
; Handler state (.bss) -- Stage 9F.
;
; Real Netstream-sized buffers and the minimal scalar state read by the low-risk
; getters above. Allocated now but NOT wired to any IRQ / SIO / send-recv path
; yet (deferred to later stages). All are internal (no .global): not part of the
; C ABI. Zero-initialized, so the getters observe 0 until later stages write them.
;
; Deferred state (added when their owning stages land): serialInSize,
; serialErrors[4] + serialConcurrentNum, in/out ring pointers + levels,
; VCount diagnostics, port + nominal-baud values.
.bss

nsStatus:       .zero 1     ; sticky status; upstream serialErrors[0]
nsVideoStd:     .zero 1     ; 0=NTSC, 1=PAL; upstream NetstreamVideoStd
nsFinalFlags:   .zero 1     ; upstream NetstreamFinalFlags
nsFinalAudf3:   .zero 1     ; upstream NetstreamFinalAUDF3
nsFinalAudf4:   .zero 1     ; upstream NetstreamFinalAUDF4

; Stage 9H RX ring scalars (8 bytes). The RX read/write pointers themselves are
; NOT here -- inReadPtr / inPtr are SMC operand bytes in .text (see above).
serialInSize:    .zero 2     ; RX capacity (=128); upstream serialInSize
serialInSpaceLo: .zero 1     ; RX free-space lo (recv frees / IRQ consumes)
serialInSpaceHi: .zero 1     ; RX free-space hi; upstream serialInSpaceHi
inBufLo:         .zero 1     ; RX buffer start lo; upstream inBufLo
inBufHi:         .zero 1     ; RX buffer start hi; upstream inBufHi
inBufEndLo:      .zero 1     ; RX buffer end lo; upstream inBufEndLo
inBufEndHi:      .zero 1     ; RX buffer end hi; upstream inBufEndHi

; Stage 9I TX ring scalars (3 bytes). TX is index-based (no pointers/SMC): both
; indices wrap with `and #0x7f` into nsOutputBuffer via absolute,X. Capacity is the
; constant 0x80, so there is no TX size variable. serialOutIdle (idle-prime flag)
; and outBuf (consumer base ptr) are deferred to the IRQ/POKEY stage.
serialOutHead:   .zero 1     ; TX producer index (send); upstream serialOutHead
outLevel:        .zero 1     ; TX count (send++ / output-IRQ--); upstream outLevel
outIndex:        .zero 1     ; TX consumer index; upstream outIndex (output IRQ / test drain)

; Stage 9J lifecycle state (2 bytes). serialOutIdle is TX-ring init state owned by
; begin (consumed later by the deferred idle-prime); serialConcurrentNum is the
; active flag (begin=1/end=0) -- NOT used to gate the byte API in this stage; it
; is forward-prep for the IRQ-stage error indexing and the adapter realtime_active().
serialOutIdle:       .zero 1 ; 0xff = TX idle; upstream serialOutIdle
serialConcurrentNum: .zero 1 ; 0 = inactive, 1 = active; upstream serialConcurrentNum

nsInputBuffer:  .zero NS_INPUT_BUFSIZE   ; RX, 128; upstream inputBuffer
nsOutputBuffer: .zero NS_OUTPUT_BUFSIZE  ; TX, 128; upstream outputBuffer0
nsPayloadBuf:   .zero NS_PAYLOAD_MAX     ; init payload, 64; upstream NetstreamPayloadBuf
