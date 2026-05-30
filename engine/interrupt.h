#ifndef ENGINE_INTERRUPT_H
#define ENGINE_INTERRUPT_H

// interrupt.h — portable DLI chain builder and VBI hook dispatch.
//
// This is the platform-agnostic half of the Interrupt Manager (ARCHITECTURE.md
// "Interrupt Manager"). It owns DLI registration, the per-frame merge/sort, and
// construction of the indexed handler/next tables the Atari dispatcher walks at
// interrupt time. It reaches hardware ONLY through the `Platform` template
// parameter (Dependency Rule 2) — never by including a platform header — exactly
// as engine/input.h stays free of PIA/POKEY details.
//
// Two-tier DLI model (DECISIONS.md ADR-019): engine-internal handlers are raw
// (minimal save/restore); user C++ handlers route through a dispatcher with
// automatic save/restore; user raw handlers bypass it. C++ handlers are
// non-capturing only (ADR-020), so every handler is stored as a plain
// `void (*)()`. The Atari dispatcher uses a self-modifying JSR (ADR-021).
//
// Chain-index contract (see the plan / ARCHITECTURE.md "Data Flow Per Frame"):
// the tables are indexed by sorted slot position, and `current_` advances once
// per DLI. The VBI sets current_=0 and points VDSLST at entry(slot 0), where
//     entry(s) = dispatcher address   if slot s is a C++ handler
//              = s.handler             if slot s is a raw handler
// When slot i fires, its handler (the C++ dispatcher, or a raw handler via the
// next_dli_addr() helper) stores next_*[i] into VDSLST and increments current_.
// next_*[i] = entry(slot i+1); the final slot's next is a no-op terminal.
//
// Depends only on types.h.

#include "types.h"

namespace engine {

namespace dli {

// ── DLISlot flag bits ────────────────────────────────────────────────
//
// bit 0   : raw handler (bypasses the C++ dispatcher)
// bit 1   : persistent (survives set_screen / clear_transient; ADR-014)
// bits 2-3: priority — lower value chains earlier on a shared scanline
//           (API_DESIGN.md "DLI Priority"): 0 engine multiplex,
//           1 engine scroll (reserved), 2 user.
inline constexpr u8 FLAG_RAW        = 0x01;
inline constexpr u8 FLAG_PERSISTENT = 0x02;
inline constexpr u8 PRIO_SHIFT      = 2;
inline constexpr u8 PRIO_MASK       = 0x0C;

inline constexpr u8 PRIO_MULTIPLEX = 0;   // dynamic, raw
inline constexpr u8 PRIO_SCROLL    = 1;   // reserved for the scroll subsystem
inline constexpr u8 PRIO_USER      = 2;

} // namespace dli

// ── DLIContext ────────────────────────────────────────────────────────
//
// Zero-storage typed register-write facade for C++ DLI handlers that don't want
// to include platform headers (API_DESIGN.md "DLI Context Object"). Each method
// forwards to the platform HAL, which compiles it to a single LDA #imm / STA abs.
// A static instance is provided so a handler can write `ctx.write_colpf0(v)`
// without the reference costing anything.
template <typename Platform>
struct DLIContext {
    using hal = typename Platform::hal;

    void write_colpf0(u8 v) const { hal::write_colpf0(v); }
    void write_colpf1(u8 v) const { hal::write_colpf1(v); }
    void write_colpf2(u8 v) const { hal::write_colpf2(v); }
    void write_colpf3(u8 v) const { hal::write_colpf3(v); }
    void write_colbk (u8 v) const { hal::write_colbk(v); }
    void write_chbase(u8 v) const { hal::write_chbase(v); }
    void write_hscrol(u8 v) const { hal::write_hscrol(v); }
    void write_vscrol(u8 v) const { hal::write_vscrol(v); }
};

// ── DLISlot ───────────────────────────────────────────────────────────
//
// One registered DLI. 4 bytes on the 6502 (a function pointer is 2 bytes).
struct DLISlot {
    u8 scanline;
    u8 flags;
    void (*handler)();
};
static_assert(sizeof(DLISlot) == 4, "DLISlot must be 4 bytes");

// ── InterruptManager ──────────────────────────────────────────────────
//
// Holds the DLI chain (static + dynamic) and the VBI hook list. MaxDLIs and
// MaxVBIHooks are template parameters, not GameConfig fields (API_DESIGN.md
// "Interrupt Manager Configuration").
template <typename Platform, u8 MaxDLIs = 12, u8 MaxVBIHooks = 4>
class InterruptManager {
    static_assert(MaxDLIs >= 1, "need at least one DLI slot");

public:
    // ── DLI registration (static) ──
    //
    // C++ handlers route through the dispatcher; raw handlers bypass it. The
    // persistent variants survive screen changes (ADR-014). All take a plain
    // void(*)() because C++ handlers must be non-capturing (ADR-020).
    void add_dli(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(false, false, dli::PRIO_USER));
    }
    void add_raw_dli(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(true, false, dli::PRIO_USER));
    }
    void add_persistent_dli(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(false, true, dli::PRIO_USER));
    }
    void add_persistent_raw_dli(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(true, true, dli::PRIO_USER));
    }

    // Remove every static DLI registered for `scanline`, compacting the chain.
    void remove_dli(u8 scanline) {
        u8 dst = 0;
        for (u8 i = 0; i < static_count_; ++i) {
            if (chain_[i].scanline != scanline) {
                if (dst != i) chain_[dst] = chain_[i];
                ++dst;
            }
        }
        static_count_ = dst;
        total_count_  = dst;
    }

    // Drop all non-persistent static DLIs (called on screen change; ADR-014).
    void clear_transient() {
        u8 dst = 0;
        for (u8 i = 0; i < static_count_; ++i) {
            if (chain_[i].flags & dli::FLAG_PERSISTENT) {
                if (dst != i) chain_[dst] = chain_[i];
                ++dst;
            }
        }
        static_count_ = dst;
        total_count_  = dst;
    }

    // ── Dynamic DLIs (rebuilt every frame by the multiplexer) ──
    //
    // begin_dynamic() discards the dynamic tail, keeping the static slots.
    // add_dynamic_dli() appends a raw, top-priority slot.
    void begin_dynamic() { total_count_ = static_count_; }

    void add_dynamic_dli(u8 scanline, void (*h)()) {
        if (total_count_ >= MaxDLIs) return;
        chain_[total_count_] = DLISlot{
            scanline, slot_flags(true, false, dli::PRIO_MULTIPLEX), h};
        ++total_count_;
    }

    // ── VBI hooks ──
    void add_vbi_hook(void (*hook)()) {
        if (hook_count_ >= MaxVBIHooks) return;
        hooks_[hook_count_++] = hook;
    }
    void remove_vbi_hook(void (*hook)()) {
        u8 dst = 0;
        for (u8 i = 0; i < hook_count_; ++i) {
            if (hooks_[i] != hook) {
                if (dst != i) hooks_[dst] = hooks_[i];
                ++dst;
            }
        }
        hook_count_ = dst;
    }

    // ── Chain construction ──
    //
    // Sort all live slots (static + dynamic) by scanline then priority, then
    // build the indexed handler and next-pointer tables the dispatcher walks.
    void prepare_chain() {
        sort_slots();

        const u16 dispatcher = Platform::hal::dli_dispatch_addr();
        const u16 terminal   = Platform::hal::dli_terminal_addr();

        for (u8 i = 0; i < total_count_; ++i) {
            const u16 ha = handler_addr(chain_[i]);
            handler_lo_[i] = lo(ha);
            handler_hi_[i] = hi(ha);

            const u16 nxt = (i + 1 < total_count_) ? entry(chain_[i + 1],
                                                           dispatcher)
                                                   : terminal;
            next_lo_[i] = lo(nxt);
            next_hi_[i] = hi(nxt);
        }

        // VDSLST is pointed here by the VBI; current_ starts the chain at 0.
        first_entry_ = (total_count_ > 0) ? entry(chain_[0], dispatcher)
                                          : terminal;
        current_ = 0;
    }

    // Address the VBI writes into VDSLST to arm the first DLI of the frame.
    u16 first_handler_addr() const { return first_entry_; }

    // Helper for raw handlers: the address to store into VDSLST after this DLI.
    // Returns next_*[current_] and advances the chain index.
    u16 next_dli_addr() {
        const u16 a = static_cast<u16>(next_lo_[current_]) |
                      (static_cast<u16>(next_hi_[current_]) << 8);
        ++current_;
        return a;
    }

    // ── Queries ──
    u8 dli_count() const { return total_count_; }
    u8 static_dli_count() const { return static_count_; }
    static constexpr u8 capacity() { return MaxDLIs; }
    u8 vbi_hook_count() const { return hook_count_; }

    // Direct table access (for the dispatcher and for tests).
    const DLISlot& slot(u8 i) const { return chain_[i]; }
    u8 next_lo(u8 i) const { return next_lo_[i]; }
    u8 next_hi(u8 i) const { return next_hi_[i]; }
    u8 handler_lo(u8 i) const { return handler_lo_[i]; }
    u8 handler_hi(u8 i) const { return handler_hi_[i]; }

private:
    static constexpr u8 slot_flags(bool raw, bool persistent, u8 priority) {
        u8 f = static_cast<u8>((priority << dli::PRIO_SHIFT) & dli::PRIO_MASK);
        if (raw)        f |= dli::FLAG_RAW;
        if (persistent) f |= dli::FLAG_PERSISTENT;
        return f;
    }
    static bool is_raw(const DLISlot& s) { return (s.flags & dli::FLAG_RAW) != 0; }
    static u8   priority(const DLISlot& s) {
        return static_cast<u8>((s.flags & dli::PRIO_MASK) >> dli::PRIO_SHIFT);
    }
    static u8   lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static u8   hi(u16 a) { return static_cast<u8>(a >> 8); }
    static u16  handler_addr(const DLISlot& s) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(s.handler));
    }
    // What VDSLST must point at to enter slot `s`: the dispatcher for a C++
    // handler, or the raw handler itself.
    static u16 entry(const DLISlot& s, u16 dispatcher) {
        return is_raw(s) ? handler_addr(s) : dispatcher;
    }

    void add_static(u8 scanline, void (*h)(), u8 flags) {
        if (static_count_ >= MaxDLIs) return;
        chain_[static_count_] = DLISlot{scanline, flags, h};
        ++static_count_;
        total_count_ = static_count_;
    }

    // Stable insertion sort by (scanline, priority). Stable so that handlers
    // sharing a scanline+priority keep registration order, and nearly-sorted
    // dynamic data costs little (ADR-024 rationale, applied to the DLI chain).
    void sort_slots() {
        for (u8 i = 1; i < total_count_; ++i) {
            const DLISlot key = chain_[i];
            u8 j = i;
            while (j > 0 && greater(chain_[j - 1], key)) {
                chain_[j] = chain_[j - 1];
                --j;
            }
            chain_[j] = key;
        }
    }
    // True if `a` must sort strictly after `b` (so equal keys don't swap →
    // stable).
    static bool greater(const DLISlot& a, const DLISlot& b) {
        if (a.scanline != b.scanline) return a.scanline > b.scanline;
        return priority(a) > priority(b);
    }

    DLISlot chain_[MaxDLIs] = {};
    u8 static_count_ = 0;
    u8 total_count_  = 0;
    u8 current_      = 0;   // ZP-intended (section placement deferred to linker)

    u8 handler_lo_[MaxDLIs] = {};
    u8 handler_hi_[MaxDLIs] = {};
    u8 next_lo_[MaxDLIs + 1] = {};
    u8 next_hi_[MaxDLIs + 1] = {};

    void (*hooks_[MaxVBIHooks])() = {};
    u8 hook_count_ = 0;

    u16 first_entry_ = 0;
};

} // namespace engine

#endif // ENGINE_INTERRUPT_H
