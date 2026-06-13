; engine/platform/atari/fujinet_netstream_handler.S
;
; Stage 9E: EDGE-owned, scaffolded translation of the upstream FujiNet Netstream
; concurrent handler into llvm-mos / GNU-as assembly.
;
; This is a SCAFFOLD ONLY. It exports the full upstream `_ns_*` symbol surface so
; the EDGE ABI shim (fujinet_netstream_realtime_abi.s) links cleanly under
; llvm-mos, but every routine is an INERT, defined-behavior stub:
;   - no IRQ vector install / swap
;   - no POKEY / SKCTL programming
;   - no SIO / DCB enable_netstream
;   - no ring buffers, no self-modifying code
;
; Real behavior (the NS_*_Impl bodies, including the upstream self-modifying-code
; design which we will preserve rather than refactor) is ported in later stages.
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
; _ns_begin_stream  (upstream: NS_BeginConcurrent_Impl)
; Scaffold: no-op. Real version installs IRQ vectors and enables POKEY IRQs.
_ns_begin_stream:
    rts

;==========================================================================
; _ns_end_stream  (upstream: NS_EndConcurrent_Impl)
; Scaffold: no-op. Real version restores vectors and deasserts the motor line.
_ns_end_stream:
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
; _ns_send_byte  (upstream: NS_SendByte_Impl)
; Input:  A = byte. Output: C=0 enqueued, C=1 full.
; Scaffold: always "full" (C=1) so the ABI shim reports not-sent.
_ns_send_byte:
    sec
    rts

;==========================================================================
; _ns_recv_byte  (upstream: NS_RecvByte_Impl)
; Output: A = byte, C=0 success / C=1 empty.
; Scaffold: always "empty" (C=1) so the ABI shim packs status=empty.
_ns_recv_byte:
    sec
    rts

;==========================================================================
; _ns_bytes_avail  (upstream: NS_BytesAvail_Impl)
; Output: A = low, X = high. Scaffold: zero available.
_ns_bytes_avail:
    lda #0
    ldx #0
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

nsInputBuffer:  .zero NS_INPUT_BUFSIZE   ; RX, 128; upstream inputBuffer
nsOutputBuffer: .zero NS_OUTPUT_BUFSIZE  ; TX, 128; upstream outputBuffer0
nsPayloadBuf:   .zero NS_PAYLOAD_MAX     ; init payload, 64; upstream NetstreamPayloadBuf
