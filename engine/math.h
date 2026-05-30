#ifndef ENGINE_MATH_H
#define ENGINE_MATH_H

// math.h — fixed-point arithmetic, lookup tables, and fast integer math.
//
// Sits beside types.h at the bottom of the dependency graph (no engine
// dependencies; see docs/ARCHITECTURE.md "Dependency Rules"). 6502 has no FPU
// (DECISIONS.md ADR-011), so runtime math is integer / fixed-point only.
//
// The trig tables are synthesised at COMPILE TIME. The generators below use
// `double` during constant evaluation only — it runs on the host compiler and
// produces no floating-point code; the emitted artifacts are plain `i8` ROM
// tables. No floating point reaches the 6502 (CONSTRAINTS.md).

#include "types.h"

namespace engine {

// ── Direction tables ─────────────────────────────────────────────────
//
// Indexed by direction: 0 = up, 1 = right, 2 = down, 3 = left.
// (Matches the player-facing convention in API_DESIGN.md's reference game.)

inline constexpr i8 dir_x[4] = {  0, 1, 0, -1 };
inline constexpr i8 dir_y[4] = { -1, 0, 1,  0 };

// ── Trig table generation (compile-time only) ────────────────────────

namespace detail {

constexpr double pi = 3.14159265358979323846;

// constexpr sine via range-reduced Taylor series. std::sin is not constexpr
// under clang, so we roll our own. Plenty accurate for an 8-bit table.
constexpr double cexpr_sin(double x) {
    while (x >  pi) x -= 2.0 * pi;
    while (x < -pi) x += 2.0 * pi;
    const double x2 = x * x;
    double term = x;   // first term: x^1 / 1!
    double sum  = x;
    for (u8 n = 1; n < 12; ++n) {
        // term_{n} = term_{n-1} * (-x^2) / ((2n)(2n+1))
        term *= -x2 / (static_cast<double>(2u * n) * static_cast<double>(2u * n + 1u));
        sum  += term;
    }
    return sum;
}

constexpr double cexpr_cos(double x) { return cexpr_sin(x + pi / 2.0); }

// Round a scaled value to the nearest i8 (truncation toward zero after the
// half-step nudge). Range stays within [-127, 127].
constexpr i8 to_i8(double scaled) {
    return static_cast<i8>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

struct Table256 {
    i8 v[256];
};

constexpr Table256 make_sin() {
    Table256 t{};
    for (u16 i = 0; i < 256; ++i) {
        const double angle = static_cast<double>(i) / 256.0 * 2.0 * pi;
        t.v[i] = to_i8(cexpr_sin(angle) * 127.0);
    }
    return t;
}

constexpr Table256 make_cos() {
    Table256 t{};
    for (u16 i = 0; i < 256; ++i) {
        const double angle = static_cast<double>(i) / 256.0 * 2.0 * pi;
        t.v[i] = to_i8(cexpr_cos(angle) * 127.0);
    }
    return t;
}

inline constexpr Table256 sin_table = make_sin();
inline constexpr Table256 cos_table = make_cos();

} // namespace detail

// ── Lookup tables ────────────────────────────────────────────────────
//
// 256-entry signed sine/cosine. Input 0..255 maps to 0..360°, output is
// i8 in [-127, +127]. sin8[0] == 0, sin8[64] == 127 (quarter-period peak).

inline constexpr const i8 (&sin8)[256] = detail::sin_table.v;
inline constexpr const i8 (&cos8)[256] = detail::cos_table.v;

// ── Fixed-point ──────────────────────────────────────────────────────

namespace detail {

// Smallest unsigned word that holds the requested bit count, rounded up to
// 8/16/32. Used to pick storage and a wider type for multiply intermediates.
constexpr unsigned round_bits(unsigned bits) {
    return bits <= 8 ? 8 : (bits <= 16 ? 16 : 32);
}

template <unsigned Bits> struct uint_storage;
template <> struct uint_storage<8>  { using type = u8;  };
template <> struct uint_storage<16> { using type = u16; };
template <> struct uint_storage<32> { using type = u32; };

} // namespace detail

// Unsigned fixed-point number with I integer bits and F fraction bits.
// Storage is the smallest word covering I+F bits (u16 for Fixed<8,8>).
// Multiply promotes to a doubly-wide intermediate to avoid overflow.
template <u8 I, u8 F>
class Fixed {
public:
    using raw_t  = typename detail::uint_storage<detail::round_bits(I + F)>::type;
    using wide_t = typename detail::uint_storage<detail::round_bits((I + F) * 2)>::type;

    static_assert(I + F >= 1, "Fixed needs at least one bit");

    constexpr Fixed() : raw_(0) {}

    // Build from a whole-number integer value (shifted into the integer field).
    static constexpr Fixed from_int(raw_t value) {
        Fixed f;
        f.raw_ = static_cast<raw_t>(value << F);
        return f;
    }

    // Build directly from the underlying fixed-point bit pattern.
    static constexpr Fixed from_raw(raw_t raw) {
        Fixed f;
        f.raw_ = raw;
        return f;
    }

    constexpr raw_t raw() const { return raw_; }

    // Integer part (high I bits) and fractional part (low F bits).
    constexpr raw_t integer()  const { return static_cast<raw_t>(raw_ >> F); }
    constexpr raw_t fraction() const {
        return static_cast<raw_t>(raw_ & ((static_cast<raw_t>(1) << F) - 1));
    }

    constexpr Fixed operator+(Fixed o) const {
        return from_raw(static_cast<raw_t>(raw_ + o.raw_));
    }
    constexpr Fixed operator-(Fixed o) const {
        return from_raw(static_cast<raw_t>(raw_ - o.raw_));
    }
    constexpr Fixed operator*(Fixed o) const {
        // Promote to wide_t so the F+F fractional bits don't overflow before
        // we shift the result back down by F.
        const wide_t product = static_cast<wide_t>(raw_) * static_cast<wide_t>(o.raw_);
        return from_raw(static_cast<raw_t>(product >> F));
    }

    constexpr bool operator==(Fixed o) const { return raw_ == o.raw_; }
    constexpr bool operator!=(Fixed o) const { return raw_ != o.raw_; }

private:
    raw_t raw_;
};

using fixed88 = Fixed<8, 8>;

// ── Random number generator ──────────────────────────────────────────
//
// 8-bit Fibonacci LFSR, taps at 8,6,5,4 (Xilinx XAPP052) → maximal length:
// cycles through all 255 nonzero states before repeating, never returns 0.
// State lives in a static (not ZP-placed yet — a later concern).

inline u8 random() {
    static u8 lfsr = 0xACu;   // arbitrary nonzero seed
    const u8 bit = static_cast<u8>(
        ((lfsr >> 7) ^ (lfsr >> 5) ^ (lfsr >> 4) ^ (lfsr >> 3)) & 1u);
    lfsr = static_cast<u8>((lfsr << 1) | bit);
    return lfsr;
}

} // namespace engine

#endif // ENGINE_MATH_H
