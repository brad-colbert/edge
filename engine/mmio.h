#ifndef ENGINE_MMIO_H
#define ENGINE_MMIO_H

// mmio.h — memory-mapped I/O primitives: register handles and banked windows.
//
// Platform-neutral building blocks for reaching hardware at fixed absolute
// addresses. This header carries NO hardware constants of its own — a backend
// (e.g. an extended-graphics register/memory-window backend) specializes these with
// concrete addresses. Generic engine subsystems do not use these directly; they
// reach hardware through Platform::hal (ARCHITECTURE.md Dependency Rule 2). The
// types live in the engine layer only so every backend HAL can reuse them
// (ADR-017 notes the register-access pattern applies to every platform).
//
// Everything is an empty struct parameterised by address: no storage, and with
// [[gnu::always_inline]] each access collapses to a single LDA/STA against the
// literal address even at -O0. Depends only on types.h (+ <string.h> for the
// bulk window copy/fill helpers).
//
// CAUTION: the banked-window bulk helpers select a bank and then access the
// window; the bank-select register is shared hardware state. If an interrupt
// (e.g. a frame interrupt that also touches the same chip) preempts a transfer and changes
// or disturbs the bank, the rest of the transfer lands in the wrong place. The
// owning backend must keep the bank stable across interrupts (a backend does this
// by having its frame interrupt save/restore the bank-select register).

#include <string.h>

#include "types.h"

namespace engine {

// ── Memory-mapped register handle ────────────────────────────────────
//
// Read via `operator u8`, write via `operator=`. Use as e.g.
// `Mmio<Addr>{} = v;` or `u8 x = Mmio<Addr>{};`.
template <u16 Address>
struct Mmio {
    [[gnu::always_inline]] inline operator u8() const noexcept {
        return *reinterpret_cast<volatile u8*>(Address);
    }
    [[gnu::always_inline]] inline void operator=(u8 v) const noexcept {
        *reinterpret_cast<volatile u8*>(Address) = v;
    }
};

// ── Memory-mapped window (range of bytes at a fixed base) ─────────────
//
// Like Mmio, but accessed via operator[] returning a reference to the volatile
// byte. Constant offsets fold to literal absolute addresses; runtime offsets
// compile to 6502 absolute-indexed addressing (e.g. LDA Base,X).
template <u16 Base>
struct MmioWindow {
    [[gnu::always_inline]] inline volatile u8& operator[](u16 offset) const noexcept {
        return *reinterpret_cast<volatile u8*>(Base + offset);
    }
    static constexpr u16 base = Base;
};

namespace detail {
// Integer log2 of a power-of-two value (compile-time).
constexpr u8 log2_pow2(u32 v) noexcept {
    u8 s = 0;
    while (v > 1) { v >>= 1; ++s; }
    return s;
}
} // namespace detail

// ── Banked memory-mapped window ──────────────────────────────────────
//
// A fixed-size aperture at Base into a larger banked backing store. Only one
// WindowSize-byte bank is visible at a time; which bank shows through is chosen
// by writing the bank-select register at BankSelAddr. WindowSize must be a
// power of two and must match the hardware window size (e.g. a banked memory
// window's SIZE bits). A linear (multi-bank) address splits into
//   bank   = addr >> bank_shift
//   offset = addr & offset_mask
// both of which fold to constants when addr is a constant. BankSelOrMask is
// OR'd into every bank write (carry e.g. a window-enable bit here),
// so a bank select is a single LDA #imm / STA with no register read and no
// temporary.
template <u16 Base, u32 WindowSize, u16 BankSelAddr, u8 BankSelOrMask = 0x00>
struct MmioBankedWindow {
    static_assert((WindowSize & (WindowSize - 1)) == 0,
                  "WindowSize must be a power of two");

    static constexpr u16 base        = Base;
    static constexpr u32 window_size = WindowSize;
    static constexpr u16 offset_mask = static_cast<u16>(WindowSize - 1);
    static constexpr u8  bank_shift  = detail::log2_pow2(WindowSize);

    // Raw in-window access (no banking) -- identical semantics to MmioWindow.
    [[gnu::always_inline]] inline volatile u8& operator[](u16 off) const noexcept {
        return *reinterpret_cast<volatile u8*>(Base + off);
    }

    // Select a bank; constant OR-mask preserves the enable bit with no read.
    [[gnu::always_inline]] inline void bank(u8 n) const noexcept {
        Mmio<BankSelAddr>{} = static_cast<u8>(BankSelOrMask | n);
    }

    // Banked linear access: set the bank, return the window byte. For a
    // constant addr, (addr >> bank_shift) and (addr & offset_mask) are constants.
    [[gnu::always_inline]] inline volatile u8& at(u32 addr) const noexcept {
        bank(static_cast<u8>(addr >> bank_shift));
        return *reinterpret_cast<volatile u8*>(
            Base + (static_cast<u16>(addr) & offset_mask));
    }

    // Efficient banked fill: per-bank memset runs. Each run stays within one
    // bank's window, so no run straddles a bank switch.
    inline void fill(u32 start, u32 len, u8 value) const noexcept {
        while (len) {
            const u16 off = static_cast<u16>(start) & offset_mask;
            u16 run = static_cast<u16>(window_size - off);
            if (run > len) run = static_cast<u16>(len);
            bank(static_cast<u8>(start >> bank_shift));
            memset(reinterpret_cast<void*>(Base + off), value, run);
            start += run;
            len   -= run;
        }
    }

    // Efficient banked copy CPU RAM -> window: per-bank memcpy runs.
    inline void copy(u32 start, const u8* src, u32 len) const noexcept {
        while (len) {
            const u16 off = static_cast<u16>(start) & offset_mask;
            u16 run = static_cast<u16>(window_size - off);
            if (run > len) run = static_cast<u16>(len);
            bank(static_cast<u8>(start >> bank_shift));
            memcpy(reinterpret_cast<void*>(Base + off), src, run);
            start += run;
            src   += run;
            len   -= run;
        }
    }

    // Efficient banked read window -> CPU RAM: mirror of copy(). The bank-walk
    // lives here so callers (e.g. a banked window read) never duplicate the boundary math.
    inline void read(u32 start, u8* dst, u32 len) const noexcept {
        while (len) {
            const u16 off = static_cast<u16>(start) & offset_mask;
            u16 run = static_cast<u16>(window_size - off);
            if (run > len) run = static_cast<u16>(len);
            bank(static_cast<u8>(start >> bank_shift));
            memcpy(dst, reinterpret_cast<const void*>(Base + off), run);
            start += run;
            dst   += run;
            len   -= run;
        }
    }

    // Iterator over linear space that auto-switches banks at window boundaries.
    // State is a 16-bit in-window pointer plus an 8-bit bank, so stepping is a
    // 16-bit increment and a boundary compare; the bank register is written only
    // when crossing a bank boundary (no per-byte 32-bit math).
    struct iterator {
        u16 ptr;  // absolute CPU address inside the window
        u8  bk;   // current bank number
        [[gnu::always_inline]] inline volatile u8& operator*() const noexcept {
            return *reinterpret_cast<volatile u8*>(ptr);
        }
        [[gnu::always_inline]] inline iterator& operator++() noexcept {
            if (++ptr == static_cast<u16>(Base + WindowSize)) {
                ptr = Base;
                Mmio<BankSelAddr>{} = static_cast<u8>(BankSelOrMask | ++bk);
            }
            return *this;
        }
        inline bool operator!=(const iterator& o) const noexcept {
            return ptr != o.ptr || bk != o.bk;
        }
    };

    // begin() also selects the starting bank so the first deref is valid.
    inline iterator begin(u32 start = 0) const noexcept {
        const u8 bk = static_cast<u8>(start >> bank_shift);
        bank(bk);
        return iterator{static_cast<u16>(Base + (static_cast<u16>(start) & offset_mask)), bk};
    }
    inline iterator end(u32 stop) const noexcept {
        return iterator{static_cast<u16>(Base + (static_cast<u16>(stop) & offset_mask)),
                        static_cast<u8>(stop >> bank_shift)};
    }
};

} // namespace engine

#endif // ENGINE_MMIO_H
