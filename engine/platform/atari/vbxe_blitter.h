#ifndef ENGINE_PLATFORM_ATARI_VBXE_BLITTER_H
#define ENGINE_PLATFORM_ATARI_VBXE_BLITTER_H

// platform/atari/vbxe_blitter.h — Blitter Command Blocks (BCBs), constexpr BCB
// builders, and the per-frame BCB queue.
//
// The VBXE blitter copies/fills VRAM rectangles. It is driven by a BlitterList:
// a chain of 21-byte BCBs in VRAM (FX Core v1.26 manual, "BLITTER" pp. 30-37).
// The queue stages BCBs in CPU RAM during the frame, then submit() uploads them
// via MEMAC and starts the blitter. BCB builders are constexpr so common ops
// (clear, sprite, copy, fill) can be assembled at compile time.

#include "../../mmio.h"
#include "../../types.h"
#include "vbxe_layout.h"
#include "vbxe_memac.h"
#include "vbxe_registers.h"

namespace atari::vbxe {

using engine::u8;
using engine::u16;
using engine::u32;
using engine::i8;
using engine::i16;

// ── BCB ──────────────────────────────────────────────────────────────
// 21 bytes, field order exactly as the blitter fetches it (manual p30). All
// members are 1-byte, so the struct is naturally 21 bytes with no padding.
struct BCB {
    u8 source_adr[3];        // 19-bit source address (LE)
    u8 source_step_y[2];     // signed 12-bit step Y (LE)
    i8 source_step_x;        // signed 8-bit step X
    u8 dest_adr[3];          // 19-bit destination address (LE)
    u8 dest_step_y[2];       // signed 12-bit step Y (LE)
    i8 dest_step_x;          // signed 8-bit step X
    u8 blt_width[2];         // 9-bit width in bytes, minus 1 (LE)
    u8 blt_height;           // 8-bit height in lines, minus 1
    u8 blt_and_mask;
    u8 blt_xor_mask;
    u8 blt_collision_mask;
    u8 blt_zoom;
    u8 pattern_feature;
    u8 blt_control;          // MODE (bits 0-2) + NEXT
};
static_assert(sizeof(BCB) == 21, "BCB must be exactly 21 bytes");

// Blitter modes (manual p34) and the chain/NEXT bit.
namespace blt_mode {
    inline constexpr u8 COPY        = 0;  // copy all bytes, no transparency
    inline constexpr u8 TRANSPARENT = 1;  // skip colour index 0
    inline constexpr u8 ADD         = 2;  // dest = dest + source
    inline constexpr u8 OR          = 3;  // dest = dest | source
    inline constexpr u8 AND         = 4;  // dest = dest & source
    inline constexpr u8 XOR         = 5;  // dest = dest ^ source
    inline constexpr u8 HR_TRANS    = 6;  // HR nibble-level transparency
    // NEXT marks "another BCB follows". Manual p34 places it at b3 ("- - - -
    // NEXT MODE", MODE in b0-b2). NOTE: the Phase 3 prompt said 0x10 (b4); the
    // manual is the hardware authority, so 0x08 is used here.
    inline constexpr u8 NEXT        = 0x08;
} // namespace blt_mode

// ── constexpr packers ────────────────────────────────────────────────

// 19-bit address -> 3 little-endian bytes.
constexpr void pack_addr(u8 out[3], u32 addr) {
    out[0] = static_cast<u8>(addr & 0xFF);
    out[1] = static_cast<u8>((addr >> 8) & 0xFF);
    out[2] = static_cast<u8>((addr >> 16) & 0x07);
}

// Signed 12-bit step -> 2 little-endian bytes (two's complement in the low 12 bits).
// NOTE on step_y: the blitter advances the row-base address by step_y after each
// row, and the X position is internal to the row — so step_y is the FULL row
// stride, NOT stride-width. (The Phase 3 prompt's "stride - width" was wrong; it
// renders every row shifted left by `width`, collapsing sprites into a diagonal
// line and making rectangle fills re-clear only the first row. Hardware-confirmed.)
constexpr void pack_step_y(u8 out[2], i16 step) {
    out[0] = static_cast<u8>(step & 0xFF);
    out[1] = static_cast<u8>((step >> 8) & 0x0F);
}

// Pack a 9-bit (width-1) value into blt_width[2] (LE).
constexpr void pack_width(u8 out[2], u16 width_minus_1) {
    out[0] = static_cast<u8>(width_minus_1 & 0xFF);
    out[1] = static_cast<u8>((width_minus_1 >> 8) & 0x01);
}

// ── constexpr BCB builders (NEXT set by default; queue clears it on the last) ──

// Fill a rectangle with a solid colour using the constant-source optimisation:
// blt_and_mask == 0 makes the source constant (= blt_xor_mask), so the blitter
// skips source reads and runs at ~2x. `width` in bytes, `height` in lines.
constexpr BCB bcb_fill_rect(u32 dest, u16 width, u8 height,
                            u16 dest_stride, u8 color) {
    BCB b{};
    // source_adr unused (constant source); leave zero.
    b.source_step_x = 1;
    pack_addr(b.dest_adr, dest);
    pack_step_y(b.dest_step_y, static_cast<i16>(dest_stride));
    b.dest_step_x = 1;
    pack_width(b.blt_width, static_cast<u16>(width - 1));
    b.blt_height = static_cast<u8>(height - 1);
    b.blt_and_mask = 0x00;          // constant source
    b.blt_xor_mask = color;         // = the constant value written
    b.blt_control = static_cast<u8>(blt_mode::COPY | blt_mode::NEXT);
    return b;
}

// Clear a whole framebuffer region to a solid colour (constant-source fill).
constexpr BCB bcb_clear(u32 dest, u16 width, u8 height,
                        u16 dest_stride, u8 color) {
    return bcb_fill_rect(dest, width, height, dest_stride, color);
}

// Blit a sprite with transparency (mode 1: colour index 0 is skipped).
constexpr BCB bcb_sprite(u32 src, u32 dest, u16 width, u8 height,
                         u16 src_stride, u16 dest_stride) {
    BCB b{};
    pack_addr(b.source_adr, src);
    pack_step_y(b.source_step_y, static_cast<i16>(src_stride));
    b.source_step_x = 1;
    pack_addr(b.dest_adr, dest);
    pack_step_y(b.dest_step_y, static_cast<i16>(dest_stride));
    b.dest_step_x = 1;
    pack_width(b.blt_width, static_cast<u16>(width - 1));
    b.blt_height = static_cast<u8>(height - 1);
    b.blt_and_mask = 0xFF;          // real source data (not constant)
    b.blt_xor_mask = 0x00;
    b.blt_control = static_cast<u8>(blt_mode::TRANSPARENT | blt_mode::NEXT);
    return b;
}

// Blit a 1bpp-expanded sprite, colouring it via the AND-mask. The source holds
// 0xFF for set pixels and 0x00 for clear; source'' = source & color, so set
// pixels become `color` and clear pixels become 0 (transparent in mode 1). This
// lets one expanded shape in VRAM be drawn in any colour per instance.
constexpr BCB bcb_sprite_colored(u32 src, u32 dest, u16 width, u8 height,
                                 u16 src_stride, u16 dest_stride, u8 color) {
    BCB b{};
    pack_addr(b.source_adr, src);
    pack_step_y(b.source_step_y, static_cast<i16>(src_stride));
    b.source_step_x = 1;
    pack_addr(b.dest_adr, dest);
    pack_step_y(b.dest_step_y, static_cast<i16>(dest_stride));
    b.dest_step_x = 1;
    pack_width(b.blt_width, static_cast<u16>(width - 1));
    b.blt_height = static_cast<u8>(height - 1);
    b.blt_and_mask = color;         // set pixel (0xFF) -> color; clear (0x00) -> 0
    b.blt_xor_mask = 0x00;
    b.blt_control = static_cast<u8>(blt_mode::TRANSPARENT | blt_mode::NEXT);
    return b;
}

// Opaque copy (mode 0: every byte copied, no transparency check).
constexpr BCB bcb_copy(u32 src, u32 dest, u16 width, u8 height,
                       u16 src_stride, u16 dest_stride) {
    BCB b{};
    pack_addr(b.source_adr, src);
    pack_step_y(b.source_step_y, static_cast<i16>(src_stride));
    b.source_step_x = 1;
    pack_addr(b.dest_adr, dest);
    pack_step_y(b.dest_step_y, static_cast<i16>(dest_stride));
    b.dest_step_x = 1;
    pack_width(b.blt_width, static_cast<u16>(width - 1));
    b.blt_height = static_cast<u8>(height - 1);
    b.blt_and_mask = 0xFF;
    b.blt_xor_mask = 0x00;
    b.blt_control = static_cast<u8>(blt_mode::COPY | blt_mode::NEXT);
    return b;
}

// ── Blitter register helpers ─────────────────────────────────────────

template <typename Config>
void set_blt_addr(u32 vram_addr) {
    Regs<Config>::BL_ADR0 = static_cast<u8>(vram_addr & 0xFF);
    Regs<Config>::BL_ADR1 = static_cast<u8>((vram_addr >> 8) & 0xFF);
    Regs<Config>::BL_ADR2 = static_cast<u8>((vram_addr >> 16) & 0x07);
}

template <typename Config>
void start_blitter() {
    Regs<Config>::BLITTER_START = blitter_start::START;
}

// ── BCB queue ────────────────────────────────────────────────────────
//
// Stages up to MaxBCBs commands in CPU RAM for one frame, then uploads the chain
// to VRAM and starts the blitter. The queue clears NEXT on the final BCB so the
// blitter stops after it.
template <u8 MaxBCBs>
class BlitterQueue {
public:
    void reset() { count_ = 0; }

    bool push(const BCB& cmd) {
        if (count_ >= MaxBCBs) return false;
        queue_[count_++] = cmd;
        return true;
    }

    u8   count() const { return count_; }
    bool full()  const { return count_ >= MaxBCBs; }
    bool empty() const { return count_ == 0; }

    BCB& operator[](u8 i) { return queue_[i]; }

    // Upload the chain to VRAM and start the blitter, asynchronously: the blitter
    // runs while the CPU continues (it has the rest of the frame to finish). We do
    // NOT busy-wait for completion here — a synchronous wait inside the VBI makes
    // the VBI longer than a frame and lets the next VBI NMI re-enter it (stack
    // overflow). The double-buffer present waits for the previous frame's blit
    // (already finished) before flipping; see OverlayHal::overlay_present.
    //
    // The bounded wait at the START guards against overwriting a still-in-flight
    // BCB list (normally already idle, so it returns immediately).
    template <typename Config>
    void submit() {
        if (count_ == 0) return;
        for (u16 spin = 0; spin < 50000; ++spin) {
            if ((static_cast<u8>(Regs<Config>::BLITTER_BUSY) &
                 (blitter_busy::BUSY | blitter_busy::BCB_LOAD)) == 0) break;
        }
        queue_[count_ - 1].blt_control &= static_cast<u8>(~blt_mode::NEXT);  // last BCB ends the chain
        using Layout = VRAMLayout<Config>;
        MemacWindow<Config>::write(
            Layout::bcb_queue,
            reinterpret_cast<const u8*>(queue_),
            static_cast<u16>(count_ * sizeof(BCB)));
        set_blt_addr<Config>(Layout::bcb_queue);
        start_blitter<Config>();
    }

    // True while the blitter is still processing (BUSY or loading a BCB).
    template <typename Config>
    static bool busy() {
        return (static_cast<u8>(Regs<Config>::BLITTER_BUSY) &
                (blitter_busy::BUSY | blitter_busy::BCB_LOAD)) != 0;
    }

private:
    BCB queue_[MaxBCBs];
    u8  count_ = 0;
};

} // namespace atari::vbxe

#endif // ENGINE_PLATFORM_ATARI_VBXE_BLITTER_H
