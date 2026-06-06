#ifndef ENGINE_SPRITES_H
#define ENGINE_SPRITES_H

// sprites.h — portable logical-sprite manager and multiplexer.
//
// This is the Sprites subsystem (ARCHITECTURE.md "Sprites" / "Multiplex"). The
// game writes logical sprite state during its render phase; the engine commits
// it to hardware-sprite memory during the frame service (one-frame buffering,
// ADR-009). When more sprites are active than there are hardware sprites (4), the
// multiplexer divides the screen into vertical zones and reprograms the hardware
// sprite horizontal positions via a raw raster hook at each zone boundary.
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform header.
// The sprite memory layout (where each sprite's strip lives, how a scanline maps
// to a byte offset) is platform knowledge surfaced through Platform::hal
// (sprite_strip_offset / set_sprite_x), mirroring how interrupt.h gets the raster
// dispatcher address.
//
// Design fixed by ADR-022 (tracked-range sprite clear), ADR-024 (always insertion-
// sort by Y), ADR-025 (projectiles not multiplexed yet; ZoneInfo reserves room).
//
// Depends on types.h and interrupt.h.

#include "config/capabilities.h"
#include "interrupt.h"
#include "types.h"

namespace engine {

// ── Sprite pixel formats ──────────────────────────────────────────────
//
// How a shape's pixels are packed. Neutral (describes packing, not a backend):
//   Packed1bpp — 1 bit/pixel, 1 byte/row (the Atari P/M format). Renders on any
//                backend; on a blitter platform it is expanded to 8bpp in VRAM
//                and coloured per-instance.
//   Pixel8bpp  — 1 byte/pixel colour indices (W*H bytes). Richer art for blitter
//                platforms; not representable in hardware P/M, so it requires a
//                blitter (a static_assert enforces this in sprite()).
enum class SpriteFormat : u8 { Packed1bpp, Pixel8bpp };

// ── SpriteShape ───────────────────────────────────────────────────────
//
// A compile-time sprite shape. `Width` is the on-screen pixel width (8 single /
// 16 double; a hardware sprite is always one byte wide in memory, so `data` is one
// byte per scanline regardless). `Height` is the scanline count. The type
// carries its dimensions as constexpr members so `sprite()` deduces a sprite's
// height from the shape object — there is no separate height argument
// (API_DESIGN.md "Sprite Shapes"). This is the value `Game::make_sprite<W,H>`
// will return.
template <u8 Width, u8 Height>
struct SpriteShape {
    static constexpr u8 width  = Width;
    static constexpr u8 height = Height;
    static constexpr SpriteFormat format = SpriteFormat::Packed1bpp;
    u8 data[Height];
};

// Build a SpriteShape from a braced array of row bytes (the eventual
// Game::make_sprite). `make_sprite<8,16>({ 0b..., ... })`.
template <u8 Width, u8 Height>
constexpr SpriteShape<Width, Height> make_sprite(const u8 (&rows)[Height]) {
    SpriteShape<Width, Height> s{};
    for (u8 i = 0; i < Height; ++i) s.data[i] = rows[i];
    return s;
}

// ── PixelShape ────────────────────────────────────────────────────────
//
// An 8bpp shape: `Width*Height` bytes of colour indices (index 0 = transparent).
// Same surface as SpriteShape (width/height/format/data), so the one `sprite()`
// template consumes either. Blitter-only (sprite() static_asserts on baseline).
template <u8 Width, u8 Height>
struct PixelShape {
    static constexpr u8 width  = Width;
    static constexpr u8 height = Height;
    static constexpr SpriteFormat format = SpriteFormat::Pixel8bpp;
    u8 data[static_cast<u16>(Width) * Height];
};

// Build a PixelShape from a braced array of W*H colour indices, row-major.
template <u8 Width, u8 Height>
constexpr PixelShape<Width, Height> make_pixel_sprite(
        const u8 (&px)[static_cast<u16>(Width) * Height]) {
    PixelShape<Width, Height> s{};
    for (u16 i = 0; i < static_cast<u16>(Width) * Height; ++i) s.data[i] = px[i];
    return s;
}

// ── Sprite vertical resolution ────────────────────────────────────────
//
// Per-screen sprite vertical resolution (ADR-023). SingleLine: 1-scanline Y
// precision, 256 bytes/sprite. DoubleLine: 2-scanline steps, 128 bytes/sprite.
// The underlying values (0/1) are the `res` byte passed to
// Platform::hal::sprite_strip_offset.
enum class SpriteVerticalResolution : u8 { SingleLine, DoubleLine };

// ── LogicalSprite ─────────────────────────────────────────────────────
//
// One logical sprite's buffered state. 7 bytes on the 6502 (pointer = 2 bytes).
// `color` is the sprite's hardware colour; the multiplexer drives it onto whichever
// hardware player slot the sprite lands in, so colour follows the sprite even
// when two sprites cross in Y (set via sprite_color(), persists across the
// per-frame sprite() position updates).
struct LogicalSprite {
    u8        x, y, height;
    u8        color;
    const u8* shape;
    u8        flags;

    static constexpr u8 FLAG_ACTIVE  = 0x01;
    static constexpr u8 FLAG_VISIBLE = 0x02;
};
static_assert(sizeof(LogicalSprite) == 7, "LogicalSprite must be 7 bytes");

// ── ZoneInfo ──────────────────────────────────────────────────────────
//
// One multiplex zone: the scanline at which it becomes active and, per hardware
// player, which logical sprite it shows (0xFF = unused), at what horizontal
// position, and in what colour. 13 active bytes; 8 reserved for future missile
// multiplexing (ADR-025 — keep the struct/hook shaped for it without implementing
// it yet). hpos/colour are snapshotted here (not read live in the hook).
struct ZoneInfo {
    u8 boundary_scanline;
    u8 player_assignment[4];
    u8 hpos[4];
    u8 color[4];
    u8 missile_reserved[8];

    static constexpr u8 UNUSED = 0xFF;
};
static_assert(sizeof(ZoneInfo) == 21, "ZoneInfo must be 13 + 8 reserved bytes");

// ── SpriteManager ─────────────────────────────────────────────────────
//
// MaxSprites logical sprites multiplexed across up to MaxZones zones of 4
// players each (so at most MaxZones*4 sprites can be shown at once).
template <typename Platform, u8 MaxSprites, u8 MaxZones = 4>
class SpriteManager {
    static_assert(MaxSprites >= 1, "need at least one sprite slot");
    static_assert(MaxZones >= 1, "need at least one zone");

public:
    static constexpr u8 kPlayers  = 4;
    static constexpr u8 kMissiles = 4;

    // Zone-boundary DLIs fire at the end of an 8-scanline mode line; bias the
    // boundary up by ~one mode line so the player-position switch lands in the gap
    // between zones (see update_zones). Layout-tunable; one MODE_2 line.
    static constexpr u8 kBoundaryBias = 8;

    using caps = engine::caps_of_t<Platform>;

    // ── Logical sprite state (buffered; committed during the frame service) ──
    template <typename Shape>
    void sprite(u8 slot, const Shape& shape, u8 x, u8 y) {
        // Assign fields individually so the slot's `color` (set via sprite_color)
        // survives the per-frame position update.
        LogicalSprite& s = sprites_[slot];
        s.x      = x;
        s.y      = y;
        s.height = Shape::height;
        s.shape  = shape.data;
        s.flags  = static_cast<u8>(LogicalSprite::FLAG_ACTIVE |
                                   LogicalSprite::FLAG_VISIBLE);
        // On a blitter backend, register the shape (lazily uploaded to VRAM and
        // keyed by its ROM pointer) so the commit phase can blit it. The shape's
        // width/format are only known here (the Shape type), not in LogicalSprite.
        if constexpr (caps::has_blitter) {
            Platform::hal::overlay_register_shape(
                shape.data, Shape::width, Shape::height, Shape::format);
        } else {
            static_assert(Shape::format == SpriteFormat::Packed1bpp,
                "Pixel8bpp sprites require a blitter platform (e.g. VBXE)");
        }
    }

    // Set a sprite's colour (its hardware colour). The multiplexer applies it to the
    // hardware player slot the sprite occupies each frame; sticky across sprite().
    void sprite_color(u8 slot, u8 color) { sprites_[slot].color = color; }

    // Missiles are not multiplexed (ADR-025) — the 4 hardware projectiles are used
    // directly. Position is buffered and pushed to the projectile registers during
    // commit.
    void missile(u8 index, u8 x, u8 y, u8 height) {
        missile_x_[index]      = x;
        missile_y_[index]      = y;
        missile_height_[index] = height;
    }

    // Hide a missile: clearing its height stops the commit from drawing it (and the
    // tracked-range clear erases last frame's strip footprint next commit).
    void missile_hide(u8 index) { missile_height_[index] = 0; }

    void sprite_hide(u8 slot) { sprites_[slot].flags = 0; }
    void sprite_hide_all() {
        for (u8 i = 0; i < MaxSprites; ++i) sprites_[i].flags = 0;
    }

    // Sprite vertical resolution is per-screen (ADR-023); the screen manager sets
    // it on transition. Default single-line, which most games use.
    void set_resolution(SpriteVerticalResolution res) { res_ = res; }
    SpriteVerticalResolution resolution() const { return res_; }

    // ── Multiplexer: sort by Y, group into zones ──
    //
    // Insertion-sort active+visible sprites by Y every frame (ADR-024), then
    // greedily chunk the sorted list into zones of up to 4 players. Zone 0 is
    // active from the top; each later zone's boundary is the midpoint between
    // the last sprite of the previous zone and its own first sprite.
    void update_zones() {
        // Gather active+visible sprite indices.
        active_count_ = 0;
        for (u8 i = 0; i < MaxSprites; ++i) {
            if (is_shown(sprites_[i])) order_[active_count_++] = i;
        }

        // Insertion sort by Y (stable; nearly-sorted ⇒ cheap, ADR-024).
        for (u8 i = 1; i < active_count_; ++i) {
            const u8 key = order_[i];
            const u8 ky  = sprites_[key].y;
            u8 j = i;
            while (j > 0 && sprites_[order_[j - 1]].y > ky) {
                order_[j] = order_[j - 1];
                --j;
            }
            order_[j] = key;
        }

        // On a blitter backend there are no hardware-player limits, so there is
        // no zone grouping — the Y-sort above is enough for back-to-front draw
        // order. (commit_blitter walks order_[0..active_count_).)
        if constexpr (caps::has_blitter) {
            zone_count_ = 0;
            return;
        }

        // Greedy grouping: ceil(active / 4) zones, capped at MaxZones. Sprites
        // beyond MaxZones*4 (lowest on screen) are dropped.
        u8 placed = active_count_;
        if (placed > MaxZones * kPlayers) placed = MaxZones * kPlayers;
        zone_count_ = static_cast<u8>((placed + kPlayers - 1) / kPlayers);

        for (u8 z = 0; z < zone_count_; ++z) {
            ZoneInfo& zi = zones_[z];
            const u8 first = static_cast<u8>(z * kPlayers);
            for (u8 p = 0; p < kPlayers; ++p) {
                const u8 si = static_cast<u8>(first + p);
                if (si < placed) {
                    zi.player_assignment[p] = order_[si];
                    zi.hpos[p]              = sprites_[order_[si]].x;
                    zi.color[p]             = sprites_[order_[si]].color;
                } else {
                    zi.player_assignment[p] = ZoneInfo::UNUSED;
                    zi.hpos[p]              = 0;
                    zi.color[p]             = 0;
                }
            }
            // Boundary: zone 0 from the top; later zones in the gap between
            // adjacent groups. boundary_scanline is consumed by the HAL as a
            // display-list-relative scanline (program_raster_lines), while sprite
            // Y is the P/M strip offset; on the standard layout the two track ~1:1.
            // The DLI fires at the END of its 8-scanline mode line, i.e. a few
            // lines BELOW the value here, so bias the switch up by kBoundaryBias so
            // it lands in the inter-zone gap rather than partway through the lower
            // zone's first sprite (which reads as a split "ghost" sprite). Never
            // raise it above the previous zone's last sprite. Tune kBoundaryBias on
            // hardware if the seams sit visibly off the sprites.
            if (z == 0) {
                zi.boundary_scanline = 0;
            } else {
                const u8 prev_last  = sprites_[order_[first - 1]].y;
                const u8 this_first = sprites_[order_[first]].y;
                const u8 mid = static_cast<u8>((prev_last + this_first) / 2);
                u8 b = (mid > kBoundaryBias) ? static_cast<u8>(mid - kBoundaryBias)
                                             : static_cast<u8>(0);
                if (b < prev_last) b = prev_last;
                zi.boundary_scanline = b;
            }
        }
    }

    // ── Commit buffered state into hardware-sprite memory ──
    //
    // Tracked-range clear (ADR-022): clear only last frame's dirty Y range per
    // sprite, then write each active sprite's shape into its strip at the
    // sprite's Y offset, write zone-0 horizontal positions, push projectile
    // positions, and record this frame's dirty ranges for the next clear.
    // `sprite_base` points at the sprite base (a real address on hardware, a test buffer
    // under the simulator). On a blitter backend it is ignored — sprites are
    // composed in VRAM via the overlay seams instead of P/M memory.
    void commit(u8* sprite_base) {
        if constexpr (caps::has_blitter) {
            commit_blitter();
        } else {
            commit_pm(sprite_base);
        }
    }

    // Blitter commit: clear the back buffer, then blit each active sprite (sorted
    // back-to-front by update_zones) into it via the overlay seams. No P/M memory,
    // no tracked-range clear, no zone-0 register writes. The frame service runs
    // the queued blits afterwards (overlay_submit).
    void commit_blitter() {
        Platform::hal::overlay_frame_begin();
        for (u8 i = 0; i < active_count_; ++i) {
            const LogicalSprite& s = sprites_[order_[i]];
            Platform::hal::overlay_blit_sprite(s.shape, s.x, s.y, s.color);
        }
    }

    void commit_pm(u8* sprite_base) {
        const u8 res = static_cast<u8>(res_);

        // 1. Erase each sprite's exact strip footprint from last frame — only the
        //    bytes it actually wrote, not the whole inter-sprite span. A player
        //    multiplexes several sprites at different Y, so the old per-player
        //    [min,max] clear zeroed the (mostly empty) gaps between them too; with
        //    9 sprites spread down the screen that pushed the commit past one frame.
        //    Clearing exact extents (≤ MaxSprites × height bytes) keeps it inside
        //    the VBI. prev_player_ == kNotDrawn means the sprite wasn't drawn (and
        //    prev_height_ is 0 on the very first commit, so nothing is cleared).
        for (u8 i = 0; i < MaxSprites; ++i) {
            if (prev_player_[i] == kNotDrawn) continue;
            const u16 off = static_cast<u16>(
                Platform::hal::sprite_strip_offset(res, prev_player_[i]) +
                prev_y_[i]);
            for (u8 r = 0; r < prev_height_[i]; ++r) sprite_base[off + r] = 0;
            prev_player_[i] = kNotDrawn;
        }

        // 2. Copy each active sprite's shape into its assigned player's strip and
        //    record the exact footprint for next frame's erase.
        for (u8 z = 0; z < zone_count_; ++z) {
            const ZoneInfo& zi = zones_[z];
            for (u8 p = 0; p < kPlayers; ++p) {
                const u8 li = zi.player_assignment[p];
                if (li == ZoneInfo::UNUSED) continue;
                const LogicalSprite& s = sprites_[li];
                const u16 base = Platform::hal::sprite_strip_offset(res, p);
                const u8 yb = (res_ == SpriteVerticalResolution::SingleLine)
                                  ? s.y : static_cast<u8>(s.y >> 1);
                const u16 off = static_cast<u16>(base + yb);
                for (u8 r = 0; r < s.height; ++r) sprite_base[off + r] = s.shape[r];
                prev_player_[li] = p;
                prev_y_[li]      = yb;
                prev_height_[li] = s.height;
            }
        }

        // 3. Write zone-0 sprite positions + colours (the frame service sets the
        //    first zone from the top; later zones are armed by the boundary raster
        //    hooks). Position goes straight to HPOSP (no OS shadow exists for it),
        //    but the COLOUR must go through the OS PCOLR shadow: this runs in the
        //    VBI, *before* the OS copies PCOLR→COLPM, so a direct COLPM write here
        //    would be overwritten (zeroed) by that copy and zone 0 would show black.
        //    The boundary DLIs, by contrast, run during the visible frame *after*
        //    the copy, so they write COLPM directly (set_sprite_color).
        if (zone_count_ > 0) {
            for (u8 p = 0; p < kPlayers; ++p) {
                Platform::hal::set_sprite_x(p, zones_[0].hpos[p]);
                Platform::hal::set_color_pm(p, zones_[0].color[p]);
            }
        }
        // Missiles are direct, not multiplexed (ADR-025). All four share ONE strip
        // (missile_strip_offset): each scanline byte packs the four missiles two bits
        // apiece (missile m in bits 2m..2m+1), so draws/clears are read-modify-writes
        // of that 2-bit field — missiles sharing a scanline must not stomp each other.
        const u16 mbase = Platform::hal::missile_strip_offset(res);
        // 1. Erase each missile's exact footprint from last frame (its 2 bits only).
        for (u8 m = 0; m < kMissiles; ++m) {
            const u8 mask = static_cast<u8>(0x03 << (m * 2));
            const u16 off = static_cast<u16>(mbase + missile_prev_y_[m]);
            for (u8 r = 0; r < missile_prev_height_[m]; ++r)
                sprite_base[off + r] &= static_cast<u8>(~mask);
            missile_prev_height_[m] = 0;
        }
        // 2. Draw each shown missile (height > 0) and record its footprint, then push
        //    its horizontal position (no OS shadow for HPOSM).
        for (u8 m = 0; m < kMissiles; ++m) {
            const u8 h = missile_height_[m];
            if (h > 0) {
                const u8 mask = static_cast<u8>(0x03 << (m * 2));
                const u8 yb = (res_ == SpriteVerticalResolution::SingleLine)
                                  ? missile_y_[m] : static_cast<u8>(missile_y_[m] >> 1);
                const u16 off = static_cast<u16>(mbase + yb);
                for (u8 r = 0; r < h; ++r) sprite_base[off + r] |= mask;
                missile_prev_y_[m]      = yb;
                missile_prev_height_[m] = h;
            }
            Platform::hal::set_projectile_x(m, missile_x_[m]);
        }
    }

    // ── Raster-hook construction ──
    //
    // Register a boundary raster hook for each zone after the first (zone 0's
    // positions are set by the frame-service commit), and pre-bake that zone's four
    // HPOSP + four COLPM bytes into mux_table_ in fire order. The raw DLI
    // (Platform::hal::multiplex_dli, edge_multiplex_dli on Atari) reads row
    // mux_index_, copies the eight bytes straight to the GTIA registers, and bumps
    // the index — no per-DLI work, so it stays well under a scanline (see the
    // re-entrancy note in interrupt.h::add_dynamic_raster_hook). mux_index_ is reset
    // here, in the VBI, before the frame's first boundary DLI fires.
    //
    // Under the simulator there is no hardware raster, so the hook is registered
    // (and the table built) but never entered.
    template <typename IM>
    void build_raster_hooks(IM& im) {
        im.begin_dynamic();
        // A blitter backend has no hardware players to reposition mid-frame, so no
        // zone-boundary DLIs are needed — just clear any stale dynamic hooks.
        if constexpr (caps::has_blitter) return;

        mux_index_ = 0;
        void (*const h)() = Platform::hal::multiplex_dli();
        for (u8 z = 1; z < zone_count_; ++z) {
            const ZoneInfo& zi = zones_[z];
            u8* row = &mux_table_[static_cast<u8>((z - 1) * kMuxRow)];
            for (u8 p = 0; p < kPlayers; ++p) {
                row[p]            = zi.hpos[p];
                row[p + kPlayers] = zi.color[p];
            }
            im.add_dynamic_raster_hook(zi.boundary_scanline, h);
        }
    }

    // One-time setup: bind the raw multiplex DLI to this manager's flat table and
    // fire index (the single instance never moves). engine::Core::init calls this on
    // baseline backends; discarded on blitter backends, which have no boundary DLIs.
    void arm_multiplex_dli() {
        if constexpr (!caps::has_blitter) {
            Platform::hal::install_multiplex_dli(
                static_cast<u16>(reinterpret_cast<uintptr_t>(mux_table_)),
                static_cast<u16>(reinterpret_cast<uintptr_t>(&mux_index_)));
        }
    }

    // ── Collision reverse-mapping ──
    //
    // Hardware collision registers accumulate across the whole frame and don't
    // distinguish zones (ARCHITECTURE.md "Multiplex"). These map between logical
    // sprites and hardware players so the game can disambiguate.

    // Bitmask of logical sprite indices assigned to `hw_player` across all zones.
    u16 sprites_on_player(u8 hw_player) const {
        u16 mask = 0;
        for (u8 z = 0; z < zone_count_; ++z) {
            const u8 li = zones_[z].player_assignment[hw_player];
            if (li != ZoneInfo::UNUSED) mask |= static_cast<u16>(1u << li);
        }
        return mask;
    }

    // Hardware player a logical sprite is shown on (0xFF if not assigned).
    u8 player_for_sprite(u8 logical) const {
        for (u8 z = 0; z < zone_count_; ++z) {
            for (u8 p = 0; p < kPlayers; ++p) {
                if (zones_[z].player_assignment[p] == logical) return p;
            }
        }
        return 0xFF;
    }

    // Zone a logical sprite is in (0xFF if not assigned).
    u8 zone_for_sprite(u8 logical) const {
        for (u8 z = 0; z < zone_count_; ++z) {
            for (u8 p = 0; p < kPlayers; ++p) {
                if (zones_[z].player_assignment[p] == logical) return z;
            }
        }
        return 0xFF;
    }

    // ── Queries / accessors ──
    u8 zone_count() const { return zone_count_; }
    u8 active_count() const { return active_count_; }
    const ZoneInfo&      zone(u8 i) const { return zones_[i]; }
    const LogicalSprite& logical(u8 slot) const { return sprites_[slot]; }

private:
    static bool is_shown(const LogicalSprite& s) {
        return (s.flags & (LogicalSprite::FLAG_ACTIVE |
                           LogicalSprite::FLAG_VISIBLE)) ==
               (LogicalSprite::FLAG_ACTIVE | LogicalSprite::FLAG_VISIBLE);
    }

    LogicalSprite sprites_[MaxSprites] = {};
    ZoneInfo      zones_[MaxZones]     = {};
    u8            zone_count_          = 0;
    u8            active_count_        = 0;

    // Per-sprite footprint from last commit, for the exact-extent erase (ADR-022).
    // prev_player_[i] is the hardware player logical sprite i was drawn on, or
    // kNotDrawn if it wasn't; prev_y_/prev_height_ are its strip offset and height.
    // prev_height_ inits to 0 so the very first commit erases nothing.
    static constexpr u8 kNotDrawn = 0xFF;
    u8 prev_player_[MaxSprites] = {};
    u8 prev_y_[MaxSprites]      = {};
    u8 prev_height_[MaxSprites] = {};

    // Buffered missile positions (no multiplexing, ADR-025).
    u8 missile_x_[kMissiles]      = {};
    u8 missile_y_[kMissiles]      = {};
    u8 missile_height_[kMissiles] = {};

    // Per-missile footprint from last commit, for the exact-extent 2-bit-field erase
    // in the shared missile strip (mirrors prev_y_/prev_height_ for players).
    // missile_prev_height_ inits to 0 so the first commit erases nothing.
    u8 missile_prev_y_[kMissiles]      = {};
    u8 missile_prev_height_[kMissiles] = {};

    // Sorted active-sprite indices, rebuilt each update_zones().
    u8 order_[MaxSprites] = {};

    SpriteVerticalResolution res_ = SpriteVerticalResolution::SingleLine;

    // Flat per-boundary register table the raw multiplex DLI walks: one kMuxRow-byte
    // row per zone after the first, [HPOSP0..3, COLPM0..3], in fire (scanline)
    // order. mux_index_ is the row the next boundary DLI will consume; reset to 0
    // each frame by build_raster_hooks and bumped by the DLI (engine HAL +
    // platform/atari/dli_dispatch.h own the hardware side).
    static constexpr u8 kMuxRow = kPlayers * 2;   // 4 HPOSP + 4 COLPM
    u8 mux_table_[MaxZones * kMuxRow] = {};
    u8 mux_index_ = 0;
};

} // namespace engine

#endif // ENGINE_SPRITES_H
