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
            // Boundary: zone 0 from the top; later zones at the midpoint of the
            // gap between adjacent groups.
            if (z == 0) {
                zi.boundary_scanline = 0;
            } else {
                const u8 prev_last  = sprites_[order_[first - 1]].y;
                const u8 this_first = sprites_[order_[first]].y;
                zi.boundary_scanline =
                    static_cast<u8>((prev_last + this_first) / 2);
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

        // 1. Clear last frame's dirty range for each player.
        for (u8 p = 0; p < kPlayers; ++p) {
            if (dirty_min_y_[p] <= dirty_max_y_[p]) {
                const u16 base = Platform::hal::sprite_strip_offset(res, p);
                for (u16 y = dirty_min_y_[p]; y <= dirty_max_y_[p]; ++y) {
                    sprite_base[base + y] = 0;
                }
            }
        }

        // 2. Reset working dirty range.
        u8 new_min[kPlayers];
        u8 new_max[kPlayers];
        for (u8 p = 0; p < kPlayers; ++p) { new_min[p] = 0xFF; new_max[p] = 0; }

        // 3. Copy each active sprite's shape into its assigned player's strip.
        for (u8 z = 0; z < zone_count_; ++z) {
            const ZoneInfo& zi = zones_[z];
            for (u8 p = 0; p < kPlayers; ++p) {
                const u8 li = zi.player_assignment[p];
                if (li == ZoneInfo::UNUSED) continue;
                const LogicalSprite& s = sprites_[li];
                const u16 base = Platform::hal::sprite_strip_offset(res, p);
                const u8 yb = (res_ == SpriteVerticalResolution::SingleLine)
                                  ? s.y : static_cast<u8>(s.y >> 1);
                for (u8 r = 0; r < s.height; ++r) {
                    sprite_base[base + yb + r] = s.shape[r];
                }
                const u8 lo_y = yb;
                const u8 hi_y = static_cast<u8>(yb + s.height - 1);
                if (lo_y < new_min[p]) new_min[p] = lo_y;
                if (hi_y > new_max[p]) new_max[p] = hi_y;
            }
        }

        // 4. Write zone-0 sprite positions + colours (the frame service sets the first
        //    zone; later zones are armed by the boundary raster hooks). The colour register is the
        //    hardware register, written after the OS shadow→hardware copy so it
        //    holds for the frame and gives this zone's sprites their colours.
        if (zone_count_ > 0) {
            for (u8 p = 0; p < kPlayers; ++p) {
                Platform::hal::set_sprite_x(p, zones_[0].hpos[p]);
                Platform::hal::set_sprite_color(p, zones_[0].color[p]);
            }
        }
        // Missiles are direct, not multiplexed (ADR-025).
        for (u8 m = 0; m < kMissiles; ++m) {
            Platform::hal::set_projectile_x(m, missile_x_[m]);
        }

        // 5. Record this frame's dirty ranges for next frame's clear.
        for (u8 p = 0; p < kPlayers; ++p) {
            dirty_min_y_[p] = new_min[p];
            dirty_max_y_[p] = new_max[p];
        }
    }

    // ── Raster-hook construction ──
    //
    // Register a raw boundary raster hook for each zone after the first (zone 0's
    // positions are set by the frame-service commit). The handler walks per-frame static
    // state: s_hook_zone_ is reset here and advanced by each hook, which writes
    // the corresponding zone's sprite positions through the HAL.
    //
    // This implements the position-write logic and the per-frame registration. The
    // production raw handler also needs the backend prologue/epilogue (chain the
    // next raster vector via InterruptManager::next_raster_addr()); that platform
    // glue is added with the live hardware path — it is never executed under the
    // simulator (no hardware raster), consistent with the backend dispatcher.
    template <typename IM>
    void build_raster_hooks(IM& im) {
        // A blitter backend has no hardware players to reposition mid-frame, so no
        // zone-boundary DLIs are needed — just clear any stale dynamic hooks.
        if constexpr (caps::has_blitter) {
            im.begin_dynamic();
            return;
        }
        s_active_   = this;
        s_hook_zone_ = 0;
        im.begin_dynamic();
        for (u8 z = 1; z < zone_count_; ++z) {
            im.add_dynamic_raster_hook(zones_[z].boundary_scanline, &zone_boundary_hook);
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

    // Raw boundary raster hook: advance to the next zone and write its sprite positions
    // and colours (so each zone's sprites show their own colour, not the slot's).
    static void zone_boundary_hook() {
        SpriteManager* m = s_active_;
        const u8 z = static_cast<u8>(++s_hook_zone_);
        const ZoneInfo& zi = m->zones_[z];
        for (u8 p = 0; p < kPlayers; ++p) {
            Platform::hal::set_sprite_x(p, zi.hpos[p]);
            Platform::hal::set_sprite_color(p, zi.color[p]);
        }
    }

    LogicalSprite sprites_[MaxSprites] = {};
    ZoneInfo      zones_[MaxZones]     = {};
    u8            zone_count_          = 0;
    u8            active_count_        = 0;

    // Tracked dirty Y range per player (ADR-022). Init empty (min>max) so the
    // first commit clears nothing.
    u8 dirty_min_y_[kPlayers] = {0xFF, 0xFF, 0xFF, 0xFF};
    u8 dirty_max_y_[kPlayers] = {0, 0, 0, 0};

    // Buffered missile positions (no multiplexing, ADR-025).
    u8 missile_x_[kMissiles]      = {};
    u8 missile_y_[kMissiles]      = {};
    u8 missile_height_[kMissiles] = {};

    // Sorted active-sprite indices, rebuilt each update_zones().
    u8 order_[MaxSprites] = {};

    SpriteVerticalResolution res_ = SpriteVerticalResolution::SingleLine;

    // Per-frame raster-hook walker state (one set per template instantiation).
    static SpriteManager* s_active_;
    static u8             s_hook_zone_;
};

template <typename Platform, u8 MaxSprites, u8 MaxZones>
SpriteManager<Platform, MaxSprites, MaxZones>*
    SpriteManager<Platform, MaxSprites, MaxZones>::s_active_ = nullptr;

template <typename Platform, u8 MaxSprites, u8 MaxZones>
u8 SpriteManager<Platform, MaxSprites, MaxZones>::s_hook_zone_ = 0;

} // namespace engine

#endif // ENGINE_SPRITES_H
