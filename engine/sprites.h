#ifndef ENGINE_SPRITES_H
#define ENGINE_SPRITES_H

// sprites.h — portable logical-sprite manager and multiplexer.
//
// This is the Sprites subsystem (ARCHITECTURE.md "Sprites" / "Multiplex"). The
// game writes logical sprite state during its render phase; the engine commits
// it to player/missile memory during the VBI (one-frame buffering, ADR-009).
// When more sprites are active than there are hardware players (4), the
// multiplexer divides the screen into vertical zones and reprograms the player
// horizontal positions via a raw DLI at each zone boundary.
//
// Like every engine header it reaches hardware ONLY through the `Platform`
// template parameter (Dependency Rule 2) — never by including a platform header.
// The P/M memory layout (where each player's strip lives, how a scanline maps to
// a byte offset) is platform knowledge surfaced through Platform::hal
// (pm_player_offset / write_hposp), mirroring how interrupt.h gets the DLI
// dispatcher address.
//
// Design fixed by ADR-022 (tracked-range P/M clear), ADR-024 (always insertion-
// sort by Y), ADR-025 (missiles not multiplexed yet; ZoneInfo reserves room).
//
// Depends on types.h and interrupt.h.

#include "interrupt.h"
#include "types.h"

namespace engine {

// ── SpriteShape ───────────────────────────────────────────────────────
//
// A compile-time sprite shape. `Width` is the on-screen pixel width (8 single /
// 16 double; a P/M player is always one byte wide in memory, so `data` is one
// byte per scanline regardless). `Height` is the scanline count. The type
// carries its dimensions as constexpr members so `sprite()` deduces a sprite's
// height from the shape object — there is no separate height argument
// (API_DESIGN.md "Sprite Shapes"). This is the value `Game::make_sprite<W,H>`
// will return.
template <u8 Width, u8 Height>
struct SpriteShape {
    static constexpr u8 width  = Width;
    static constexpr u8 height = Height;
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

// ── P/M resolution ────────────────────────────────────────────────────
//
// Per-screen P/M resolution (ADR-023). SingleLine: 1-scanline Y precision, 256
// bytes/player. DoubleLine: 2-scanline steps, 128 bytes/player. The underlying
// values (0/1) are the `res` byte passed to Platform::hal::pm_player_offset.
enum class PMRes : u8 { SingleLine, DoubleLine };

// ── LogicalSprite ─────────────────────────────────────────────────────
//
// One logical sprite's buffered state. 6 bytes on the 6502 (pointer = 2 bytes).
struct LogicalSprite {
    u8        x, y, height;
    const u8* shape;
    u8        flags;

    static constexpr u8 FLAG_ACTIVE  = 0x01;
    static constexpr u8 FLAG_VISIBLE = 0x02;
};
static_assert(sizeof(LogicalSprite) == 6, "LogicalSprite must be 6 bytes");

// ── ZoneInfo ──────────────────────────────────────────────────────────
//
// One multiplex zone: the scanline at which it becomes active and, per hardware
// player, which logical sprite it shows (0xFF = unused) and at what horizontal
// position. 9 active bytes; 8 reserved for future missile multiplexing (ADR-025
// — keep the struct/DLI shaped for it without implementing it yet).
struct ZoneInfo {
    u8 boundary_scanline;
    u8 player_assignment[4];
    u8 hpos[4];
    u8 missile_reserved[8];

    static constexpr u8 UNUSED = 0xFF;
};
static_assert(sizeof(ZoneInfo) == 17, "ZoneInfo must be 9 + 8 reserved bytes");

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

    // ── Logical sprite state (buffered; committed during VBI) ──
    template <typename Shape>
    void sprite(u8 slot, const Shape& shape, u8 x, u8 y) {
        sprites_[slot] = LogicalSprite{
            x, y, Shape::height, shape.data,
            static_cast<u8>(LogicalSprite::FLAG_ACTIVE |
                            LogicalSprite::FLAG_VISIBLE)};
    }

    // Missiles are not multiplexed (ADR-025) — the 4 hardware missiles are used
    // directly. Position is buffered and pushed to HPOSM during commit.
    void missile(u8 index, u8 x, u8 y, u8 height) {
        missile_x_[index]      = x;
        missile_y_[index]      = y;
        missile_height_[index] = height;
    }

    void sprite_hide(u8 slot) { sprites_[slot].flags = 0; }
    void sprite_hide_all() {
        for (u8 i = 0; i < MaxSprites; ++i) sprites_[i].flags = 0;
    }

    // P/M resolution is per-screen (ADR-023); the screen manager sets it on
    // transition. Default single-line, which most games use.
    void  set_resolution(PMRes res) { res_ = res; }
    PMRes resolution() const { return res_; }

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
                } else {
                    zi.player_assignment[p] = ZoneInfo::UNUSED;
                    zi.hpos[p]              = 0;
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

    // ── Commit buffered state into player memory ──
    //
    // Tracked-range clear (ADR-022): clear only last frame's dirty Y range per
    // player, then write each active sprite's shape into its player strip at the
    // sprite's Y offset, write zone-0 horizontal positions, push missile
    // positions, and record this frame's dirty ranges for the next clear.
    // `pmbase` points at the P/M base (a real address on hardware, a test buffer
    // under the simulator).
    void commit(u8* pmbase) {
        const u8 res = static_cast<u8>(res_);

        // 1. Clear last frame's dirty range for each player.
        for (u8 p = 0; p < kPlayers; ++p) {
            if (dirty_min_y_[p] <= dirty_max_y_[p]) {
                const u16 base = Platform::hal::pm_player_offset(res, p);
                for (u16 y = dirty_min_y_[p]; y <= dirty_max_y_[p]; ++y) {
                    pmbase[base + y] = 0;
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
                const u16 base = Platform::hal::pm_player_offset(res, p);
                const u8 yb = (res_ == PMRes::SingleLine)
                                  ? s.y : static_cast<u8>(s.y >> 1);
                for (u8 r = 0; r < s.height; ++r) {
                    pmbase[base + yb + r] = s.shape[r];
                }
                const u8 lo_y = yb;
                const u8 hi_y = static_cast<u8>(yb + s.height - 1);
                if (lo_y < new_min[p]) new_min[p] = lo_y;
                if (hi_y > new_max[p]) new_max[p] = hi_y;
            }
        }

        // 4. Write zone-0 player positions (the VBI sets the first zone; later
        //    zones are armed by the boundary DLIs).
        if (zone_count_ > 0) {
            for (u8 p = 0; p < kPlayers; ++p) {
                Platform::hal::write_hposp(p, zones_[0].hpos[p]);
            }
        }
        // Missiles are direct, not multiplexed (ADR-025).
        for (u8 m = 0; m < kMissiles; ++m) {
            Platform::hal::write_hposm(m, missile_x_[m]);
        }

        // 5. Record this frame's dirty ranges for next frame's clear.
        for (u8 p = 0; p < kPlayers; ++p) {
            dirty_min_y_[p] = new_min[p];
            dirty_max_y_[p] = new_max[p];
        }
    }

    // ── DLI handler construction ──
    //
    // Register a raw boundary DLI for each zone after the first (zone 0's
    // positions are set by the VBI commit). The handler walks per-frame static
    // state: s_dli_zone_ is reset here and advanced by each DLI, which writes
    // the corresponding zone's player positions through the HAL.
    //
    // This implements the HPOS-write logic and the per-frame registration. The
    // production raw handler also needs the assembly prologue/epilogue (save A,
    // chain VDSLST via InterruptManager::next_dli_addr(), RTI); that platform
    // glue is added with the live ANTIC path — it is never executed under the
    // simulator (no ANTIC), consistent with dli_dispatch.h.
    template <typename IM>
    void build_dli_handlers(IM& im) {
        s_active_   = this;
        s_dli_zone_ = 0;
        im.begin_dynamic();
        for (u8 z = 1; z < zone_count_; ++z) {
            im.add_dynamic_dli(zones_[z].boundary_scanline, &zone_dli);
        }
    }

    // ── Collision reverse-mapping ──
    //
    // GTIA collision registers accumulate across the whole frame and don't
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

    // Raw boundary DLI: advance to the next zone and write its player positions.
    static void zone_dli() {
        SpriteManager* m = s_active_;
        const u8 z = static_cast<u8>(++s_dli_zone_);
        const ZoneInfo& zi = m->zones_[z];
        for (u8 p = 0; p < kPlayers; ++p) {
            Platform::hal::write_hposp(p, zi.hpos[p]);
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

    PMRes res_ = PMRes::SingleLine;

    // Per-frame DLI walker state (one set per template instantiation).
    static SpriteManager* s_active_;
    static u8             s_dli_zone_;
};

template <typename Platform, u8 MaxSprites, u8 MaxZones>
SpriteManager<Platform, MaxSprites, MaxZones>*
    SpriteManager<Platform, MaxSprites, MaxZones>::s_active_ = nullptr;

template <typename Platform, u8 MaxSprites, u8 MaxZones>
u8 SpriteManager<Platform, MaxSprites, MaxZones>::s_dli_zone_ = 0;

} // namespace engine

#endif // ENGINE_SPRITES_H
