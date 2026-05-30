#ifndef ENGINE_POOL_H
#define ENGINE_POOL_H

// pool.h — fixed-size, statically allocated object pools.
//
// Two pool types, each optimized for a different access pattern
// (DECISIONS.md ADR-001):
//
//   SlotPool<T, N>    bitmap occupancy, STABLE indices. Use when something
//                     external references a slot by index (sprite-to-hardware
//                     mapping, collision targets).
//
//   PackedPool<T, N>  dense packing, UNSTABLE indices (release swaps with the
//                     last element). Use for iteration-only collections
//                     (particles, effects, queued events).
//
// Neither pool allocates from the heap, throws, or uses virtual functions
// (CONSTRAINTS.md). Acquired slots have UNDEFINED content — the caller must
// initialise every field it uses (DECISIONS.md ADR-002).

#include "types.h"

namespace engine {

// ── SlotPool<T, N> ───────────────────────────────────────────────────
//
// Fixed-size pool of N objects with stable indices, backed by a one-byte
// occupancy bitmap. N must be 1..8 (one byte tracks 8 slots); a future
// SlotPool16 covers 9..16.

template <typename T, u8 N>
class SlotPool {
    static_assert(N >= 1, "SlotPool capacity must be at least 1");
    static_assert(N <= 8, "SlotPool capacity must be 1-8; use SlotPool16 for 9-16");

public:
    // Marks every slot free. Does NOT zero the underlying storage (ADR-002).
    void clear() { occupied_ = 0; }

    // Acquire the first free slot. Returns a pointer to undefined storage the
    // caller must initialise, or nullptr if the pool is full.
    T* acquire() {
        for (u8 i = 0; i < N; ++i) {
            if (!(occupied_ & bit_mask[i])) {
                occupied_ |= bit_mask[i];
                return &data_[i];
            }
        }
        return nullptr;
    }

    // Acquire and report the assigned slot index (useful for sprite mapping).
    T* acquire(u8& slot) {
        for (u8 i = 0; i < N; ++i) {
            if (!(occupied_ & bit_mask[i])) {
                occupied_ |= bit_mask[i];
                slot = i;
                return &data_[i];
            }
        }
        return nullptr;
    }

    // Release by index — natural in collision resolution (ADR-003).
    void release(u8 idx) { occupied_ &= static_cast<u8>(~bit_mask[idx]); }

    // Release by pointer — natural after acquire() (ADR-003).
    void release(T* ptr) { release(static_cast<u8>(ptr - data_)); }

    // ── Queries ──
    bool active(u8 idx) const { return (occupied_ & bit_mask[idx]) != 0; }

    u8 count() const {
        u8 c = 0;
        for (u8 i = 0; i < N; ++i) {
            if (occupied_ & bit_mask[i]) ++c;
        }
        return c;
    }

    u8   available() const { return static_cast<u8>(N - count()); }
    bool full() const { return count() == N; }
    bool empty() const { return occupied_ == 0; }

    static constexpr u8 capacity() { return N; }

    // ── Direct access (no bounds or active check) ──
    T&       operator[](u8 idx)       { return data_[idx]; }
    const T& operator[](u8 idx) const { return data_[idx]; }

    // ── Lambda iteration over occupied slots only ──
    template <typename F>
    void for_each(F&& fn) {
        for (u8 i = 0; i < N; ++i) {
            if (occupied_ & bit_mask[i]) fn(data_[i]);
        }
    }

    template <typename F>
    void for_each_indexed(F&& fn) {
        for (u8 i = 0; i < N; ++i) {
            if (occupied_ & bit_mask[i]) fn(i, data_[i]);
        }
    }

    // ── Range-for over occupied slots only ──
    class iterator {
    public:
        iterator(SlotPool* pool, u8 i) : pool_(pool), i_(i) { skip_free(); }

        T&        operator*() const { return pool_->data_[i_]; }
        iterator& operator++() { ++i_; skip_free(); return *this; }
        bool      operator!=(const iterator& o) const { return i_ != o.i_; }

    private:
        void skip_free() {
            while (i_ < N && !(pool_->occupied_ & bit_mask[i_])) ++i_;
        }
        SlotPool* pool_;
        u8        i_;
    };

    iterator begin() { return iterator(this, 0); }
    iterator end()   { return iterator(this, N); }

private:
    T  data_[N];
    u8 occupied_ = 0;
};

// ── PackedPool<T, N> ─────────────────────────────────────────────────
//
// Dense-packed pool: elements 0..count()-1 are always active. release() moves
// the former last element into the freed slot, keeping storage contiguous.
//
// WARNING: indices are UNSTABLE. After release(idx), whatever was at
// count()-1 now lives at idx. Never hold a PackedPool index across a release.

template <typename T, u8 N>
class PackedPool {
    static_assert(N >= 1, "PackedPool capacity must be at least 1");
    // N is a u8, so capacity already fits the count_ field (max 255).

public:
    // Resets to empty. Does NOT zero the underlying storage (ADR-002).
    void clear() { count_ = 0; }

    // Append a new element. Returns a pointer to undefined storage the caller
    // must initialise, or nullptr if the pool is full.
    T* acquire() {
        if (count_ >= N) return nullptr;
        return &data_[count_++];
    }

    // Release by index — swaps the former last element into idx, then shrinks.
    void release(u8 idx) {
        --count_;
        if (idx != count_) data_[idx] = data_[count_];
    }

    // Release by pointer.
    void release(T* ptr) { release(static_cast<u8>(ptr - data_)); }

    // ── Queries ──
    u8   count() const { return count_; }
    u8   available() const { return static_cast<u8>(N - count_); }
    bool full() const { return count_ == N; }
    bool empty() const { return count_ == 0; }

    static constexpr u8 capacity() { return N; }

    T&       operator[](u8 idx)       { return data_[idx]; }
    const T& operator[](u8 idx) const { return data_[idx]; }

    // ── Iteration — every element is active, so this is zero-overhead ──
    template <typename F>
    void for_each(F&& fn) {
        for (u8 i = 0; i < count_; ++i) fn(data_[i]);
    }

    T* begin() { return data_; }
    T* end()   { return data_ + count_; }

private:
    T  data_[N];
    u8 count_ = 0;
};

} // namespace engine

#endif // ENGINE_POOL_H
