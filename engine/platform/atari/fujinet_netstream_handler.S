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
; Returns A:X = address of the API jump table base. Scaffold returns null.
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
; Output: A = status (cleared on read). Scaffold: no errors.
_ns_get_status:
    lda #0
    rts

;==========================================================================
; _ns_get_video_std  (upstream: NS_GetVideoStd_Impl)
; Output: A = 0 NTSC / 1 PAL. Scaffold: 0.
_ns_get_video_std:
    lda #0
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
; Output: A = cached value. Scaffold: 0.
_ns_get_final_flags:
    lda #0
    rts

_ns_get_final_audf3:
    lda #0
    rts

_ns_get_final_audf4:
    lda #0
    rts
