#ifndef ENGINE_INTERRUPT_H
#define ENGINE_INTERRUPT_H

// interrupt.h — portable raster-hook chain builder and frame-hook dispatch.
//
// This is the platform-agnostic half of the Interrupt Manager (ARCHITECTURE.md
// "Interrupt Manager"). A raster hook fires partway down the frame at a chosen
// scanline; a frame hook runs once per frame. This header owns raster-hook
// registration, the per-frame merge/sort, and construction of the indexed
// handler/next tables the backend dispatcher walks at interrupt time. It reaches
// hardware ONLY through the `Platform` template parameter (Dependency Rule 2) —
// never by including a platform header — exactly as engine/input.h stays free of
// input-hardware details.
//
// Two-tier hook model (DECISIONS.md ADR-019): engine-internal handlers are raw
// (minimal save/restore); user C++ handlers route through a dispatcher with
// automatic save/restore; user raw handlers bypass it. C++ handlers are
// non-capturing only (ADR-020), so every handler is stored as a plain
// `void (*)()`. The backend dispatcher uses a self-modifying JSR (ADR-021).
//
// Chain-index contract (see the plan / ARCHITECTURE.md "Data Flow Per Frame"):
// the tables are indexed by sorted slot position, and `current_` advances once
// per raster hook. The frame service sets current_=0 and points the backend's
// raster vector at entry(slot 0), where
//     entry(s) = dispatcher address   if slot s is a C++ handler
//              = s.handler             if slot s is a raw handler
// When slot i fires, its handler (the C++ dispatcher, or a raw handler via the
// next_raster_addr() helper) installs next_*[i] as the next vector and increments
// current_. next_*[i] = entry(slot i+1); the final slot's next is a no-op terminal.
//
// Depends only on types.h.

#include "types.h"

namespace engine {

namespace raster {

// ── RasterSlot flag bits ─────────────────────────────────────────────
//
// bit 0   : raw handler (bypasses the C++ dispatcher)
// bit 1   : persistent (survives set_screen / clear_transient; ADR-014)
// bits 2-3: priority — lower value chains earlier on a shared scanline
//           (API_DESIGN.md "Raster Hook Priority"): 0 engine multiplex,
//           1 engine scroll (reserved), 2 user.
inline constexpr u8 FLAG_RAW        = 0x01;
inline constexpr u8 FLAG_PERSISTENT = 0x02;
inline constexpr u8 PRIO_SHIFT      = 2;
inline constexpr u8 PRIO_MASK       = 0x0C;

inline constexpr u8 PRIO_MULTIPLEX = 0;   // dynamic, raw
inline constexpr u8 PRIO_SCROLL    = 1;   // reserved for the scroll subsystem
inline constexpr u8 PRIO_USER      = 2;

} // namespace raster

// ── RasterContext ──────────────────────────────────────────────────────
//
// Zero-storage typed register-write facade for C++ raster-hook handlers that
// don't want to include platform headers (API_DESIGN.md "Raster Context Object").
// Each method forwards to the platform HAL, which compiles it to a single
// LDA #imm / STA abs. A static instance is provided so a handler can write
// `ctx.set_background_color(v)` without the reference costing anything.
//
// `set_playfield_color` takes the field as a template argument so it stays a
// single direct store (no runtime branch) inside a time-critical raster hook.
template <typename Platform>
struct RasterContext {
    using hal = typename Platform::hal;

    template <u8 Field>
    void set_playfield_color(u8 v) const {
        static_assert(Field < 4, "playfield colour field is 0..3");
        if      constexpr (Field == 0) hal::write_colpf0(v);
        else if constexpr (Field == 1) hal::write_colpf1(v);
        else if constexpr (Field == 2) hal::write_colpf2(v);
        else                           hal::write_colpf3(v);
    }
    void set_background_color(u8 v) const { hal::write_colbk(v); }
    void set_charset_base(u8 v)    const { hal::set_charset_base(v); }
    void set_fine_scroll_x(u8 v)   const { hal::set_fine_scroll_x(v); }
    void set_fine_scroll_y(u8 v)   const { hal::set_fine_scroll_y(v); }
};

// ── RasterSlot ───────────────────────────────────────────────────────────
//
// One registered raster hook. 4 bytes on the 6502 (a function pointer is 2 bytes).
struct RasterSlot {
    u8 scanline;
    u8 flags;
    void (*handler)();
};
static_assert(sizeof(RasterSlot) == 4, "RasterSlot must be 4 bytes");

// ── InterruptManager ──────────────────────────────────────────────────
//
// Holds the raster-hook chain (static + dynamic) and the frame-hook list. MaxRasterHooks and
// MaxFrameHooks are template parameters, not GameConfig fields (API_DESIGN.md
// "Interrupt Manager Configuration").
template <typename Platform, u8 MaxRasterHooks = 12, u8 MaxFrameHooks = 4>
class InterruptManager {
    static_assert(MaxRasterHooks >= 1, "need at least one raster-hook slot");

public:
    // ── Raster-hook registration (static) ──
    //
    // C++ handlers route through the dispatcher; raw handlers bypass it. The
    // persistent variants survive screen changes (ADR-014). All take a plain
    // void(*)() because C++ handlers must be non-capturing (ADR-020).
    void add_raster_hook(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(false, false, raster::PRIO_USER));
    }
    void add_raw_raster_hook(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(true, false, raster::PRIO_USER));
    }
    void add_persistent_raster_hook(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(false, true, raster::PRIO_USER));
    }
    void add_persistent_raw_raster_hook(u8 scanline, void (*h)()) {
        add_static(scanline, h, slot_flags(true, true, raster::PRIO_USER));
    }

    // Remove every static raster hook registered for `scanline`, compacting the chain.
    void remove_raster_hook(u8 scanline) {
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

    // Drop all non-persistent static raster hooks (called on screen change; ADR-014).
    void clear_transient() {
        u8 dst = 0;
        for (u8 i = 0; i < static_count_; ++i) {
            if (chain_[i].flags & raster::FLAG_PERSISTENT) {
                if (dst != i) chain_[dst] = chain_[i];
                ++dst;
            }
        }
        static_count_ = dst;
        total_count_  = dst;
    }

    // ── Dynamic raster hooks (rebuilt every frame by the multiplexer) ──
    //
    // begin_dynamic() discards the dynamic tail, keeping the static slots.
    // add_dynamic_raster_hook() appends a raw, top-priority slot.
    //
    // The slot is registered RAW: `h` is a backend-supplied, hand-written DLI that
    // ANTIC enters directly (the multiplexer's edge_multiplex_dli). Routing it
    // through the generic C++ dispatcher was tried and failed on hardware — the
    // dispatcher's full $80-$9F save makes each DLI ~11 scanlines, and a DLI is an
    // unmaskable, non-re-entrant NMI, so two zone boundaries landing within a mode
    // line of each other corrupt the shared dispatch state (ADR-019). The raw
    // handler stays under a scanline by writing pre-baked registers, then chains
    // VDSLST from the next_ table just like the dispatcher (it shares that tail).
    void begin_dynamic() { total_count_ = static_count_; }

    void add_dynamic_raster_hook(u8 scanline, void (*h)()) {
        if (total_count_ >= MaxRasterHooks) return;
        chain_[total_count_] = RasterSlot{
            scanline, slot_flags(true, false, raster::PRIO_MULTIPLEX), h};
        ++total_count_;
    }

    // ── Frame hooks ──
    void add_frame_hook(void (*hook)()) {
        if (hook_count_ >= MaxFrameHooks) return;
        hooks_[hook_count_++] = hook;
    }
    void remove_frame_hook(void (*hook)()) {
        u8 dst = 0;
        for (u8 i = 0; i < hook_count_; ++i) {
            if (hooks_[i] != hook) {
                if (dst != i) hooks_[dst] = hooks_[i];
                ++dst;
            }
        }
        hook_count_ = dst;
    }

    // Invoke every registered frame hook in registration order. The engine's frame
    // service (engine/core.h) calls this as the final step of the per-frame
    // sequence (ARCHITECTURE.md "Data Flow Per Frame" — user frame hooks).
    void run_frame_hooks() const {
        for (u8 i = 0; i < hook_count_; ++i) hooks_[i]();
    }

    // ── Chain construction ──
    //
    // Sort all live slots (static + dynamic) by scanline then priority, then
    // build the indexed handler and next-pointer tables the dispatcher walks.
    void prepare_chain(u8* display_list = nullptr, u16 dl_size = 0) {
        sort_slots();

        const u16 dispatcher = Platform::hal::raster_dispatch_addr();
        const u16 terminal   = Platform::hal::raster_terminal_addr();

        u8 lines[MaxRasterHooks];
        for (u8 i = 0; i < total_count_; ++i) {
            const u16 ha = handler_addr(chain_[i]);
            handler_lo_[i] = lo(ha);
            handler_hi_[i] = hi(ha);

            const u16 nxt = (i + 1 < total_count_) ? entry(chain_[i + 1],
                                                           dispatcher)
                                                   : terminal;
            next_lo_[i] = lo(nxt);
            next_hi_[i] = hi(nxt);

            lines[i] = chain_[i].scanline;
        }

        // The backend's raster vector is pointed here by the frame service; current_ starts at 0.
        first_entry_ = (total_count_ > 0) ? entry(chain_[0], dispatcher)
                                          : terminal;
        current_ = 0;

        // Complete the hardware delivery: arm per-line raster delivery on each
        // chained line (the backend owns that display-program walk — Dependency
        // Rule 2), re-point the raster vector at the chain head, and arm/disarm it.
        if (display_list)
            Platform::hal::program_raster_lines(display_list, dl_size, lines,
                                             total_count_);
        Platform::hal::set_raster_vector(first_entry_);
        if (total_count_ > 0) Platform::hal::enable_raster();
        else                  Platform::hal::disable_raster();
    }

    // One-time setup: patch the backend C++ raster dispatcher's operands with this
    // manager's table and current_ addresses (the single instance never moves, so
    // the addresses are stable). engine::Core::init calls this. Portable: the
    // backend-specific patching lives behind Platform::hal (Dependency Rule 2).
    void arm_dispatch() {
        Platform::hal::install_raster_dispatch(
            addr(&current_), addr(handler_lo_), addr(handler_hi_),
            addr(next_lo_), addr(next_hi_));
    }

    // Address the frame service installs to arm the first raster hook of the frame.
    u16 first_handler_addr() const { return first_entry_; }

    // Helper for raw handlers: the address to install as the next raster vector.
    // Returns next_*[current_] and advances the chain index.
    u16 next_raster_addr() {
        const u16 a = static_cast<u16>(next_lo_[current_]) |
                      (static_cast<u16>(next_hi_[current_]) << 8);
        ++current_;
        return a;
    }

    // ── Queries ──
    u8 raster_hook_count() const { return total_count_; }
    u8 static_raster_hook_count() const { return static_count_; }
    static constexpr u8 capacity() { return MaxRasterHooks; }
    u8 frame_hook_count() const { return hook_count_; }

    // Direct table access (for the dispatcher and for tests).
    const RasterSlot& slot(u8 i) const { return chain_[i]; }
    u8 next_lo(u8 i) const { return next_lo_[i]; }
    u8 next_hi(u8 i) const { return next_hi_[i]; }
    u8 handler_lo(u8 i) const { return handler_lo_[i]; }
    u8 handler_hi(u8 i) const { return handler_hi_[i]; }

private:
    static constexpr u8 slot_flags(bool raw, bool persistent, u8 priority) {
        u8 f = static_cast<u8>((priority << raster::PRIO_SHIFT) & raster::PRIO_MASK);
        if (raw)        f |= raster::FLAG_RAW;
        if (persistent) f |= raster::FLAG_PERSISTENT;
        return f;
    }
    static bool is_raw(const RasterSlot& s) { return (s.flags & raster::FLAG_RAW) != 0; }
    static u8   priority(const RasterSlot& s) {
        return static_cast<u8>((s.flags & raster::PRIO_MASK) >> raster::PRIO_SHIFT);
    }
    static u8   lo(u16 a) { return static_cast<u8>(a & 0xFF); }
    static u8   hi(u16 a) { return static_cast<u8>(a >> 8); }
    static u16  addr(const void* p) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(p));
    }
    static u16  handler_addr(const RasterSlot& s) {
        return static_cast<u16>(reinterpret_cast<uintptr_t>(s.handler));
    }
    // What the raster vector must point at to enter slot `s`: the dispatcher for a
    // C++ handler, or the raw handler itself.
    static u16 entry(const RasterSlot& s, u16 dispatcher) {
        return is_raw(s) ? handler_addr(s) : dispatcher;
    }

    void add_static(u8 scanline, void (*h)(), u8 flags) {
        if (static_count_ >= MaxRasterHooks) return;
        chain_[static_count_] = RasterSlot{scanline, flags, h};
        ++static_count_;
        total_count_ = static_count_;
    }

    // Stable insertion sort by (scanline, priority). Stable so that handlers
    // sharing a scanline+priority keep registration order, and nearly-sorted
    // dynamic data costs little (ADR-024 rationale, applied to the raster-hook chain).
    void sort_slots() {
        for (u8 i = 1; i < total_count_; ++i) {
            const RasterSlot key = chain_[i];
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
    static bool greater(const RasterSlot& a, const RasterSlot& b) {
        if (a.scanline != b.scanline) return a.scanline > b.scanline;
        return priority(a) > priority(b);
    }

    RasterSlot chain_[MaxRasterHooks] = {};
    u8 static_count_ = 0;
    u8 total_count_  = 0;
    u8 current_      = 0;   // ZP-intended (section placement deferred to linker)

    u8 handler_lo_[MaxRasterHooks] = {};
    u8 handler_hi_[MaxRasterHooks] = {};
    u8 next_lo_[MaxRasterHooks + 1] = {};
    u8 next_hi_[MaxRasterHooks + 1] = {};

    void (*hooks_[MaxFrameHooks])() = {};
    u8 hook_count_ = 0;

    u16 first_entry_ = 0;
};

} // namespace engine

#endif // ENGINE_INTERRUPT_H
