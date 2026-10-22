// tests/backends/atari/vbi_skip_altirra_probe.cpp
// Altirra/hardware probe: how much per-frame work survives a frame whose service
// never runs, and does the raster hook still fire?
//
// The defect this measures (EDGE writeup 18) is that the frame service rides the
// OS *deferred* vertical-blank vector, and the OS reaches that vector only when
// the code it interrupted had interrupts enabled. A game whose loop holds short
// SEI windows — the netstream handler's per-byte ring operations do — loses the
// whole service on those frames: no scroll commit, no raster-chain re-arm, and
// nothing restores the colour registers a raster hook left behind, because the
// restore the game relies on is the OS's own deferred shadow copy. The symptoms
// on the consumer's screen were scroll jitter, a playfield wearing the hook's
// background colour, and the hook's own band losing its palette.
//
// NEITHER CLASS REPRODUCES UNDER mos-sim (no ANTIC, no NMI) and neither
// reproduces in an ordinary demo: a demo's loop never holds interrupts off long
// enough. So this probe MANUFACTURES both classes, independently, and counts the
// outcome per frame of the OS's own clock:
//
//   class A  the OS skipped its deferred stage.  Forced by probe_sei_window():
//            the loop sits with interrupts disabled for longer than one frame, so
//            the vertical blank always lands inside that window. The deferred
//            vector is never reached, so the engine's re-entry guard never sees
//            the frame either — edge_vbi_skips must NOT count these.
//   class B  the OS ran its deferred stage; the engine's guard turned the frame
//            away because the previous frame was not yet consumed. Forced by
//            probe_busy_window(): a delay with interrupts ENABLED placed after the
//            loop takes the frame but before it releases the guard. These are
//            exactly what edge_vbi_skips counts.
//
// Setting the two knobs independently is what proves the counter counts the right
// class: with only the SEI window, skipped must be 0 while most frames go
// unserviced; with only the busy window, every unserviced frame must be counted.
//
// The frame clock is the OS jiffy counter RTCLOK, which the OS's *immediate*
// stage increments on every frame — and the immediate stage is reached THROUGH
// this engine's own immediate-path handler (it is what jumps to SYSVBV), so a
// jiffy is also proof that that handler ran to completion on that frame. That is
// why the fine-scroll flush and anything else placed there needs no counter of
// its own: jiffies IS its count.
//
// WHAT CANNOT BE COUNTED FROM INSIDE: COLBK and HSCROL are write-only, so the
// probe cannot read back whether a colour was restored. That half is a pixel A/B
// — scripts/altirra_screenshot.sh, and Fujisan for the second emulator. Run the
// probe with a long SEI window so nearly every frame is class A, and read the
// screen: the marker palette is written by the raster hook over the BOTTOM of the
// screen only, so
//     correct  -> playfield colour above the seam, marker colour below it
//     broken   -> the marker colour over the WHOLE screen (the hook's background
//                 survived the frame because nothing restored it)
//
// Dump layout at $0600, little-endian 16-bit unless noted:
//   $0600 jiffies over the run (RTCLOK)   $060C stage 2 ran = serviced + skipped
//   $0602 frames serviced                 $060E class A = jiffies - stage 2
//   $0604 frames skipped (guard; class B) $0610 probe_sei_outer  (knob, 1 byte)
//   $0606 raster hook dispatches          $0611 probe_busy_outer (knob, 1 byte)
//   $0608 frames dropped (overrun)        $0612 marker COLBK     (1 byte)
//   $060A loop iterations                 $0613 playfield COLBK  (1 byte)
//                                         $0614 done marker $A5 (written LAST)
//
// Altirra/Fujisan only; intentionally NOT a CTest.

#include <stdint.h>

#include <engine/platform/atari/platform.h>
#include <engine/core.h>

#include "atari_hostdump.h"

using engine::u8;
using engine::u16;
namespace M = atari;

// ── Knobs ────────────────────────────────────────────────────────────────
//
// Each outer iteration of a window is ~1,285 cycles (256 x `dey/bne`), against
// ~29,868 cycles in an NTSC frame. A window must exceed ONE FRAME to land the
// vertical blank inside itself at all: the loop's phase is locked to the service
// that woke it, so a sub-frame window always finishes before the next frame
// arrives and manufactures nothing. 24 outer iterations is one frame; the
// defaults below sit just over it, which alternates serviced/unserviced for a
// ~50 % rate of each class.
#ifndef PROBE_SEI_OUTER
#define PROBE_SEI_OUTER 30      // ~1.3 frames with interrupts OFF  -> class A
#endif
#ifndef PROBE_BUSY_OUTER
#define PROBE_BUSY_OUTER 0      // ~frames with interrupts ON, guard held -> class B
#endif
#ifndef PROBE_RUN_JIFFIES
#define PROBE_RUN_JIFFIES 300   // ~5 s at 60 Hz
#endif
// Scroll the view every iteration (the default). Set to 0 to hold it still, which
// is how the residual deficit in the dispatch count is attributed: the scroll's
// coarse step swaps the double-buffered display list, and the copy swapped in
// carries the raster hook's display-list bit only once the chain has been prepared
// against it, so a swap costs that frame's hook no matter how delivery is armed.
#ifndef PROBE_SCROLL
#define PROBE_SCROLL 1
#endif

extern "C" {
volatile uint8_t probe_sei_outer  = PROBE_SEI_OUTER;
volatile uint8_t probe_busy_outer = PROBE_BUSY_OUTER;
// Bumped by the raw raster hook below. 16-bit: a run is hundreds of frames.
volatile uint16_t probe_dli_count = 0;
[[gnu::naked]] void probe_sei_window();
[[gnu::naked]] void probe_busy_window();
[[gnu::naked]] void probe_marker_dli();
extern uint8_t edge_dli_op_curx2;   // engine raster-chain tail (platform/atari/dli_dispatch.h)
}

// The two manufactured windows. Naked and hand-written so the delay is the loop
// body and nothing else, and so the SEI/CLI pair cannot be reordered around it.
// Both clobber A/X/Y only, and neither touches the llvm-mos imaginary registers
// $80-$9F or any zero-page static frame, so an interrupt landing inside either
// one has nothing of the probe's to corrupt.
asm(R"(
    .globl probe_sei_window
    .globl probe_busy_window
probe_sei_window:
    sei                        ; the OS will skip its deferred stage for this frame
    ldx probe_sei_outer
    beq .Lpsw_done
.Lpsw_outer:
    ldy #0
.Lpsw_inner:
    dey
    bne .Lpsw_inner            ; 256 * 5 = 1280 cycles
    dex
    bne .Lpsw_outer
.Lpsw_done:
    cli
    rts

probe_busy_window:
    ldx probe_busy_outer       ; interrupts stay ENABLED: the OS runs its deferred
    beq .Lpbw_done             ; stage, and the engine's guard turns the frame away
.Lpbw_outer:
    ldy #0
.Lpbw_inner:
    dey
    bne .Lpbw_inner
    dex
    bne .Lpbw_outer
.Lpbw_done:
    rts
)");

// ── The marker raster hook ───────────────────────────────────────────────
//
// A RAW hook (no C++ dispatcher, no $80-$9F save) that repaints the whole colour
// set partway down the screen and counts itself — the consumer's status band in
// miniature, down to leaving the registers dirty for the rest of the frame and
// relying on the next vertical blank to put them back. It chains through the
// engine's raster-hook tail, which re-points the raster vector from the chain,
// advances the walk index, restores A/X/Y and RTIs.
// kMarkerColbk MUST match the `lda #$36` in the hook below: the hook is hand
// assembly and cannot read a C++ constant, and the dump echoes both colours so a
// screenshot can be read without consulting this source.
static constexpr u8 kMarkerColbk = 0x36;   // bright orange: unmistakable if it leaks
static constexpr u8 kFieldColbk  = 0x90;   // dark blue playfield background

asm(R"(
    .globl probe_marker_dli
probe_marker_dli:
    pha
    txa
    pha
    tya
    pha
    lda #$36
    sta $d01a                  ; COLBK  : the marker band
    lda #$1a
    sta $d016                  ; COLPF0
    lda #$2a
    sta $d017                  ; COLPF1
    lda #$4a
    sta $d018                  ; COLPF2
    lda #$6a
    sta $d019                  ; COLPF3
    inc probe_dli_count
    bne .Lpmd_done
    inc probe_dli_count+1
.Lpmd_done:
    jmp edge_dli_op_curx2
)");

// ── Platform + game configuration ────────────────────────────────────────

using Platform = atari::Platform<
    atari::Machine::XL,
    atari::RAM::Baseline,
    atari::gfx::Baseline,
    atari::Sound::Mono,
    atari::TV::NTSC>;

static constexpr u16 kMapW = 48;
static constexpr u16 kMapH = 32;

// One full-screen Mode 4 scroll region. The scroll is live every frame so the
// fine-scroll half of the per-frame work is actually exercised.
struct ProbeScreen {
    using display = engine::DisplayLayout<
        engine::ScrollRegion<engine::TextRegion<M::Mode::MODE_4, 24>, kMapW, kMapH>>;
};

struct GameConfig {
    using screens = engine::ScreenSet<ProbeScreen>;
    static constexpr u8 max_sprites       = 1;
    static constexpr u8 sound_channels    = 1;
    static constexpr u8 max_raster_hooks  = 1;   // the consumer's shape: one static hook
    static constexpr u8 max_frame_hooks   = 0;
};

using Game = engine::Core<Platform, GameConfig>;

static engine::TileMap<kMapW, kMapH> g_map;

// The display list opens with 3 dl_blank(8) = 24 scanlines, then 24 Mode 4 rows of
// 8 scanlines. Put the seam two thirds down, at row 16. ANTIC raises the DLI on
// the LAST scanline of the mode line carrying the bit and the new colours latch on
// the NEXT line, so the hook sits one row earlier than the first marked line.
static constexpr u8 kMarkerFirstLine = 24 + 16 * 8;
static constexpr u8 kMarkerDli       = kMarkerFirstLine - 8;

// ── Result block (page 6, free RAM on every Atari) ───────────────────────
static volatile uint8_t* const kResult = reinterpret_cast<volatile uint8_t*>(0x0600);
static constexpr u16 kDumpBytes = 21;

static void store16(u16 addr, u16 v) {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(addr);
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// RTCLOK's middle and low bytes as one 16-bit jiffy count. The OS increments the
// low byte in its immediate stage, so this advances on every frame whether or not
// the service ran. Read low-high-low so a carry between the two reads is caught
// instead of producing a 256-jiffy jump.
static u16 jiffies() {
    volatile uint8_t* const rtclok = reinterpret_cast<volatile uint8_t*>(0x0013);
    for (;;) {
        const uint8_t lo1 = rtclok[1];
        const uint8_t hi  = rtclok[0];
        const uint8_t lo2 = rtclok[1];
        if (lo1 == lo2) return static_cast<u16>((static_cast<u16>(hi) << 8) | lo1);
    }
}

static engine::Charset1K make_tileset() {
    engine::Charset1K cs{};                       // zero-init: tile 0 is all background
    // Tile 1: a COLPF0 vertical bar — enough texture that a frozen scroll is
    // visible in a screenshot, and it is drawn in a colour the marker hook moves.
    for (u8 i = 0; i < 8; ++i) cs.data[1 * 8 + i] = 0x55;
    // Tile 2: solid COLPF2, for the column rulers.
    for (u8 i = 0; i < 8; ++i) cs.data[2 * 8 + i] = 0xAA;
    return cs;
}

static engine::Charset1K g_tileset = make_tileset();

static void fill_map() {
    for (u16 r = 0; r < kMapH; ++r)
        for (u16 c = 0; c < kMapW; ++c)
            g_map.set_tile(c, r, static_cast<u8>((c % 8 == 0) ? 2 : (c & 1)));
}

int main() {
    for (u16 i = 0; i < kDumpBytes; ++i) kResult[i] = 0;

    Game::init(g_tileset);

    Platform::hal::set_color_pf(4, kFieldColbk);   // COLBK  : playfield background
    Platform::hal::set_color_pf(0, 0x0E);          // COLPF0 : white bars
    Platform::hal::set_color_pf(1, 0x46);          // COLPF1
    Platform::hal::set_color_pf(2, 0xC8);          // COLPF2 : green rulers
    Platform::hal::set_color_pf(3, 0x84);          // COLPF3

    fill_map();
    Game::scroll_map(g_map);
    Game::scroll.set(0, 0);

    Game::interrupts.add_raw_raster_hook(kMarkerDli, &probe_marker_dli);

    // Let the first service build and arm the chain before the measurement window
    // opens, so the counts are not skewed by start-up.
    const u16 warm = jiffies();
    while (static_cast<u16>(jiffies() - warm) < 30) { }

    Game::reset_frame_stats();
    probe_dli_count = 0;
    const u16 j0   = jiffies();
    const u16 dli0 = probe_dli_count;

    u16 iterations = 0;
    u16 scroll_x   = 0;
    for (;;) {
        while (!Game::frame_ready_) { }
        Game::frame_ready_ = false;
        // Interrupts are ON here and the re-entry guard is still held: a frame
        // arriving inside this window reaches the deferred vector and is turned
        // away by the guard. That is class B, and the only thing that counts it.
        probe_busy_window();
        Game::frame_consumed();
        engine::sync_after_vbi();

        ++iterations;
        if (PROBE_SCROLL) {
            scroll_x = static_cast<u16>((scroll_x + 1) % (kMapW * 4));
            Game::scroll.set(scroll_x, 0);
        }

        if (static_cast<u16>(jiffies() - j0) >= PROBE_RUN_JIFFIES) break;

        // Interrupts OFF for longer than a frame: the OS skips its deferred stage
        // and the service is lost outright. That is class A.
        probe_sei_window();
    }

    const u16 jn       = static_cast<u16>(jiffies() - j0);
    const u16 serviced = Game::frames_serviced();
    const u16 skipped  = Game::frames_skipped();
    const u16 dropped  = Game::frames_dropped();
    const u16 dli      = static_cast<u16>(probe_dli_count - dli0);
    const u16 stage2   = static_cast<u16>(serviced + skipped);

    store16(0x0600, jn);
    store16(0x0602, serviced);
    store16(0x0604, skipped);
    store16(0x0606, dli);
    store16(0x0608, dropped);
    store16(0x060A, iterations);
    store16(0x060C, stage2);
    store16(0x060E, static_cast<u16>(jn - stage2));
    kResult[0x10] = probe_sei_outer;
    kResult[0x11] = probe_busy_outer;
    kResult[0x12] = kMarkerColbk;
    kResult[0x13] = kFieldColbk;
    kResult[0x14] = 0xA5;                          // done marker, written last

    edge_host_dump("H1:NSDUMP.BIN", (const void*)0x0600, kDumpBytes);

    // Terminal state for the screenshot leg, which is where the colour half of the
    // result is read (COLBK is write-only and cannot be counted from inside).
    //
    // Interrupts stay off FOREVER here, so from this point not one frame is ever
    // serviced and the state under test is permanent rather than a coin flip. A
    // capture taken while the loop above was still running lands on a serviced frame
    // about one time in six and shows the healthy picture either way, which is how a
    // run on the second emulator first read as a pass it had not earned. The two
    // outcomes are now stable and tell themselves apart at a glance:
    //
    //   without the immediate restore  the marker palette covers the WHOLE screen.
    //       Nothing puts the colour registers back, so the band's colours — written
    //       by the last hook that fired — survive every following frame. The hook
    //       itself stops firing after that one frame too: the chain tail leaves the
    //       raster vector on the terminal and only a service would re-point it, so
    //       the band does not even redraw.
    //   with it                        the playfield's palette above the seam and
    //       the marker's below it, held steady, because the immediate entry restores
    //       the registers and re-points the chain head on every frame without a
    //       service.
    //
    // The map's stripes are on screen in both, which is what shows the capture is
    // this probe and not a blank or crashed machine.
    for (;;) probe_sei_window();
}
