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
;   - 9M/9N: IRQ dispatcher + serial IRQ handlers + vector swap/install/restore,
;         DetectPALViaVCOUNT (all translated; install/restore wired into begin/end).
;   - 9O.3a: begin/end do PAL detect + PACTL motor line + OS-vector install/restore.
;   - 9O.3b: begin programs POKEY serial mode (pokey_init copy, AUDF3/AUDF4, AUDCTL
;         0x28, SKCTL/SSKCTL) -- WITHOUT enabling POKMSK/IRQEN serial IRQs.
;   - 9O.4: begin ARMS the POKMSK/IRQEN serial-IRQ bits (LAST) + a hardwareActivated
;         re-entry guard; end DISARMS them (FIRST). Serial IRQs are now live.
;   - 9P: _ns_send_byte idle-primes -- on the idle->active edge (hardwareActivated &&
;         serialOutIdle) it writes the first queued byte to SEROUT to kick the output
;         IRQ chain, consumes it, and clears serialOutIdle. begin/end are UNTOUCHED
;         and SerialOutputIrqHandler is byte-for-byte unchanged.
; Still INERT / deferred (later stages):
;   - outBuf consumer-base SMC (EDGE stays index-based; not needed)
;   - _ns_init_netstream / SIO baud selection (9Q)
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
; _ns_begin_stream  (upstream: NS_BeginConcurrent_Impl).
;
; Stage 9O.4: begin = [re-entry guard] -> RAM init (ns_begin_soft) -> PAL/NTSC detect
; -> assert the PACTL motor/control line -> program POKEY for serial mode
; (AUDF/AUDCTL/SKCTL) -> install OS vectors -> ARM the POKMSK/IRQEN serial-IRQ bits
; (LAST). PACTL 0x34 is the staged translation of upstream's motor-on value (cassette/
; SIO motor line asserted for the concurrent session). ns_enable_serial_irqs is the
; final step so the handlers are installed before any serial IRQ can fire. The early
; hardwareActivated guard makes a second begin (while a stream is live) a no-op rather
; than letting ns_begin_soft reset the rings under the running IRQ handlers.
; ns_install_vectors is idempotent (vectorInstalled). NOTE: _ns_begin_stream writes
; the OS-vector page + PACTL + POKEY + IRQEN -- under mos-sim the vector page is the
; loaded program image, so it must NOT be called from sim tests (they use
; ns_test_begin_soft); runtime validation of real begin is Altirra/hardware only.
_ns_begin_stream:
    ; --- 9O.4: early re-entry guard ---
    ; If serial IRQs are already armed (a stream is live), return immediately WITHOUT
    ; re-running ns_begin_soft -- ns_init_ring_state would reset the RX/TX rings out
    ; from under the running input/output IRQ handlers (double-begin race). The
    ; hardwareActivated guard inside ns_enable_serial_irqs alone is not enough because
    ; ns_begin_soft runs before it; this guard protects the soft init too.
    php
    sei
    lda hardwareActivated
    beq .Lbegin_not_active
    plp                          ; already streaming -> no-op, leave rings/hardware intact
    rts
.Lbegin_not_active:
    plp
    jsr ns_begin_soft
    jsr DetectPALViaVCOUNT       ; sets nsVideoStd (overrides ns_begin_soft's 0 default)
    lda #0x34
    sta NS_PACTL                 ; motor/control line ON (upstream 0x34)
    ; --- 9O.3b: POKEY serial-mode config (NO serial-IRQ enable; that is 9O.4) ---
    ; Program POKEY for concurrent serial mode without touching POKMSK/IRQEN. Copy the
    ; 9-byte pokey_init table to AUDF1..AUDCTL ($D200..$D208), then override the joined
    ; channel-3/4 divisors and AUDCTL.
    ldx #8
.Lbegin_pokey:
    lda pokey_init,x            ; 9 zero bytes -> NS_POKEY_BASE+0..8 ($D200..$D208)
    sta NS_POKEY_BASE,x
    dex
    bpl .Lbegin_pokey
    ; AUDF3/AUDF4 are the per-stream baud divisors (joined 16-bit ch3+4 timer).
    ; TRANSITIONAL (9O.3b): nsFinalAudf3 / nsFinalAudf4 / nsFinalFlags are ALL 0 today
    ; because _ns_init_netstream / SIO / baud selection is deferred to 9Q. Stage 9O.3b
    ; intentionally writes the CURRENT (zero) values verbatim -- it does NOT invent a
    ; placeholder baud default. This is safe in the staged build because the POKMSK /
    ; IRQEN serial enable remains deferred to 9O.4, so no serial transfer is clocked
    ; from these timing values yet. Production use MUST call _ns_init_netstream (once
    ; 9Q exists) before _ns_begin_stream so these hold the real selected baud.
    lda nsFinalAudf3
    sta NS_AUDF3
    lda nsFinalAudf4
    sta NS_AUDF4
    lda #0x28                   ; 1.79MHz clock, join ch3+4 (upstream audctl value)
    sta NS_AUDCTL
    ; SKCTL/SSKCTL serial timing selected from nsFinalFlags bits 0x0c (upstream-equiv):
    ;   flags & 0x0c == 0x00 -> 0x30  (RX internal, TX internal)
    ;                == 0x04 -> 0x10  (RX internal, TX external)
    ;                == 0x08 -> 0x40  (RX external, TX internal)
    ;                == 0x0c -> 0x00  (RX external, TX external)  [see UPSTREAM QUIRK]
    ; The mapped bits are OR'd into the existing SKCTL OS shadow (NS_SSKCTL) and the
    ; result stored to both the shadow and the hardware SKCTL.
    lda nsFinalFlags
    and #0x0c
    beq .Lsk_int_int            ; 0x00: RX int, TX int
    cmp #0x04
    beq .Lsk_int_ext            ; 0x04: RX int, TX ext
    cmp #0x08
    beq .Lsk_ext_int            ; 0x08: RX ext, TX int
    ; 0x0c (RX ext, TX ext): UPSTREAM QUIRK preserved. Upstream codes this as
    ; `lda #$00 / bne skctl_apply`, but `lda #$00` sets Z, so the `bne` is never taken
    ; and execution falls through into the int/int case -- so upstream actually applies
    ; 0x30 here, not 0x00. We translate that literal behavior. It is unreachable at this
    ; stage anyway (nsFinalFlags == 0 until 9Q), so only the int/int path runs today.
    lda #0x00
    bne .Lsk_apply              ; A==0 -> not taken -> falls through (as upstream)
.Lsk_int_int:
    lda #0x30                   ; %011 RX int, TX int
    bne .Lsk_apply
.Lsk_int_ext:
    lda #0x10                   ; %001 RX int, TX ext
    bne .Lsk_apply
.Lsk_ext_int:
    lda #0x40                   ; %100 RX ext, TX int
.Lsk_apply:
    ora NS_SSKCTL               ; merge with existing SKCTL OS shadow ($0232)
    sta NS_SSKCTL               ; update shadow
    sta NS_SKCTL                ; HW write $D20F serial port control
    jsr ns_install_vectors
    jsr ns_enable_serial_irqs   ; 9O.4: POKMSK/IRQEN serial enable LAST, after vectors+POKEY
    rts

;==========================================================================
; _ns_end_stream  (upstream: NS_EndConcurrent_Impl).
;
; Stage 9O.4: end = DISARM the POKMSK/IRQEN serial-IRQ bits (FIRST) -> remove OS
; vectors (ns_restore_vectors) -> deassert the PACTL motor line -> reset RAM state
; (ns_end_soft). ns_disable_serial_irqs runs first so no serial IRQ can vector through
; handlers that are about to be torn down. PACTL 0x3c is the staged translation of
; upstream's motor-off value. ns_disable_serial_irqs / ns_restore_vectors are safe
; no-ops if never armed/installed (hardwareActivated / vectorInstalled guards), so a
; double-end is stable. Same mos-sim caveat as begin: do not call from sim tests.
_ns_end_stream:
    jsr ns_disable_serial_irqs   ; 9O.4: disable serial IRQs FIRST, before vector teardown
    jsr ns_restore_vectors
    lda #0x3c
    sta NS_PACTL                 ; motor/control line OFF (upstream 0x3c)
    jsr ns_end_soft
    rts

;==========================================================================
; ns_begin_soft (internal) -- the HARDWARE-FREE part of begin: initialize ALL
; RX/TX/status RAM to empty-ready and mark the stream active. No hardware/OS/vector
; writes. Shared by _ns_begin_stream and the test-only ns_test_begin_soft so mos-sim
; tests can set up rings without triggering the (later) vector install, which would
; write the OS-vector page (= the sim program image) and crash.
ns_begin_soft:
    jsr ns_init_ring_state      ; RX+TX+status+serialOutIdle -> empty-ready
    lda #0
    sta nsVideoStd              ; NTSC default; real PAL detection deferred (9O.3)
    lda #1
    sta serialConcurrentNum     ; mark active (NOT used to gate the byte API)
    rts

;==========================================================================
; ns_end_soft (internal) -- the HARDWARE-FREE part of end: full ring re-init to a
; safe empty state, marked inactive. No hardware/OS/vector writes.
ns_end_soft:
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
    sta inReadPtr               ; patch SMC operand (RX consumer ptr; in _ns_recv_byte)
    sta inPtr                   ; patch SMC operand (RX producer ptr; in ns_rx_produce)
    sty inBufHi
    sty inReadPtr+1
    sty inPtr+1
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
    sta serialOutIdle           ; idle (0xff); consumed by the 9P idle-prime in _ns_send_byte
    ; --- status: clear all four serialErrors bytes ---
    lda #0
    ldx #3
.Lclr_err:
    sta serialErrors,x
    dex
    bpl .Lclr_err
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
; Stage 9P IDLE-PRIME: after enqueuing the byte, if serial IRQs are armed
; (hardwareActivated) AND the transmitter is idle (serialOutIdle bit7 set), prime
; the FIRST queued byte directly into POKEY SEROUT to kick off transmission -- one
; inline SerialOutputIrqHandler drain iteration (dec outLevel, sta SEROUT, advance
; outIndex) plus clear serialOutIdle. The installed output-ready IRQ then drains the
; rest; when the ring empties SerialOutputIrqHandler re-sets serialOutIdle and stops
; (no further SEROUT write), matching the 9O.4-validated no-storm quiescence. When
; NOT armed/idle the byte is simply buffered (carry clear), exactly as in 9I. This is
; the ONLY SEROUT write outside SerialOutputIrqHandler. The php/sei critical section
; keeps the outLevel/outIndex/serialOutIdle RMW atomic vs the output IRQ; carry is
; set AFTER plp (the 9H fix). Zeroed BSS is already a valid empty ring (outLevel=0,
; serialOutHead=0), so TX needs no explicit init.
;
; A test-only mirror (ns_test_last_prime_byte / ns_test_prime_count, compiled only
; under EDGE_NETSTREAM_TEST_HOOKS) records the primed byte right next to the
; sta SEROUT so tests/probe observe the prime via RAM -- a CPU read of $D20D returns
; SERIN, not the SEROUT latch, so it cannot prove what was primed.
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
    ; --- 9P idle-prime (inside the php/sei critical section) ---
    lda hardwareActivated
    beq .Lsend_done         ; serial IRQs not armed -> just buffer (no prime)
    bit serialOutIdle
    bpl .Lsend_done         ; bit7 clear -> output chain already running, no prime
    dec outLevel            ; consume the byte we are about to prime
    ldx outIndex
    lda nsOutputBuffer,x
    sta NS_SEROUT           ; HW $D20D: prime first byte -> kicks the output-ready IRQ chain
#if defined(EDGE_NETSTREAM_TEST_HOOKS)
    sta ns_test_last_prime_byte ; mirror primed byte (A still = byte); $D20D reads are SERIN
    inc ns_test_prime_count
#endif
    inx
    txa
    and #0x7f               ; advance consumer index, wrapped
    sta outIndex
    lda #0
    sta serialOutIdle       ; transmitter now active (output IRQ drains the rest)
.Lsend_done:
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
; Output: A = status, X = 0 (clobbered). CLEARS the status byte on read.
; Reads+clears serialErrors[0] (upstream behavior). The php/sei/plp critical
; section is inert here -- it only touches the CPU I-flag, no prohibited hardware
; -- and matches upstream so it stays correct once the input-IRQ overflow path
; (a later stage) starts setting serialErrors. Until then serialErrors[0] is 0.
_ns_get_status:
    php
    sei
    lda serialErrors        ; serialErrors[0]
    ldx #0
    stx serialErrors        ; clear-on-read
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
; IRQ dispatcher + vector swap -- Stage 9M.1. TRANSLATED BUT UNINSTALLED.
;
; These are NOT reachable in this stage: begin/end do not install them, no VIMIRQ
; vector points at them, and the production adapter never calls them. They sit as
; dead code until the vector-install stage (9N). IrqHandler contains real IRQ
; hardware access (IRQST/IRQEN) but never executes while uninstalled; do NOT call
; it from mos-sim tests. SwapIrqVector touches only the VIMIRQ OS vector + the
; chain_addr SMC operand (no hardware registers) and is exercised by a gated test.

;==========================================================================
; IrqHandler (upstream) -- immediate-IRQ dispatcher installed at VIMIRQ.
; If the BREAK-key IRQ is pending, ack it; otherwise chain to the previously
; installed handler via the self-modified jmp operand (chain_addr).
IrqHandler:
    bit NS_IRQST            ; HW read $D20E
    bpl IH_is_break
    jmp 0xffff              ; SMC: chain to old handler (operand patched by SwapIrqVector)
chain_addr = . - 2
IH_is_break:
    pha
    lda #0x7f
    sta NS_IRQEN            ; HW write $D20E
    lda NS_POKMSK           ; OS shadow $0010
    sta NS_IRQEN            ; HW write
    pla
    rti

;==========================================================================
; SwapIrqVector (upstream) -- exchange the 2-byte VIMIRQ with chain_addr.
; Internal (not exported in the ABI header / not in Game::net). begin/end will
; jsr this from the same translation unit in the vector-install stage (9N).
; Touches only VIMIRQ (OS RAM) and chain_addr (the .text SMC operand) -- no
; hardware registers -- so it is safe to exercise under mos-sim.
SwapIrqVector:
    ldx #1
.Lswap_loop:
    lda NS_VIMIRQ,x
    pha
    lda chain_addr,x
    sta NS_VIMIRQ,x
    pla
    sta chain_addr,x
    dex
    bpl .Lswap_loop
    rts

;==========================================================================
; SerialOutputIrqHandler (upstream) -- TX output-ready IRQ consumer.
; Stage 9M.2: TRANSLATED BUT UNINSTALLED. Entered via VSEROR only once installed
; (deferred to 9N); never reached now. Entry assumes the OS pushed A before
; vectoring here (balanced by the `pla; rti` at SOH_xit).
;
; EDGE is index-based: outIndex addresses nsOutputBuffer via absolute,X -- the
; upstream `outBuf` SMC base pointer is NOT used. No new SMC operands. Only the TX
; state outLevel/outIndex/serialOutIdle is touched. This routine contains the only
; SEROUT write in the file; it is unreachable from begin/end/production.
SerialOutputIrqHandler:
    lda outLevel
    beq SOH_is_empty
    dec outLevel
    txa
    pha
    ldx outIndex
    lda nsOutputBuffer,x
    sta NS_SEROUT               ; HW write $D20D (uninstalled: never executes)
    inx
    txa
    and #0x7f                   ; advance consumer index, wrapped
    sta outIndex
    pla
    tax
SOH_xit:
    pla                         ; restore A pushed by the OS IRQ dispatch
    rti
SOH_is_empty:
    sec
    ror serialOutIdle           ; set idle (bit7) -- upstream-equivalent
    bne SOH_xit

; Serial output-complete IRQ shares the output handler's exit (upstream alias).
SerialCompleteIrqHandler = SOH_xit

;==========================================================================
; ns_rx_produce (internal, always compiled) -- shared RX producer.
; Input:  A = received byte.
; Output: C=0 enqueued, C=1 full/overflow. (Internal carry convention only -- NOT
;         public ABI.) Preserves X and Y (safe to call from the input IRQ).
;
; This is the permanent home of the RX PRODUCER pointer inPtr (the SMC operand of
; the `sta` below). Both the real SerialInputIrqHandler and the test-only
; ns_test_rx_push call this -- one state model, one inPtr. Touches only RX BSS
; (serialInSpace/inBuf*/serialErrors/serialConcurrentNum) + the inPtr operand; no
; hardware registers.
ns_rx_produce:
    pha                         ; save the byte (do not lose A)
    lda serialInSpaceLo
    ora serialInSpaceHi
    beq .Lprod_full
    ; --- space available: enqueue ---
    pla                         ; A = byte
    sta 0xffff                  ; SMC: *inPtr = byte
inPtr = . - 2
    inc inPtr                   ; advance producer ptr (16-bit)
    bne .Lprod_adv
    inc inPtr+1
.Lprod_adv:
    lda inPtr                   ; wrap inPtr == inBufEnd -> inBuf
    cmp inBufEndLo
    bne .Lprod_nowrap
    lda inPtr+1
    cmp inBufEndHi
    bne .Lprod_nowrap
    lda inBufLo
    sta inPtr
    lda inBufHi
    sta inPtr+1
.Lprod_nowrap:
    lda serialInSpaceLo         ; consume one free slot (16-bit dec)
    bne .Lprod_declo
    dec serialInSpaceHi
.Lprod_declo:
    dec serialInSpaceLo
    clc                         ; enqueued
    rts
    ; --- full: set overflow status, ring unchanged ---
.Lprod_full:
    pla                         ; discard the byte (not stored)
    txa                         ; preserve X (6502-safe; no PHX)
    pha
    ldx serialConcurrentNum
    beq .Lprod_full_noerr       ; serialConcurrentNum==0: no active channel, skip (safe)
    dex                         ; index = serialConcurrentNum - 1
    lda serialErrors,x
    ora #0x10                   ; upstream overflow bit (bit 4)
    sta serialErrors,x
.Lprod_full_noerr:
    pla                         ; restore X
    tax
    sec                         ; full/overflow
    rts

;==========================================================================
; SerialInputIrqHandler (upstream) -- RX input-ready IRQ producer.
; Stage 9M.3: TRANSLATED BUT UNINSTALLED. Entered via VSERIN only once installed
; (deferred to 9N); never reached now. Entry assumes the OS pushed A (balanced by
; the `pla; rti`). Reads the received byte from SERIN and feeds ns_rx_produce
; (which preserves X/Y). This routine contains the only SERIN read in the file;
; it is unreachable from begin/end/production and is never sim-called.
SerialInputIrqHandler:
    lda NS_SERIN                ; HW read $D20D (uninstalled: never executes)
    jsr ns_rx_produce           ; A=byte; preserves X,Y; carry status ignored here
    pla                         ; restore A pushed by the OS IRQ dispatch
    rti

;==========================================================================
; DetectPALViaVCOUNT (upstream) -- PAL/NTSC detection. Stage 9M.4: TRANSLATED BUT
; UNCALLED. begin will call it in the POKEY-activation stage (9O); nothing calls it
; now. The only VCOUNT read in the file lives here.
;
; VCOUNT is ANTIC's scanline/2 counter. Sample it across (up to) one full frame:
; track the max seen and stop early once it wraps (current < previous). NTSC tops
; out ~131, PAL ~156, so max>=150 => PAL. The sampling loop is BOUNDED by the
; nested 8-bit counters (<= 255*255 iterations) and also exits early on wrap, so it
; cannot spin forever even if VCOUNT never changes (e.g. under mos-sim). Sets
; nsVideoStd = 0 (NTSC) / 1 (PAL). Clobbers A/X/Y (ordinary subroutine, not an IRQ);
; touches no ring/POKEY/SIO/vector state.
DetectPALViaVCOUNT:
    lda NS_VCOUNT
    sta NetstreamVCountPrev
    sta NetstreamVCountMax
    ldx #0xff
    ldy #0xff
.Ldp_loop:
    lda NS_VCOUNT
    cmp NetstreamVCountMax
    bcc .Ldp_no_max
    sta NetstreamVCountMax
.Ldp_no_max:
    cmp NetstreamVCountPrev
    bcc .Ldp_wrapped            ; current < previous -> frame wrapped, done
    sta NetstreamVCountPrev
    dey
    bne .Ldp_loop
    dex
    bne .Ldp_loop               ; bounded: <= 255*255 samples
.Ldp_wrapped:
    lda NetstreamVCountMax
    cmp #150
    bcc .Ldp_ntsc
    lda #1                      ; PAL
    bne .Ldp_store
.Ldp_ntsc:
    lda #0                      ; NTSC
.Ldp_store:
    sta nsVideoStd
    rts

;==========================================================================
; Serial IRQ vector table -- Stage 9N. Laid out to match the OS vector byte order
; VSERIN/VSEROR/VSEROC so a single index drives both save and install. Local data
; (not executed; reached via indexed loads only).
serialVecs:
    .word SerialInputIrqHandler     ; -> VSERIN  ($020A)
    .word SerialOutputIrqHandler    ; -> VSEROR  ($020C)
    .word SerialCompleteIrqHandler  ; -> VSEROC  ($020E)

;==========================================================================
; pokey_init -- Stage 9O.3b. The 9-byte POKEY serial-mode init image copied to
; AUDF1..AUDCTL ($D200..$D208) by _ns_begin_stream. All zero (upstream pokey_init):
; AUDF3/AUDF4/AUDCTL are overridden immediately after the copy with the per-stream
; baud divisors and the 0x28 AUDCTL value. Read-only data in .text (indexed loads
; only) -- NOT .bss; keeps the handler .bss footprint unchanged.
pokey_init:
    .byte 0x00      ; AUDF1  ($D200)
    .byte 0x00      ; AUDC1  ($D201)
    .byte 0x00      ; AUDF2  ($D202)
    .byte 0x00      ; AUDC2  ($D203)
    .byte 0x00      ; AUDF3  ($D204, overridden from nsFinalAudf3)
    .byte 0x00      ; AUDC3  ($D205)
    .byte 0x00      ; AUDF4  ($D206, overridden from nsFinalAudf4)
    .byte 0x00      ; AUDC4  ($D207)
    .byte 0x00      ; AUDCTL ($D208, overridden to 0x28)

;==========================================================================
; ns_install_vectors / ns_restore_vectors -- Stage 9N. Internal, always compiled,
; but NOT called by begin/end yet (9O wires them in). They touch ONLY OS-vector RAM
; ($020A-$020F via NS_VSERIN, and VIMIRQ via SwapIrqVector) + serialVecSave +
; chain_addr + vectorInstalled -- no POKEY/SKCTL/AUDCTL/PACTL, no IRQEN/POKMSK, no
; SEROUT, no SIOV. php/sei/plp is a CPU-only critical section (the vector swap must
; be atomic vs any IRQ). Guarded by vectorInstalled so double-install /
; double-restore / restore-without-install are all safe.

; void ns_install_vectors(void)
ns_install_vectors:
    php
    sei
    lda vectorInstalled
    bne .Linst_done             ; already installed -> no double-install
    ldx #5
.Linst_loop:
    lda NS_VSERIN,x             ; save old VSERIN/VSEROR/VSEROC
    sta serialVecSave,x
    lda serialVecs,x            ; install handler vectors
    sta NS_VSERIN,x
    dex
    bpl .Linst_loop
    ; seed the chain target with our dispatcher BEFORE swapping it into VIMIRQ.
    ; REQUIRED: IrqHandler assembles `jmp 0xffff`, so chain_addr's initial value is
    ; 0xFFFF, not &IrqHandler -- without this seed the swap would put 0xFFFF in VIMIRQ.
    lda #<IrqHandler
    sta chain_addr
    lda #>IrqHandler
    sta chain_addr+1
    jsr SwapIrqVector           ; VIMIRQ := &IrqHandler ; chain_addr := old VIMIRQ
    lda #1
    sta vectorInstalled
.Linst_done:
    plp
    rts

; void ns_restore_vectors(void)
ns_restore_vectors:
    php
    sei
    lda vectorInstalled
    beq .Lrest_done             ; not installed -> safe no-op (chain_addr would be stale)
    ldx #5
.Lrest_loop:
    lda serialVecSave,x         ; restore old VSERIN/VSEROR/VSEROC
    sta NS_VSERIN,x
    dex
    bpl .Lrest_loop
    jsr SwapIrqVector           ; VIMIRQ := chain_addr (old VIMIRQ) ; chain_addr := &IrqHandler
    lda #0
    sta vectorInstalled
.Lrest_done:
    plp
    rts

;==========================================================================
; ns_enable_serial_irqs / ns_disable_serial_irqs -- Stage 9O.4. Internal, always
; compiled. Arm / disarm the POKEY serial-IRQ bits in the POKMSK shadow ($0010) and
; the hardware IRQEN register ($D20E), gated by hardwareActivated (a re-entry guard
; that mirrors vectorInstalled). These are the FINAL hardware step of begin and the
; FIRST of end: _ns_begin_stream calls enable AFTER ns_install_vectors (so the
; handlers are in place before any serial IRQ can fire), _ns_end_stream calls disable
; BEFORE ns_restore_vectors (so no serial IRQ can vector through torn-down handlers).
;
; php/sei/plp is a CPU-only critical section that makes the POKMSK read-modify-write +
; IRQEN write atomic vs an IRQ. Deliberate deviation from upstream's unconditional
; `cli`: plp RESTORES the caller's I-flag (enabled in normal C context), so serial
; IRQs are live on return without forcing them on regardless of caller state.
;
; Upstream asymmetry preserved: enable ORs 0x30 (SerInputReady + SerOutputReady);
; disable ANDs 0xc7 (clears bits 3,4,5 = 0x38, incl. SerOutputComplete) and leaves
; every other IRQ bit untouched. NS_POKMSK ($0010) is the only zero-page write here;
; NS_IRQEN ($D20E) is the only IRQEN write in begin/end (the IrqHandler break path
; also writes IRQEN, but it is uninstalled).

; void ns_enable_serial_irqs(void) -- arm serial IRQs. Guarded by hardwareActivated.
ns_enable_serial_irqs:
    php
    sei
    lda hardwareActivated
    bne .Len_done               ; already armed -> no double-enable
    lda NS_POKMSK
    ora #0x30                   ; SerInputReady + SerOutputReady (upstream ora #$30)
    sta NS_POKMSK               ; OS shadow $0010
    sta NS_IRQEN                ; HW $D20E (write-only)
    lda #1
    sta hardwareActivated
.Len_done:
    plp
    rts

; void ns_disable_serial_irqs(void) -- disarm serial IRQs. Guarded by hardwareActivated.
ns_disable_serial_irqs:
    php
    sei
    lda hardwareActivated
    beq .Ldis_done              ; not armed -> safe no-op (touches nothing)
    lda NS_POKMSK
    and #0xc7                   ; clear serial bits 0x38; preserve all other IRQ bits (upstream and #$c7)
    sta NS_POKMSK
    sta NS_IRQEN
    lda #0
    sta hardwareActivated
.Ldis_done:
    plp
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
.global ns_test_set_status
.global ns_test_swap_irq_vector
.global ns_test_detect_pal
.global ns_test_install_vectors
.global ns_test_restore_vectors
.global ns_test_enable_serial_irqs
.global ns_test_disable_serial_irqs
.global ns_test_begin_soft
.global ns_test_end_soft
.global ns_test_set_hw_activated
; 9P: expose the idle-prime mirror + the TX/gate state so the mos-sim test and the
; Altirra probe can snapshot the prime via RAM (a CPU read of $D20D returns SERIN,
; not the SEROUT latch). Declarations only -- no behavior change.
.global ns_test_last_prime_byte
.global ns_test_prime_count
.global serialOutIdle
.global outLevel
.global outIndex
.global hardwareActivated

; void ns_test_begin_soft(void) / ns_test_end_soft(void)
; Test-only entries to the HARDWARE-FREE begin/end bodies. Sim tests use these for
; ring setup/reset instead of _ns_begin_stream/_ns_end_stream, which (from 9O.2 on)
; install the OS vectors -- writing the OS-vector page that mos-sim loads code into.
ns_test_begin_soft:
    jsr ns_begin_soft
    rts
ns_test_end_soft:
    jsr ns_end_soft
    rts

; void ns_test_install_vectors(void) / ns_test_restore_vectors(void)
; Test-only entries to the internal install/restore routines (kept unexported). Let
; the mos-sim test drive vector install/restore (OS vectors are RAM under sim).
ns_test_install_vectors:
    jsr ns_install_vectors
    rts
ns_test_restore_vectors:
    jsr ns_restore_vectors
    rts

; void ns_test_enable_serial_irqs(void) / ns_test_disable_serial_irqs(void)
; Test-only entries to the internal 9O.4 arm/disarm routines (kept unexported). NOTE:
; the full arm path writes IRQEN ($D20E) and the POKMSK shadow ($0010, which collides
; with llvm-mos zero-page usage under mos-sim), so the ENABLE wrapper is for Altirra
; use only -- do NOT call it from sim tests. The DISABLE wrapper with hardwareActivated
; == 0 early-returns before touching any hardware, so it is the sim-safe guard smoke.
ns_test_enable_serial_irqs:
    jsr ns_enable_serial_irqs
    rts
ns_test_disable_serial_irqs:
    jsr ns_disable_serial_irqs
    rts

; void ns_test_detect_pal(void)
; Test-only entry to the internal DetectPALViaVCOUNT (stays unexported). The routine
; is bounded, so the sim smoke can call it and assert it returns + nsVideoStd in 0/1.
ns_test_detect_pal:
    jsr DetectPALViaVCOUNT
    rts

; void ns_test_swap_irq_vector(void)
; Test-only entry to the internal SwapIrqVector (which stays unexported). Lets the
; mos-sim test verify the VIMIRQ<->chain_addr swap is an involution without putting
; SwapIrqVector in the ABI.
ns_test_swap_irq_vector:
    jsr SwapIrqVector
    rts

; void ns_test_set_status(uint8_t value)
; Seeds serialErrors[0] so tests can exercise the get_status read+clear model in
; place of the still-deferred input-IRQ overflow path.
ns_test_set_status:
    sta serialErrors
    rts

; void ns_test_set_hw_activated(uint8_t value)
; Sets the hardwareActivated gate (RAM-only) so the sim test / probe can arm the 9P
; idle-prime WITHOUT calling ns_enable_serial_irqs (which writes POKMSK/IRQEN --
; unsafe under mos-sim). Touches no hardware.
ns_test_set_hw_activated:
    sta hardwareActivated
    rts

; uint8_t ns_test_rx_push(uint8_t byte)
; Thin test wrapper over the shared producer ns_rx_produce (same path the real
; input IRQ uses). Converts the internal carry status to the existing A=0/1 return
; (0 = pushed, 1 = full). On full, ns_rx_produce also sets the overflow status bit.
ns_test_rx_push:
    jsr ns_rx_produce          ; C=0 pushed / C=1 full
    lda #0
    bcc .Lrxp_done
    lda #1
.Lrxp_done:
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
; Deferred state (added when their owning stages land): port + nominal-baud +
; BaudTable (SIO). (chain_addr, serialVecs and pokey_init now exist in .text.)
.bss

; Status/error model (Stage 9L): serialErrors[4] replaces the former 1-byte
; nsStatus stand-in. _ns_get_status reads+clears serialErrors[0]; the future input
; IRQ sets serialErrors[serialConcurrentNum-1] on RX overflow. begin/end clear all 4.
serialErrors:   .zero 4     ; upstream serialErrors[4] (overflow status, IRQ-written)
nsVideoStd:     .zero 1     ; 0=NTSC, 1=PAL; upstream NetstreamVideoStd
nsFinalFlags:   .zero 1     ; upstream NetstreamFinalFlags
nsFinalAudf3:   .zero 1     ; upstream NetstreamFinalAUDF3
nsFinalAudf4:   .zero 1     ; upstream NetstreamFinalAUDF4

; Stage 9L hardware-lifecycle state (declared ahead of its consumers; no HW yet):
; serialVecSave saves vserin..vseroc before the (deferred) vector install;
; NetstreamVCountMax/Prev are DetectPALViaVCOUNT scratch (deferred).
serialVecSave:       .zero 6 ; saved serial IRQ vectors; upstream serialVecSave
NetstreamVCountMax:  .zero 1 ; DetectPALViaVCOUNT scratch (deferred)
NetstreamVCountPrev: .zero 1 ; DetectPALViaVCOUNT scratch (deferred)

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
; constant 0x80, so there is no TX size variable. serialOutIdle (idle-prime flag) is
; consumed by the 9P idle-prime in _ns_send_byte; outBuf (consumer base ptr) is not
; used (EDGE stays index-based).
serialOutHead:   .zero 1     ; TX producer index (send); upstream serialOutHead
outLevel:        .zero 1     ; TX count (send++ / output-IRQ--); upstream outLevel
outIndex:        .zero 1     ; TX consumer index; upstream outIndex (output IRQ / test drain)

; Stage 9J lifecycle state (2 bytes). serialOutIdle is TX-ring init state owned by
; begin (consumed by the 9P idle-prime in _ns_send_byte); serialConcurrentNum is the
; active flag (begin=1/end=0) -- NOT used to gate the byte API in this stage; it
; is forward-prep for the IRQ-stage error indexing and the adapter realtime_active().
serialOutIdle:       .zero 1 ; 0xff = TX idle; upstream serialOutIdle
serialConcurrentNum: .zero 1 ; 0 = inactive, 1 = active; upstream serialConcurrentNum

; Stage 9N: tracks whether the OS serial/IRQ vectors are physically installed.
; HARDWARE state, NOT ring state: modified ONLY by ns_install_vectors (->1) and
; ns_restore_vectors (->0). Deliberately NOT cleared by ns_init_ring_state, so a
; hardware-free begin/end cannot desync it from the real vector state (matters once
; 9O makes begin/end install/restore). BSS zero-init = not installed. Not a byte-API gate.
vectorInstalled:     .zero 1 ; 0 = vectors not installed, 1 = installed

; Stage 9O.4: tracks whether the POKEY serial IRQs are armed in POKMSK/IRQEN.
; HARDWARE state, NOT ring state: written ONLY by ns_enable_serial_irqs (->1) and
; ns_disable_serial_irqs (->0). Deliberately NOT cleared by ns_init_ring_state /
; ns_begin_soft / ns_end_soft, so the hardware-free soft path cannot desync it from
; the real IRQEN state -- that would defeat the early re-entry guard in _ns_begin_stream
; (and let a double-begin reset the rings under live IRQ handlers). BSS zero = disarmed.
hardwareActivated:   .zero 1 ; 0 = serial IRQs disarmed, 1 = armed

nsInputBuffer:  .zero NS_INPUT_BUFSIZE   ; RX, 128; upstream inputBuffer
nsOutputBuffer: .zero NS_OUTPUT_BUFSIZE  ; TX, 128; upstream outputBuffer0
nsPayloadBuf:   .zero NS_PAYLOAD_MAX     ; init payload, 64; upstream NetstreamPayloadBuf

#if defined(EDGE_NETSTREAM_TEST_HOOKS)
; 9P test-only mirror: the idle-prime in _ns_send_byte writes the primed byte here
; (right next to the sta SEROUT) and bumps the counter, so tests/probe observe the
; prime through RAM instead of reading $D20D (a CPU read of $D20D returns SERIN).
ns_test_last_prime_byte: .zero 1
ns_test_prime_count:     .zero 1
#endif // EDGE_NETSTREAM_TEST_HOOKS
