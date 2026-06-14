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
; _ns_begin_stream  (upstream: NS_BeginConcurrent_Impl).
;
; Stage 9O.3a: begin = RAM init (ns_begin_soft) -> PAL/NTSC detect -> assert the
; PACTL motor/control line -> install OS vectors. PACTL 0x34 is the staged
; translation of upstream's motor-on value (cassette/SIO motor line asserted for the
; concurrent session). STILL NO POKEY/SKCTL/AUDCTL/AUDF and NO POKMSK/IRQEN serial
; enable (9O.3b / 9O.4), so installed handlers receive no serial IRQs yet.
; ns_install_vectors is idempotent (vectorInstalled). NOTE: _ns_begin_stream writes
; the OS-vector page + PACTL -- under mos-sim the vector page is the loaded program
; image, so it must NOT be called from sim tests (they use ns_test_begin_soft);
; runtime validation of real begin is Altirra/hardware only.
_ns_begin_stream:
    jsr ns_begin_soft
    jsr DetectPALViaVCOUNT       ; sets nsVideoStd (overrides ns_begin_soft's 0 default)
    lda #0x34
    sta NS_PACTL                 ; motor/control line ON (upstream 0x34)
    jsr ns_install_vectors
    ; (9O.3b: POKEY/SKCTL/AUDCTL/AUDF ; 9O.4: POKMSK/IRQEN enable go here, IRQEN LAST)
    rts

;==========================================================================
; _ns_end_stream  (upstream: NS_EndConcurrent_Impl).
;
; Stage 9O.3a: end = remove OS vectors (ns_restore_vectors) -> deassert the PACTL
; motor line -> reset RAM state (ns_end_soft). PACTL 0x3c is the staged translation
; of upstream's motor-off value. NO POKMSK/IRQEN disable, NO POKEY yet.
; ns_restore_vectors is a safe no-op if vectors were never installed. Same mos-sim
; caveat as begin: do not call from sim tests.
_ns_end_stream:
    ; (9O.4: disable POKMSK/IRQEN serial bits go here, BEFORE restore)
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
    sta serialOutIdle           ; idle (consumed later by the deferred idle-prime)
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
.global ns_test_begin_soft
.global ns_test_end_soft

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
; Deferred state (added when their owning stages land): chain_addr (with the IRQ
; dispatcher), serialVecs/pokey_init tables, port + nominal-baud + BaudTable (SIO).
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

; Stage 9N: tracks whether the OS serial/IRQ vectors are physically installed.
; HARDWARE state, NOT ring state: modified ONLY by ns_install_vectors (->1) and
; ns_restore_vectors (->0). Deliberately NOT cleared by ns_init_ring_state, so a
; hardware-free begin/end cannot desync it from the real vector state (matters once
; 9O makes begin/end install/restore). BSS zero-init = not installed. Not a byte-API gate.
vectorInstalled:     .zero 1 ; 0 = vectors not installed, 1 = installed

nsInputBuffer:  .zero NS_INPUT_BUFSIZE   ; RX, 128; upstream inputBuffer
nsOutputBuffer: .zero NS_OUTPUT_BUFSIZE  ; TX, 128; upstream outputBuffer0
nsPayloadBuf:   .zero NS_PAYLOAD_MAX     ; init payload, 64; upstream NetstreamPayloadBuf
