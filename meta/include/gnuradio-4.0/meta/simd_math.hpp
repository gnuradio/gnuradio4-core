#ifndef GNURADIO_META_SIMD_MATH_HPP
#define GNURADIO_META_SIMD_MATH_HPP

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include <gnuradio-4.0/meta/simd.hpp>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include <vir/simd_bit.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Elementary functions that take a sample or a simd vector: exp, sinh, cosh, tanh, log, log2,
// log10, sin, cos, tan, atan, atan2, hypot and pow. A sample goes to the C library. A simd vector
// of float or double lanes is computed in its registers by the algorithms of simd_elementary.hpp,
// where libstdc++'s simd calls the C library once per lane for all of these but sin, cos and hypot,
// and vir-simd's fallback calls it per lane for each and has no simd exp. Float lanes compute in
// float, every function within 2 ulp of the exact result; with FloatLanes::Double as the template
// argument they compute in double and round once, within 1 ulp at about twice the cost. For double
// lanes exp, log, log2, log10, sin, cos and atan are within 1 ulp, tan within 1.1, and sinh, cosh,
// tanh and atan2 within 2; hypot and pow on double lanes are the simd library's own forms. Special
// values are C's, except that float pow in double lanes may round a result of exactly half the
// smallest subnormal up to it where C gives 0. The functions computed here raise no floating-point
// exception the C library does not raise for the argument (inexact aside), under a compiler that
// keeps exceptions where the code puts them (GCC's default, Clang with
// -ffp-exception-behavior=maytrap), except that float pow in double lanes may raise underflow for
// an exact subnormal result, and under GCC sin, cos, tan and atan may raise underflow for a vector
// whose lanes all hold one argument of magnitude below 2^-30; double hypot and pow raise what the
// simd library's forms raise. The vector forms assume the default rounding mode, round to nearest.
// Both arguments of the two-argument functions have one type.

namespace gr::meta {

// Where the float lanes of a simd vector compute: in float, within 2 ulp; or in double,
// within 1 ulp at about twice the cost.
enum class FloatLanes { Float, Double };

namespace detail::elementary_impl {

#define SIMD_ELEMENTARY_INLINE inline
#include <gnuradio-4.0/meta/simd_elementary.hpp>
#undef SIMD_ELEMENTARY_INLINE

// The operations simd_elementary.hpp's functions are written against, on a simd of float or double
// lanes, in the simd interface alone, so that they serve libstdc++'s simd and vir-simd's fallback
// alike.
template<typename TSimd>
struct SimdOps {
    using V    = TSimd;
    using T    = typename V::value_type;
    using M    = typename V::mask_type;
    using UInt = std::conditional_t<std::same_as<T, double>, std::uint64_t, std::uint32_t>;
    using U    = stdx::rebind_simd_t<UInt, V>;

    static constexpr int  kMantissaBits = std::numeric_limits<T>::digits - 1;       // 52, 23
    static constexpr int  kBias         = std::numeric_limits<T>::max_exponent - 1; // 1023, 127
    static constexpr UInt kSign         = UInt{1} << (8 * sizeof(T) - 1);
    static constexpr T    kTwoPowM      = static_cast<T>(UInt{1} << kMantissaBits); // 2^52, 2^23
    // clear_low leaves 21 significant bits of a double, 12 of a float
    static constexpr UInt kHighMask = ~((UInt{1} << (std::same_as<T, double> ? 32 : 12)) - 1);
    // fmadd's a * b + c may or may not be contracted, so the kernel takes its forms that are exact
    // unfused
    static constexpr bool fused = false;

    static V    set(double x) noexcept { return V(static_cast<T>(x)); }
    static V    add(V a, V b) noexcept { return a + b; }
    static V    sub(V a, V b) noexcept { return a - b; }
    static V    mul(V a, V b) noexcept { return a * b; }
    static V    div(V a, V b) noexcept { return a / b; }
    static V    min(V a, V b) noexcept { return stdx::min(a, b); }
    static V    max(V a, V b) noexcept { return stdx::max(a, b); }
    static V    sqrt(V a) noexcept { return stdx::sqrt(a); }
    static V    fmadd(V a, V b, V c) noexcept { return a * b + c; }
    static V    fnmadd(V a, V b, V c) noexcept { return c - a * b; }
    static M    lt(V a, V b) noexcept { return a < b; }
    static M    le(V a, V b) noexcept { return a <= b; }
    static M    eq(V a, V b) noexcept { return a == b; }
    static M    isnan(V a) noexcept { return a != a; }
    static M    mand(M a, M b) noexcept { return a && b; }
    static M    mor(M a, M b) noexcept { return a || b; }
    static M    mandnot(M a, M b) noexcept { return a && !b; }
    static bool any(M m) noexcept { return stdx::any_of(m); }

    // for 0 < lo <= hi the bits of a positive value order as the value does, and a negative value
    // or a NaN lies above hi's; an integer comparison raises no flag
    static bool all_within(V a, double lo, double hi) noexcept {
        const U bits = bitsOf(a);
        return stdx::all_of(bits >= bitsOf(V(static_cast<T>(lo))) && bits <= bitsOf(V(static_cast<T>(hi))));
    }

    static V select(M m, V a, V b) noexcept {
        where(m, b) = a;
        return b;
    }

    static U bitsOf(V x) noexcept { return vir::simd_bit_cast<U>(x); }
    static V fromBits(U u) noexcept { return vir::simd_bit_cast<V>(u); }

    static V abs(V x) noexcept { return fromBits(bitsOf(x) & U(static_cast<UInt>(~kSign))); }
    static V copysign(V x, V y) noexcept { return fromBits((bitsOf(x) & U(static_cast<UInt>(~kSign))) | (bitsOf(y) & U(kSign))); }

    static V flip(V x, M m) noexcept {
        where(m, x) = -x;
        return x;
    }

    static M signbit(V x) noexcept { return stdx::signbit(x); }

    // 1.5 * 2^52 (1.5 * 2^23 for float) added and taken away rounds |x| < 2^51 (2^22) to the
    // nearest integer value (in the default rounding mode)
    static V round(V x) noexcept {
        const V magic(T(1.5) * kTwoPowM);
        return (x + magic) - magic;
    }

    // the kernel calls it on double lanes only, with |x| < 2^31, within round's range
    static V round_cvt(V x) noexcept
    requires std::same_as<T, double>
    {
        return round(x);
    }

    // 2^52 (2^23) added to a smaller |x| and taken away rounds it to an integer value; from there
    // on every value of T is one
    static M is_int(V x) noexcept {
        const V magic(kTwoPowM);
        const V a = abs(x);
        return a >= magic || (a + magic) - magic == a;
    }

    // with s = 2^-(n+1), k s - floor(k s) >= 1/2; floor(k s) is round(k s - 1/2 + s/2), which meets
    // no tie for an integer value k
    template<int N>
    static M kbit(V k) noexcept {
        constexpr T s = T(1) / static_cast<T>(1ULL << (N + 1));
        const V     q = k * V(s);
        return q - round(q - V(T(0.5) - s / 2)) >= V(T(0.5));
    }

    // k plus the bias in the low bits of 1.5 * 2^52 (2^23), shifted into the exponent field
    static V pow2(V k) noexcept { return fromBits(bitsOf(k + V(T(1.5) * kTwoPowM + T(kBias))) << kMantissaBits); }

    // the biased exponent in the low bits of 2^52 (2^23), less that power and the bias
    static V exponent(V x) noexcept { return fromBits((bitsOf(x) >> kMantissaBits) | bitsOf(V(kTwoPowM))) - V(kTwoPowM + T(kBias)); }

    static V mantissa(V x) noexcept { return fromBits((bitsOf(x) & U((UInt{1} << kMantissaBits) - 1)) | bitsOf(V(T(1)))); }
    static V clear_low(V x) noexcept { return fromBits(bitsOf(x) & U(kHighMask)); }

    template<typename F>
    static V fallback(V x, V r, M m) noexcept {
        if (stdx::any_of(m)) {
            for (std::size_t i = 0UZ; i < V::size(); ++i) {
                if (m[i]) {
                    r[i] = F::template s<T>(x[i]);
                }
            }
        }
        return r;
    }
};

// K::f<E> on x: E at double accuracy for double lanes; for float lanes, at float accuracy in float,
// or, where L asks, on the lanes widened to double, narrowed once.
template<typename K, FloatLanes L, typename V, typename... TMore>
[[nodiscard]] V inRegisters(const V& x, const TMore&... more) noexcept {
    if constexpr (std::same_as<typename V::value_type, double>) {
        return K::template f<elementary<SimdOps<V>, double>>(x, more...);
    } else if constexpr (L == FloatLanes::Float) {
        return K::template f<elementary<SimdOps<V>, float>>(x, more...);
    } else {
        using D = stdx::rebind_simd_t<double, V>;
        return stdx::static_simd_cast<V>(K::template f<elementary<SimdOps<D>, float>>(stdx::static_simd_cast<D>(x), stdx::static_simd_cast<D>(more)...));
    }
}

template<typename V>
concept FloatingSimd = any_simd<V> && (std::same_as<typename V::value_type, float> || std::same_as<typename V::value_type, double>);

template<typename V>
concept DoubleSimd = FloatingSimd<V> && std::same_as<typename V::value_type, double>;

// K::f on a simd of float or double lanes, K::s on anything else
template<typename K, FloatLanes L, typename V, typename... TMore>
[[nodiscard]] V call(const V& x, const TMore&... more) noexcept {
    if constexpr (FloatingSimd<V>) {
        return inRegisters<K, L>(x, more...);
    } else {
        return K::s(x, more...);
    }
}

struct Exp {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::exp;
        return exp(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::exp(a);
    }
};

struct Sinh {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::sinh;
        return sinh(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::sinh(a);
    }
};

struct Cosh {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::cosh;
        return cosh(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::cosh(a);
    }
};

struct Tanh {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::tanh;
        return tanh(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::tanh(a);
    }
};

struct Log {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::log;
        return log(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::log(a);
    }
};

struct Log2 {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::log2;
        return log2(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::log2(a);
    }
};

struct Log10 {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::log10;
        return log10(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::log10(a);
    }
};

struct Sin {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::sin;
        return sin(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::template sin<Sin>(a);
    }
};

struct Cos {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::cos;
        return cos(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::template cos<Cos>(a);
    }
};

struct Tan {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::tan;
        return tan(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::template tan<Tan>(a);
    }
};

struct Atan {
    template<typename T>
    static T s(const T& a) noexcept {
        using std::atan;
        return atan(a);
    }
    template<typename E>
    static typename E::V f(typename E::V a) noexcept {
        return E::atan(a);
    }
};

struct Atan2 {
    template<typename T>
    static T s(const T& a, const T& b) noexcept {
        using std::atan2;
        return atan2(a, b);
    }
    template<typename E>
    static typename E::V f(typename E::V a, typename E::V b) noexcept {
        return E::atan2(a, b);
    }
};

struct Hypot {
    template<typename T>
    static T s(const T& a, const T& b) noexcept {
        using std::hypot;
        return hypot(a, b);
    }
    template<typename E>
    static typename E::V f(typename E::V a, typename E::V b) noexcept {
        return E::hypot(a, b);
    }
};

struct Pow {
    template<typename T>
    static T s(const T& a, const T& b) noexcept {
        using std::pow;
        return pow(a, b);
    }
    template<typename E>
    static typename E::V f(typename E::V a, typename E::V b) noexcept {
        return E::pow(a, b);
    }
};

} // namespace detail::elementary_impl

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V exp(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Exp, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V sinh(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Sinh, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V cosh(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Cosh, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V tanh(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Tanh, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V log(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Log, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V log2(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Log2, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V log10(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Log10, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V sin(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Sin, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V cos(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Cos, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V tan(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Tan, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V atan(const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Atan, L>(x);
}

template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V atan2(const V& y, const V& x) noexcept {
    return detail::elementary_impl::call<detail::elementary_impl::Atan2, L>(y, x);
}

namespace detail::elementary_impl {
// K on float lanes as call does; on double lanes the simd library's own form
template<typename K, FloatLanes L, typename V>
[[nodiscard]] V callFloatLanes(const V& x, const V& y) noexcept {
    if constexpr (DoubleSimd<V>) {
        return K::s(x, y);
    } else {
        return call<K, L>(x, y);
    }
}
} // namespace detail::elementary_impl

// hypot on double lanes is the simd library's own: libstdc++ computes it in registers, vir-simd's
// fallback calls the C library's hypot lane by lane
template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V hypot(const V& x, const V& y) noexcept {
    return detail::elementary_impl::callFloatLanes<detail::elementary_impl::Hypot, L>(x, y);
}

// pow on double lanes needs log beyond double and is the simd library's own
template<FloatLanes L = FloatLanes::Float, typename V>
requires(!std::integral<V>)
[[nodiscard]] V pow(const V& x, const V& y) noexcept {
    return detail::elementary_impl::callFloatLanes<detail::elementary_impl::Pow, L>(x, y);
}

} // namespace gr::meta

#endif // GNURADIO_META_SIMD_MATH_HPP
