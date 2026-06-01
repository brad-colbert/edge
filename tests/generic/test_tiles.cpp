// test_tiles.cpp — unit tests for engine/tiles.h.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`; main()'s
// return value becomes the process exit code (0 = pass) for CTest.
//
// Everything here is platform-independent: the TileMap/CharsetData assets are
// pure data, and the one hardware touch (bind_charset_page) is driven through a MOCK
// HAL that records the charset-base write, so no real backend is needed. The live
// charset-base write in the Atari HAL is verified separately on Altirra/Fujisan.

#include <stdint.h>
#include <stdio.h>

#include <engine/tiles.h>

using engine::u8;
using engine::u16;

using engine::CharsetData;
using engine::Charset1K;
using engine::Charset512;
using engine::TileManager;
using engine::make_charset;
using engine::make_map;

// ── Mock platform ─────────────────────────────────────────────────────
//
// Records the last charset-base page written and how many writes occurred, so
// bind_charset_page can be asserted exactly without backend hardware.
struct MockHal {
    static u8       chbase;
    static unsigned chbase_writes;

    static void set_charset_base(u8 page) { chbase = page; ++chbase_writes; }

    static void reset() { chbase = 0; chbase_writes = 0; }
};
u8       MockHal::chbase        = 0;
unsigned MockHal::chbase_writes = 0;

struct MockPlatform {
    using hal = MockHal;
};

// ── Runtime harness ────────────────────────────────────────────────────

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Shared test assets ─────────────────────────────────────────────────
//
// Build a 16×8 map filled row-major so tile[col,row] == row*16 + col. Generated
// at compile time into a flat braced array (constexpr lambda would need C++17;
// a plain helper keeps it simple).
template <u16 W, u16 H>
struct RampMap {
    u8 data[W * H];
    constexpr RampMap() : data{} {
        for (u16 r = 0; r < H; ++r)
            for (u16 c = 0; c < W; ++c)
                data[r * W + c] = static_cast<u8>(r * W + c);
    }
};

// ── TileMap reads ───────────────────────────────────────────────────────

static void test_tile_at() {
    constexpr RampMap<16, 8> ramp;
    constexpr auto map = make_map<16, 8>(ramp.data);

    CHECK(map.width == 16);
    CHECK(map.height == 8);
    CHECK(map.tile_at(0, 0) == 0);
    CHECK(map.tile_at(15, 7) == 127);   // 7*16 + 15
    CHECK(map.tile_at(5, 3) == 53);     // 3*16 + 5
}

// ── set_tile round-trips, leaves neighbors alone ───────────────────────

static void test_set_tile() {
    RampMap<16, 8> ramp;                     // mutable RAM map
    auto map = make_map<16, 8>(ramp.data);

    CHECK(map.tile_at(5, 3) == 53);
    map.set_tile(5, 3, 200);
    CHECK(map.tile_at(5, 3) == 200);         // change reads back
    CHECK(map.tile_at(4, 3) == 52);          // neighbor untouched
    CHECK(map.tile_at(6, 3) == 54);
}

// ── make_charset: size + data accessible ───────────────────────────────

static void test_make_charset() {
    // 1024-byte charset where byte i holds i & 0xFF.
    struct Ramp {
        u8 data[1024];
        constexpr Ramp() : data{} {
            for (u16 i = 0; i < 1024; ++i) data[i] = static_cast<u8>(i);
        }
    };
    constexpr Ramp ramp;
    constexpr auto cs = make_charset(ramp.data);

    CHECK(cs.size == 1024);
    CHECK(cs.data[0] == 0);
    CHECK(cs.data[255] == 255);
    CHECK(cs.data[256] == 0);                // wrapped (256 & 0xFF)
    CHECK(cs.data[1023] == static_cast<u8>(1023));
}

// ── CharsetData sizes are fixed at compile time ────────────────────────

static_assert(sizeof(CharsetData<1024>) == 1024, "1K charset is 1024 bytes");
static_assert(sizeof(CharsetData<512>) == 512,   "512-byte charset is 512 bytes");
static_assert(Charset1K::size == 1024, "Charset1K is 1024 bytes");
static_assert(Charset512::size == 512,  "Charset512 is 512 bytes");

// ── TileManager: init_charset copies, bind_charset_page writes, viewport stores ─

static void test_tile_manager() {
    MockHal::reset();
    TileManager<MockPlatform> tiles;

    // init_charset copies exactly ::size bytes into the destination buffer.
    struct Ramp {
        u8 data[512];
        constexpr Ramp() : data{} {
            for (u16 i = 0; i < 512; ++i) data[i] = static_cast<u8>(i ^ 0x5A);
        }
    };
    constexpr Ramp ramp;
    constexpr auto cs = make_charset(ramp.data);   // CharsetData<512>

    u8 dest[512];
    for (u16 i = 0; i < 512; ++i) dest[i] = 0;
    tiles.init_charset(cs, dest);
    bool copied = true;
    for (u16 i = 0; i < 512; ++i)
        if (dest[i] != static_cast<u8>(i ^ 0x5A)) copied = false;
    CHECK(copied);

    // bind_charset_page forwards the page to the HAL.
    tiles.bind_charset_page(0x9C);
    CHECK(MockHal::chbase == 0x9C);
    CHECK(MockHal::chbase_writes == 1);

    // set_viewport stores the position for later tilemap lookups.
    CHECK(tiles.viewport_x() == 0);
    CHECK(tiles.viewport_y() == 0);
    tiles.set_viewport(40, 17);
    CHECK(tiles.viewport_x() == 40);
    CHECK(tiles.viewport_y() == 17);
}

int main() {
    test_tile_at();
    test_set_tile();
    test_make_charset();
    test_tile_manager();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
