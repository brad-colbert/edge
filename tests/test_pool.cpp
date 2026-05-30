// test_pool.cpp — unit tests for engine::SlotPool and engine::PackedPool.
//
// Built for the llvm-mos `mos-sim` platform and run under `mos-sim`. The
// simulator wires stdout to the host and returns main()'s value as the
// process exit code, so CTest treats a 0 return as pass.
//
// No <cassert> (abort path), no exceptions: failures are counted and reported,
// and main() returns the failure count != 0.

#include <stdio.h>

#include <engine/pool.h>

using engine::u8;
using engine::u16;
using engine::i8;

static unsigned g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ── Test fixtures (plain data) ───────────────────────────────────────

struct Enemy {
    u8 x, y, speed;
};

struct Particle {
    u8 x, y;
    i8 dx, dy;
};

// ── compile-time guards ──────────────────────────────────────────────

static_assert(engine::SlotPool<Enemy, 8>::capacity() == 8,
              "SlotPool capacity() must be a compile-time constant");
static_assert(engine::PackedPool<Particle, 16>::capacity() == 16,
              "PackedPool capacity() must be a compile-time constant");

// ── bit_mask sanity ──────────────────────────────────────────────────

static void test_bit_mask() {
    for (u8 i = 0; i < 8; ++i) {
        CHECK(engine::bit_mask[i] == static_cast<u8>(1u << i));
    }
}

// ── SlotPool: acquire fills to capacity then returns nullptr ─────────

static void test_slotpool_acquire() {
    engine::SlotPool<Enemy, 4> pool;
    pool.clear();

    CHECK(pool.empty());
    CHECK(!pool.full());
    CHECK(pool.count() == 0);
    CHECK(pool.available() == 4);

    for (u8 i = 0; i < 4; ++i) {
        u8 slot = 0xFF;
        Enemy* e = pool.acquire(slot);
        CHECK(e != nullptr);
        CHECK(slot == i);            // stable: slots assigned in order
        CHECK(pool.active(slot));
        e->x = i;                    // caller must initialise (ADR-002)
    }

    CHECK(pool.full());
    CHECK(pool.count() == 4);
    CHECK(pool.available() == 0);
    CHECK(pool.acquire() == nullptr);   // no free slot
}

// ── SlotPool: release by pointer ─────────────────────────────────────

static void test_slotpool_release_by_pointer() {
    engine::SlotPool<Enemy, 4> pool;
    pool.clear();

    Enemy* a = pool.acquire();
    Enemy* b = pool.acquire();
    CHECK(a != nullptr && b != nullptr);
    CHECK(pool.count() == 2);

    pool.release(a);                 // release by pointer
    CHECK(pool.count() == 1);
    CHECK(!pool.active(0));           // slot 0 (a) freed
    CHECK(pool.active(1));            // slot 1 (b) still held

    // The freed slot is reused on the next acquire.
    u8 slot = 0xFF;
    Enemy* c = pool.acquire(slot);
    CHECK(c != nullptr);
    CHECK(slot == 0);
    CHECK(c == a);                   // same storage reclaimed
}

// ── SlotPool: release by index ───────────────────────────────────────

static void test_slotpool_release_by_index() {
    engine::SlotPool<Enemy, 4> pool;
    pool.clear();

    pool.acquire();   // slot 0
    pool.acquire();   // slot 1
    pool.acquire();   // slot 2
    CHECK(pool.count() == 3);

    pool.release(1);                 // release by index
    CHECK(pool.count() == 2);
    CHECK(pool.active(0));
    CHECK(!pool.active(1));
    CHECK(pool.active(2));
}

// ── SlotPool: range-for iteration visits occupied slots only ─────────

static void test_slotpool_iteration() {
    engine::SlotPool<Enemy, 4> pool;
    pool.clear();

    for (u8 i = 0; i < 4; ++i) {
        Enemy* e = pool.acquire();
        e->x = static_cast<u8>(10 + i);   // 10, 11, 12, 13 in slots 0..3
    }

    pool.release(static_cast<u8>(1));      // free the middle slot

    // Range-for must skip the freed slot and visit exactly slots 0, 2, 3.
    u8 visited = 0;
    u16 sum = 0;
    for (auto& e : pool) {
        ++visited;
        sum += e.x;
    }
    CHECK(visited == 3);
    CHECK(sum == static_cast<u16>(10 + 12 + 13));   // 11 (slot 1) skipped
    CHECK(!pool.active(1));
}

// ── SlotPool: for_each_indexed reports stable indices ────────────────

static void test_slotpool_for_each_indexed() {
    engine::SlotPool<Enemy, 4> pool;
    pool.clear();

    for (u8 i = 0; i < 4; ++i) {
        Enemy* e = pool.acquire();
        e->x = static_cast<u8>(i);
    }
    pool.release(static_cast<u8>(2));      // free slot 2

    // Index passed to the callback must equal the slot the element lives in,
    // and slot 2 must not be visited.
    u8 visited = 0;
    bool index_matches_payload = true;
    bool saw_slot_2 = false;
    pool.for_each_indexed([&](u8 idx, Enemy& e) {
        ++visited;
        if (e.x != idx) index_matches_payload = false;   // x was set to its slot
        if (idx == 2) saw_slot_2 = true;
    });
    CHECK(visited == 3);
    CHECK(index_matches_payload);
    CHECK(!saw_slot_2);
}

// ── PackedPool: acquire / release-by-pointer basics ──────────────────

static void test_packedpool_acquire() {
    engine::PackedPool<Particle, 4> pool;
    pool.clear();

    CHECK(pool.empty());
    for (u8 i = 0; i < 4; ++i) {
        Particle* p = pool.acquire();
        CHECK(p != nullptr);
        p->x = static_cast<u8>(i);
    }
    CHECK(pool.full());
    CHECK(pool.count() == 4);
    CHECK(pool.acquire() == nullptr);

    Particle* last = &pool[3];
    pool.release(last);                    // release by pointer
    CHECK(pool.count() == 3);
}

// ── PackedPool: density preserved after release ──────────────────────

static void test_packedpool_density_after_release() {
    engine::PackedPool<Particle, 8> pool;
    pool.clear();

    // Tag each element with a unique id in .x: 0,1,2,3,4.
    const u8 n = 5;
    for (u8 i = 0; i < n; ++i) {
        Particle* p = pool.acquire();
        p->x = i;
    }
    CHECK(pool.count() == n);

    // Release a middle index (1). The former last element (id 4, at index 4)
    // is swapped into index 1; count drops to 4.
    const u8 last_id = pool[static_cast<u8>(n - 1)].x;   // == 4
    pool.release(static_cast<u8>(1));
    CHECK(pool.count() == static_cast<u8>(n - 1));       // == 4
    CHECK(pool[1].x == last_id);                         // hole filled by last

    // Density: every index 0..count-1 is live and reachable with no gaps.
    // The surviving ids are {0, 4, 2, 3} (order reflects the swap). Verify the
    // released id (1) is gone and exactly count elements are iterated.
    u8 iterated = 0;
    bool saw_released_id = false;
    pool.for_each([&](Particle& p) {
        ++iterated;
        if (p.x == 1) saw_released_id = true;
    });
    CHECK(iterated == pool.count());
    CHECK(!saw_released_id);

    // Range-for must visit the same contiguous run.
    u8 iterated2 = 0;
    for (auto& p : pool) {
        (void)p;
        ++iterated2;
    }
    CHECK(iterated2 == pool.count());
}

int main() {
    test_bit_mask();

    test_slotpool_acquire();
    test_slotpool_release_by_pointer();
    test_slotpool_release_by_index();
    test_slotpool_iteration();
    test_slotpool_for_each_indexed();

    test_packedpool_acquire();
    test_packedpool_density_after_release();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%u FAILURES\n", g_failures);
    }
    return g_failures != 0;
}
