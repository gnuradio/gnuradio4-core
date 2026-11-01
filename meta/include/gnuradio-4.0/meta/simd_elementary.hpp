/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Jeff Long <willcode4@gmail.com> */

/* Elementary functions on vectors of float or double lanes: exp, sinh, cosh, tanh, log, log2,
 * log10, sin, cos, tan, atan and atan2 at double accuracy or at float accuracy, and hypot and pow
 * at float accuracy. A float function runs in one of two forms: in float lanes, with the
 * reductions' constants split for float and shorter polynomials and reductions that spend float's
 * 2-ulp bound; or in double lanes, from the float arguments widened by the caller, rounded once to
 * float at the end, which keeps every function within 1 ulp of the exact result at about twice
 * the cost.
 *
 * The file is written against a set of operations on one vector type, so that the same algorithms
 * serve x86 intrinsics, std::experimental::simd, or anything else that can supply them. It has no
 * include guard and no namespace of its own: include it inside a namespace of yours, after
 * <cstddef>, <limits>, <type_traits> and <utility>, with SIMD_ELEMENTARY_INLINE defined to the
 * inline specifier the functions should carry. Two libraries that both carry a copy then keep
 * their copies apart, whatever their versions.
 *
 * elementary<O, Acc> holds the functions, for the operations O and the accuracy Acc: double or
 * float on double lanes, float on float lanes. O supplies:
 *
 *   T                            the lane type, float or double
 *   V, M                         the vector of T lanes and its mask
 *   set(double)                  every lane the value, rounded to T
 *   add sub mul div min max      lane by lane
 *   sqrt(x)
 *   fmadd(a, b, c), fnmadd(a, b, c)    a b + c and c - a b, fused where the target has FMA
 *   fused                        constexpr bool: fmadd and fnmadd round once
 *   lt le eq (V, V) -> M; isnan(V) -> M
 *   mand mor (M, M) -> M; mandnot(a, b) -> a and not b; any(M) -> bool
 *   all_within(x, lo, hi) -> bool    for float lanes: every lane in [lo, hi], 0 < lo <= hi, false
 *                                for a NaN, with no flag raised
 *   select(m, a, b)              a where m, b elsewhere
 *   abs(x), copysign(x, y), flip(x, m)  |x|; the magnitude of x with the sign of y; x negated in m
 *   signbit(x) -> M              the sign bit set, -0 and negative NaNs included
 *   round(x)                     the nearest integer value, a half either way, |x| < 2^22 (float),
 *                                2^51 (double)
 *   round_cvt(x)                 double lanes only, |x| < 2^31: an integer value within one of x,
 *                                by another route, for a caller whose additions are its bottleneck
 *   is_int(x) -> M               x an integer value
 *   kbit<n>(k) -> M              bit n of the integer value k, |k| < 2^22, in two's complement
 *   pow2(k)                      2^k for an integer value k of a normal power: [-1022, 1023] for
 *                                double, [-126, 127] for float
 *   exponent(x), mantissa(x)     for a positive normal x: e and m with x = 2^e m, m in [1, 2)
 *   clear_low(x)                 x with the low 32 bits of its significand cleared for double, the
 *                                low 12 for float: 21 and 12 significant bits left
 *   fallback<F>(x, r, m)         F::template s<T>(x) in the lanes of m, r elsewhere
 *
 * Each function reduces its argument to a short interval, evaluates a minimax polynomial there and
 * rebuilds the result; the reductions carry their rounding error as a second term where it would
 * otherwise reach the result. The coefficients come from a Remez fit for the least relative error
 * of the whole function, rounded to double; the float table rounded to float stays within 0.09 ulp
 * of float. The bound each function meets is stated with it, in ulp of the result in the accuracy
 * type, as measured against a reference of higher precision: for double, and for float in float
 * lanes; in double lanes every float function meets 1 ulp.
 *
 * Special values follow C: a NaN argument gives a NaN, the infinities and signed zeros give what C
 * gives, and a result past the range overflows to infinity or underflows through the subnormals to
 * zero, except that float pow in double lanes may round a result of exactly half the smallest
 * subnormal up to it where C gives 0. Under a compiler that keeps floating-point exceptions where
 * the code puts them (GCC by default; Clang with -ffp-exception-behavior=maytrap or strict, since
 * by default it may move a division past the select that discards its lane), no lane raises an
 * exception the C library does not raise for its argument (inexact aside), with two exceptions:
 * float pow in double lanes may raise underflow for an exact subnormal result, and under GCC sin,
 * cos, tan and atan may raise underflow for a vector whose lanes all hold one argument of magnitude
 * below 2^-30. Some that C raises (divide-by-zero for log(0), invalid for log of a negative) are
 * not raised.
 */

#ifndef SIMD_ELEMENTARY_INLINE
#error "define SIMD_ELEMENTARY_INLINE before including simd_elementary.hpp"
#endif

// The minimax coefficients, highest degree first, for double accuracy and for float accuracy.
// The relative error of each rounded polynomial, in ulp of the function: double sin_pi4 0.11,
// cos_pi4 0.008, atan 0.07, expm1 0.11, log 0.013, sinh 0.08, tanh 0.07; float all below 0.002
// but sin_pi4 below 0.0001.
template<typename Acc>
struct elementary_coefficients;

template<>
struct elementary_coefficients<double> {
    // sin r = r + r^3 q(r^2), |r| <= pi/4
    static constexpr double sin_pi4[] = {1.5896338488464894e-10, -2.5050749574800333e-08, 2.7557313632571474e-06, -0.00019841269829620582, 0.008333333333322156, -0.1666666666666663};
    // cos r = 1 - r^2 / 2 + r^4 q(r^2), |r| <= pi/4
    static constexpr double cos_pi4[] = {-1.1358612600578793e-11, 2.087570227702573e-09, -2.755731418959074e-07, 2.480158728888638e-05, -0.001388888888887311, 0.041666666666666595};
    // atan u = u + u^3 q(u^2), |u| <= 7/16
    static constexpr double atan[] = {-0.016275458996732155, 0.03652017151966706, -0.04976337584023659, 0.05833425742322207, -0.06661049245392378, 0.07691873673192567, -0.09090886961794904, 0.11111110398249423, -0.1428571427232845, 0.19999999999874357, -0.3333333333333292};
    // e^r - 1 = r + r^2 q(r), |r| <= ln(2) / 2
    static constexpr double expm1[] = {1.768754469601459e-09, 2.5105038460189987e-08, 2.756789641389999e-07, 2.755725583595051e-06, 2.4801574612705713e-05, 0.0001984126987447036, 0.0013888888895538189, 0.008333333333326238, 0.04166666666665246, 0.1666666666666667, 0.5000000000000001};
    // 2 atanh s = 2 s + s^3 q(s^2), |s| <= (sqrt 2 - 1) / (sqrt 2 + 1)
    static constexpr double log[] = {0.14794434284852379, 0.15314196978553518, 0.18183558810672482, 0.2222219867502825, 0.2857142874144069, 0.39999999999418706, 0.6666666666666734};
    // sinh x = x + x^3 q(x^2), |x| <= 1
    static constexpr double sinh[] = {7.758674685278605e-13, 1.6057262126604528e-10, 2.505212310762102e-08, 2.7557319157246888e-06, 0.00019841269841431347, 0.008333333333333148, 0.16666666666666669};
    // tanh x = x + x^3 q(x^2), |x| <= 3/4
    static constexpr double tanh[] = {-1.4867132594261287e-06, 9.495886784439984e-06, -3.362209842318689e-05, 9.3447150557875e-05, -0.00023762227939624374, 0.0005895602140002541, -0.001455731379430425, 0.003592112140204279, -0.008863233863981516, 0.021869488423520357, -0.05396825396374775, 0.1333333333332423, -0.3333333333333327};
};

template<>
struct elementary_coefficients<float> {
    static constexpr double sin_pi4[] = {2.7181466012819983e-06, -0.00019839314873638223, 0.008333329312882748, -0.1666666664086579};
    static constexpr double cos_pi4[] = {2.4433405711191532e-05, -0.0013887318387195795, 0.041666645725843984};
    static constexpr double atan[]    = {0.04503761774554831, -0.08358624383214909, 0.11023423892800306, -0.14280278617065603, 0.19999843271704743, -0.33333331819044004};
    static constexpr double expm1[]   = {1.9323417940420612e-05, 0.00019899081817824818, 0.0013900448051552923, 0.008333298725608415, 0.04166659738158488, 0.16666666718411327, 0.5000000010356275};
    static constexpr double log[]     = {0.2987004324372032, 0.3997760450422696, 0.6666677586407064};
    static constexpr double sinh[]    = {2.8169103317664072e-06, 0.00019836166570266376, 0.008333350024965115, 0.16666666498006255};
    static constexpr double tanh[]    = {-0.0006331401506313234, 0.002970315008015755, -0.00859708690969414, 0.02180450793697485, -0.053959605528184254, 0.13333278138208934, -0.3333333213257902};
};

// The reductions' constants for each lane type. A constant in two parts is hi + lo, hi with few
// enough significant bits that its product with every integer the reduction meets is exact, lo the
// rest rounded to the type; pio2 is pi/2 the same way in more parts, all but the last short.
template<typename T>
struct elementary_splits;

template<>
struct elementary_splits<double> {
    static constexpr double log2e = 1.4426950408889634;
    // |k| and |e| below 2^11
    static constexpr double ln2_hi = 0.6931471806019545, ln2_lo = -4.2009150726810846e-11;
    // e^x is 0 below exp_min and infinite above exp_max; e^x / 2 above half_exp_max
    static constexpr double exp_min = -746, exp_max = 711, half_exp_max = 711;
    // times the 21 bits of clear_low
    static constexpr double ivln2_hi = 1.4426950407214463, ivln2_lo = 1.6751713164886512e-10;
    static constexpr double ivln10_hi = 0.4342944818781689, ivln10_lo = 2.5082946711645275e-11;
    static constexpr double log10_2_hi = 0.3010299956658855, log10_2_lo = -1.9043128467164274e-12;
    // |k| below 2^26 up to |x| = trig_max
    static constexpr double two_over_pi = 0.6366197723675814, trig_max = 67108864.;
    static constexpr double pio2[] = {1.5707963407039642, -1.3909067675399456e-08, 6.123233932053594e-17, 6.36831716351095e-25};
    // atan(1/2), atan(1), atan(3/2), pi/2 and pi
    static constexpr double atan_half_hi = 0.4636476090008061;
    static constexpr double atan_half_lo = 2.2698777452961687e-17;
    static constexpr double pi4_hi = 0.7853981633974483, pi4_lo = 3.061616997868383e-17;
    static constexpr double atan_3half_hi = 0.982793723247329;
    static constexpr double atan_3half_lo = 1.3903311031230998e-17;
    static constexpr double pi2_hi = 1.5707963267948966, pi2_lo = 6.123233995736766e-17;
    static constexpr double pi_hi = 3.141592653589793, pi_lo = 1.2246467991473532e-16;
    // atan rounds to pi/2 beyond atan_max, where -1/t cubed is still normal
    static constexpr double atan_max = 0x1p60;
};

template<>
struct elementary_splits<float> {
    static constexpr double log2e = 1.4426950216293335;
    // |k| and |e| below 2^8
    static constexpr double ln2_hi = 0.693115234375, ln2_lo = 3.194618329871446e-05;
    static constexpr double exp_min = -104, exp_max = 89, half_exp_max = 90;
    // times the 12 bits of clear_low
    static constexpr double ivln2_hi = 1.44287109375, ivln2_lo = -0.00017605285393074155;
    static constexpr double ivln10_hi = 0.434326171875, ivln10_lo = -3.168997136526741e-05;
    static constexpr double log10_2_hi = 0.301025390625, log10_2_lo = 4.605039066518657e-06;
    // |k| below 2^13 up to |x| = trig_max; four parts of at most 11 bits, so that where x is
    // nearest a multiple of pi/2 the sum's error stays far below r's last bit
    static constexpr double two_over_pi = 0.6366197466850281, trig_max = 8192.;
    static constexpr double pio2[]       = {1.5703125, 0.0004837512969970703, 7.549533620476723e-08, 2.5632829192545614e-12, 6.123234262925839e-17};
    static constexpr double atan_half_hi = 0.46364760398864746;
    static constexpr double atan_half_lo = 5.01215868808913e-09;
    static constexpr double pi4_hi = 0.7853981852531433, pi4_lo = -2.1855694143368964e-08;
    static constexpr double atan_3half_hi = 0.9827937483787537;
    static constexpr double atan_3half_lo = -2.5131424052915463e-08;
    static constexpr double pi2_hi = 1.5707963705062866, pi2_lo = -4.371138828673793e-08;
    static constexpr double pi_hi = 3.1415927410125732, pi_lo = -8.742277657347586e-08;
    // past 2^26 atan rounds to pi/2; the polynomial's products with z^2 stay normal without FMA
    static constexpr double atan_max = 0x1p26;
};

// Float lanes' own polynomials, shorter than the float-accuracy table's, whose extra terms serve
// the double lanes' 1 ulp, or with none there: e^r - 1 = r + r^2 q(r), |r| <= ln(2) / 2;
// tan r = r + r^3 q(r^2), |r| <= pi/4; log(1 + f) = f - f^2 / 2 + f^3 q(f), f in
// [sqrt(1/2) - 1, sqrt 2 - 1]; atan u = u + u^3 q(u^2), |u| <= 1; 2^r - 1 = r q(r), |r| <= 1/2.
// Rounded to float, each stays within 0.05, 0.022, 0.14, 0.07 and 0.025 ulp of float; tan's and
// 2^r's coefficients were rounded one at a time from the lowest degree, the rest refitted. For
// pow's log2, 2 atanh(s) / ln 2 = (2 / ln 2) (s + s^3 / 3 + s^5 / 5 + s^7 / 7) to 2^-45 for
// |s| <= 0.035, the terms past s from their series.
// pi is in four parts, the first three of at most 11 bits, so that k / 2 times each is exact for
// |k| < 2^13.
struct elementary_float_lanes {
    static constexpr double expm1[]      = {0.00019961844373028725, 0.0013943050289526582, 0.008333221077919006, 0.04166632890701294, 0.1666666716337204, 0.5};
    static constexpr double tan_pi4[]    = {0.004372667521238327, 9.82720302999951e-05, 0.010827437974512577, 0.02128620818257332, 0.05405891314148903, 0.13332675397396088, 0.33333349227905273};
    static constexpr double log1p[]      = {-0.0762893408536911, 0.12753218412399292, -0.1315828114748001, 0.1420273780822754, -0.16623638570308685, 0.2000119537115097, -0.25000813603401184, 0.3333333134651184};
    static constexpr double atan_1[]     = {-0.0017966382438316941, 0.01092721801251173, -0.031199652701616287, 0.05797773227095604, -0.08404520153999329, 0.10952512919902802, -0.14264295995235443, 0.19998551905155182, -0.33333298563957214};
    static constexpr double pi[]         = {3.140625, 0.0009675025939941406, 1.5099067240953445e-07, 5.126688136514179e-12};
    static constexpr double exp2[]       = {9.606630555936135e-06, 0.0001540301163913682, 0.0013349381042644382, 0.009618195705115795, 0.05550403520464897, 0.24022650718688965, 0.6931471824645996};
    static constexpr double log2_atanh[] = {0.4121985831111324, 0.5770780163555853, 0.9617966939259756};
    // c = 2^(k / 5) rounded to float for k = -2, -1, 1, 2, and log2 c in two parts, the first of
    // 12 bits, so that an exponent plus it is exact
    static constexpr double pow_c[]     = {0.7578582763671875, 0.8705505728721619, 1.148698329925537, 1.3195079565048218};
    static constexpr double pow_lc_hi[] = {-0.4000244140625, -0.20001220703125, 0.20001220703125, 0.4000244140625};
    static constexpr double pow_lc_lo[] = {2.440094976918772e-05, 1.2222901204950176e-05, -1.2238519957463723e-05, -2.436406066408381e-05};
};

template<typename O, typename Acc>
struct elementary {
    using T = typename O::T;
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float>);
    static_assert(std::is_same_v<Acc, T> || (std::is_same_v<Acc, float> && std::is_same_v<T, double>));

    // float accuracy in double lanes, where the double lanes' spare bits replace the compensated
    // steps
    static constexpr bool widened = !std::is_same_v<Acc, T>;

    // float lanes, which spend the 2-ulp budget on fewer steps: shorter polynomials, reductions
    // without compensation where float's bound allows
    static constexpr bool lean = std::is_same_v<T, float>;
    using L                    = elementary_float_lanes;

    using V = typename O::V;
    using M = typename O::M;
    using C = elementary_coefficients<Acc>;
    using S = elementary_splits<T>;

    SIMD_ELEMENTARY_INLINE static V c(double x) noexcept { return O::set(x); }

    // The polynomial with the coefficients k, highest degree first, at z, by Horner's rule; one
    // expansion of an index pack, as MSVC unrolls no loop.
    template<std::size_t N, std::size_t... I>
    SIMD_ELEMENTARY_INLINE static V horner(V z, const double (&k)[N], std::index_sequence<I...>) noexcept {
        V p = c(k[0]);
        ((p = O::fmadd(p, z, c(k[I + 1]))), ...);
        return p;
    }

    template<std::size_t N>
    SIMD_ELEMENTARY_INLINE static V horner(V z, const double (&k)[N]) noexcept {
        return horner(z, k, std::make_index_sequence<N - 1>());
    }

    // The same polynomial as two chains in z^2, the terms of even and of odd degree, joined by one
    // fused multiply-add: half the dependency chain of Horner's rule for the longer polynomials.
    // N - 1 - 2 J and N - 2 - 2 J index the coefficients of even and odd degree from the highest.
    template<std::size_t N, std::size_t... J, std::size_t... L>
    SIMD_ELEMENTARY_INLINE static V horner2(V z, const double (&k)[N], std::index_sequence<J...>, std::index_sequence<L...>) noexcept {
        // float lanes without FMA: |z| max(|z|, 2^-45), z^2 from 2^-45 on and 0 at 0; between, its
        // terms fall far below the last bit of each chain's constant, so that the sum is the same,
        // and their products, rounded on their own there, stay normal
        V z2 = O::mul(z, z);
        if constexpr (lean && !O::fused) {
            const V za = O::abs(z);
            z2         = O::mul(za, O::max(za, c(0x1p-45)));
        }
        V even = c(k[(N - 1) % 2]);
        ((even = O::fmadd(even, z2, c(k[(N - 1) % 2 + 2 * (J + 1)]))), ...);
        V odd = c(k[N % 2]);
        ((odd = O::fmadd(odd, z2, c(k[N % 2 + 2 * (L + 1)]))), ...);
        return O::fmadd(odd, z, even);
    }

    template<std::size_t N>
    SIMD_ELEMENTARY_INLINE static V poly(V z, const double (&k)[N]) noexcept {
        // float lanes split from three coefficients on, for the shorter dependency chain; their
        // bound leaves room for the extra rounding
        if constexpr (N < 6 && !(lean && N >= 3)) {
            return horner(z, k);
        } else {
            return horner2(z, k, std::make_index_sequence<(N + 1) / 2 - 1>(), std::make_index_sequence<N / 2 - 1>());
        }
    }

    SIMD_ELEMENTARY_INLINE static V infinity() noexcept { return c(std::numeric_limits<double>::infinity()); }

    SIMD_ELEMENTARY_INLINE static V quiet_nan() noexcept { return c(std::numeric_limits<double>::quiet_NaN()); }

    // the smallest normal value of the accuracy type
    static constexpr double min_normal() noexcept { return static_cast<double>(std::numeric_limits<Acc>::min()); }

    // exp, sinh, cosh, tanh. x = k ln 2 + r, |r| <= ln(2) / 2 up to rounding, with k = round(x /
    // ln 2) and ln 2 in two parts, the first short enough that k times it is exact; e^r - 1 from
    // the polynomial, and e^x = 2^k + 2^k (e^r - 1), rounded once. 2^k is two factors, each a
    // normal power of two, so that k may reach past the exponent range and the second product
    // rounds again only for a subnormal result.

    // e^r - 1 in float lanes
    SIMD_ELEMENTARY_INLINE static V expm1_lean(V r) noexcept { return O::fmadd(O::mul(r, r), horner(r, L::expm1), r); }

    // e^x in float lanes for |x| <= 87, where the result and 2^k are normal: 2^k as one factor;
    // e^x / 2 the same way with 2^(k - 1)
    SIMD_ELEMENTARY_INLINE static V exp_normal(V x) noexcept {
        V k, r;
        exp_reduce(x, k, r);
        const V s = O::pow2(k);
        return O::fmadd(s, expm1_lean(r), s);
    }

    SIMD_ELEMENTARY_INLINE static V half_exp_normal(V x) noexcept {
        V k, r;
        exp_reduce(x, k, r);
        const V s = O::pow2(O::add(k, c(-1)));
        return O::fmadd(s, expm1_lean(r), s);
    }

    SIMD_ELEMENTARY_INLINE static void exp_reduce(V x, V& k, V& r) noexcept {
        k = O::round(O::mul(x, c(S::log2e)));
        r = O::fnmadd(k, c(S::ln2_hi), x);
        r = O::fnmadd(k, c(S::ln2_lo), r);
    }

    // e^x for x in [exp_min, half_exp_max], where the result is already zero or infinite beyond;
    // the reduction keeps k within twice the exponent range.
    SIMD_ELEMENTARY_INLINE static V exp_clamped(V x, V bias) noexcept {
        const V k = O::round(O::mul(x, c(S::log2e)));
        V       r = O::fnmadd(k, c(S::ln2_hi), x);
        r         = O::fnmadd(k, c(S::ln2_lo), r);
        if constexpr (lean) {
            // float lanes: the shorter polynomial, and 2^k as 2^k1, k1 clamped to the normal range,
            // times the normal power of two left over
            const V em = expm1_lean(r);
            const V kb = O::add(k, bias);
            const V k1 = O::min(O::max(kb, c(-126)), c(127));
            const V s1 = O::pow2(k1);
            return O::mul(O::fmadd(s1, em, s1), O::pow2(O::sub(kb, k1)));
        } else {
            // Horner's rule here: exp's budget below 1 ulp leaves no room for the two chains' extra
            // rounding
            const V em = O::fmadd(O::mul(r, r), horner(r, C::expm1), r);
            const V kb = O::add(k, bias);
            // any integer near kb / 2 gives the same result; round_cvt keeps MSVC's double exp
            // fast, and is instantiated for double lanes only
            const V k1 = O::round_cvt(O::mul(kb, c(0.5)));
            const V s1 = O::pow2(k1);
            return O::mul(O::fmadd(s1, em, s1), O::pow2(O::sub(kb, k1)));
        }
    }

    // The forms below, named with a trailing underscore, take no NaN: the functions at the end
    // replace a NaN argument before calling them. Every lane is computed and a select keeps some:
    // a lane whose argument is infinite or tiny goes in as 0 wherever it would raise a flag the C
    // library does not (overflow, underflow).
    SIMD_ELEMENTARY_INLINE static M is_inf(V x) noexcept { return O::eq(O::abs(x), infinity()); }

    // x, or 0 where it is infinite; below 2^-60, where e^x rounds to 1, 2^-60 with x's sign but
    // 0 itself, which leaves the result and raises the inexact C raises for a tiny x
    SIMD_ELEMENTARY_INLINE static V exp_arg(V x) noexcept {
        const V a    = O::abs(x);
        const V tiny = O::select(O::eq(a, c(0)), x, O::copysign(c(0x1p-60), x));
        return O::select(is_inf(x), c(0), O::select(O::lt(a, c(0x1p-60)), tiny, x));
    }

    // exp: at most 1 ulp (double), 2 ulp (float).
    SIMD_ELEMENTARY_INLINE static V exp_(V x) noexcept {
        const V e = exp_clamped(O::min(O::max(exp_arg(x), c(S::exp_min)), c(S::exp_max)), c(0));
        return O::select(O::eq(x, infinity()), x, O::select(O::eq(x, O::mul(c(-1), infinity())), c(0), e));
    }

    // The odd polynomial of sinh or tanh on |x| up to its bound: its argument limited to the
    // bound, and below 2^-28, where the function rounds to x, replaced by 0; the caller selects.
    template<std::size_t N>
    SIMD_ELEMENTARY_INLINE static V odd_poly(V x, V a, double bound, const double (&k)[N]) noexcept {
        const V xs = O::copysign(O::select(O::lt(a, c(0x1p-28)), c(0), O::min(a, c(bound))), x);
        const V z  = O::mul(xs, xs);
        return O::fmadd(O::mul(xs, z), poly(z, k), xs);
    }

    // sinh: at most 2 ulp. The polynomial up to |x| = 1; above it, h - 1 / (4 h) with h = e^|x| /
    // 2, the second term falling below h's last bit as |x| grows. Here and in cosh the division
    // takes h no larger than 2^60, which leaves it as it is below |x| = 20 and keeps the discarded
    // lanes' quotient normal without a select, which Clang would move the division past.
    SIMD_ELEMENTARY_INLINE static V sinh_(V x) noexcept {
        const V a     = O::abs(x);
        const V small = O::select(O::lt(a, c(0x1p-28)), x, odd_poly(x, exp_arg(a), 1, C::sinh));
        const M mid   = O::lt(a, c(20));
        const V h     = exp_clamped(O::min(exp_arg(a), c(S::half_exp_max)), c(-1));
        const V q     = O::div(c(0.25), O::min(h, c(0x1p60)));
        const V large = O::copysign(O::select(mid, O::sub(h, q), h), x);
        const V s     = O::select(O::lt(a, c(1)), small, large);
        return O::select(is_inf(x), x, s);
    }

    // cosh: at most 2 ulp. h + 1 / (4 h) with h = e^|x| / 2, h alone from |x| = 20 on.
    SIMD_ELEMENTARY_INLINE static V cosh_(V x) noexcept {
        const V a   = O::abs(x);
        const M mid = O::lt(a, c(20));
        const V h   = exp_clamped(O::min(exp_arg(a), c(S::half_exp_max)), c(-1));
        const V r   = O::select(mid, O::add(h, O::div(c(0.25), O::min(h, c(0x1p60)))), h);
        return O::select(is_inf(x), a, r);
    }

    // tanh: at most 2 ulp. The polynomial up to |x| = 3/4; above it, 1 - 2 / (e^(2|x|) + 1),
    // which rounds to 1 from |x| = 19 on, so |x| is limited to 20.
    SIMD_ELEMENTARY_INLINE static V tanh_(V x) noexcept {
        const V a     = O::abs(x);
        const V small = O::select(O::lt(a, c(0x1p-28)), x, odd_poly(x, a, 0.75, C::tanh));
        // below 2^-60 as in exp_arg
        const V m     = O::select(O::lt(a, c(0x1p-60)), O::select(O::eq(a, c(0)), a, c(0x1p-60)), O::min(a, c(20.)));
        const V a2    = O::add(m, m);
        const V e2    = exp_clamped(a2, c(0));
        const V large = O::copysign(O::sub(c(1), O::div(c(2), O::add(e2, c(1)))), x);
        return O::select(O::lt(a, c(0.75)), small, large);
    }

    // log, log2, log10. x = 2^e m with m in [sqrt(1/2), sqrt 2), f = m - 1 exactly; log(1 + f) =
    // f - hfsq + s (hfsq + R) with s = f / (2 + f), hfsq = f^2 / 2 and 2 s + s R = 2 atanh s, so
    // that the rounding error of s reaches the result only through a term of order f^3. A
    // subnormal x is scaled into the normal range first.

    struct log_parts {
        V e;    // the exponent, an integer value
        V f;    // m - 1
        V hfsq; // f^2 / 2
        V sr;   // s (hfsq + R)
    };

    SIMD_ELEMENTARY_INLINE static log_parts log_reduce(V x) noexcept {
        const V min_normal = c(static_cast<double>(std::numeric_limits<T>::min()));
        const M sub        = O::mand(O::lt(c(0), x), O::lt(x, min_normal));
        const V xs         = O::mul(x, O::select(sub, c(18014398509481984.), c(1))); // 2^54
        return log_reduce_normal(xs, O::select(sub, c(54), c(0)));
    }

    // the parts of a positive normal xs whose exponent is eoff too large
    SIMD_ELEMENTARY_INLINE static log_parts log_reduce_normal(V xs, V eoff) noexcept {
        V       e  = O::sub(O::exponent(xs), eoff);
        V       m  = O::mantissa(xs);
        const M hi = O::lt(c(1.4142135623730951), m);
        m          = O::select(hi, O::mul(m, c(0.5)), m);
        e          = O::select(hi, O::add(e, c(1)), e);
        const V f  = O::sub(m, c(1));
        if constexpr (lean) {
            // float lanes: log(1 + f) - (f - f^2 / 2) as f^3 times a polynomial in f, without the
            // division
            const V ff = O::mul(f, f);
            return {e, f, O::mul(c(0.5), ff), O::mul(O::mul(ff, f), poly(f, L::log1p))};
        }
        const V s    = O::div(f, O::add(f, c(2)));
        const V z    = O::mul(s, s);
        const V hfsq = O::mul(c(0.5), O::mul(f, f));
        const V sr   = O::mul(s, O::fmadd(z, poly(z, C::log), hfsq));
        return {e, f, hfsq, sr};
    }

    // The special values of the logarithms: NaN below zero, -inf at zero, +inf at +inf.
    SIMD_ELEMENTARY_INLINE static V log_special(V x, V r) noexcept {
        r = O::select(O::eq(x, infinity()), x, r);
        r = O::select(O::eq(x, c(0)), c(-std::numeric_limits<double>::infinity()), r);
        return O::select(O::lt(x, c(0)), quiet_nan(), r);
    }

    // log(1 + f) from its parts, for float accuracy
    SIMD_ELEMENTARY_INLINE static V log1p_parts(const log_parts& p) noexcept { return O::add(O::sub(p.f, p.hfsq), p.sr); }

    // log(1 + f) as hi + lo: hi = f - hfsq rounded, f - hi exact as hi is near f, and lo its
    // rounding error with s (hfsq + R)
    SIMD_ELEMENTARY_INLINE static void log1p_hilo(const log_parts& p, V& hi, V& lo) noexcept {
        hi = O::sub(p.f, p.hfsq);
        lo = O::add(O::sub(O::sub(p.f, hi), p.hfsq), p.sr);
    }

    // log x without the special values: e ln 2 + log(1 + f), ln 2 in two parts.
    SIMD_ELEMENTARY_INLINE static V log_value(const log_parts& p) noexcept {
        const V lo = O::sub(O::sub(p.hfsq, O::fmadd(p.e, c(S::ln2_lo), p.sr)), p.f);
        return O::fnmadd(c(1), lo, O::mul(p.e, c(S::ln2_hi)));
    }

    SIMD_ELEMENTARY_INLINE static V log_finite(V x) noexcept { return log_value(log_reduce(x)); }

    // log: at most 1 ulp (double), 2 ulp (float).
    SIMD_ELEMENTARY_INLINE static V log_(V x) noexcept { return log_special(x, log_finite(x)); }

    // f - hfsq + s (hfsq + R) as hi + lo, hi cut by clear_low, so that hi times a constant as
    // short as ivln2_hi is exact.
    SIMD_ELEMENTARY_INLINE static void log_split(const log_parts& p, V& hi, V& lo) noexcept {
        hi = O::clear_low(O::sub(p.f, p.hfsq));
        lo = O::add(O::sub(O::sub(p.f, hi), p.hfsq), p.sr);
    }

    // log2: at most 1 ulp (double), 2 ulp (float). e + (hi + lo) / ln 2, with 1 / ln 2 in two
    // parts and the sum with e kept exact until the last addition.
    SIMD_ELEMENTARY_INLINE static V log2_value(const log_parts& p) noexcept {
        if constexpr (widened) {
            return O::fmadd(log1p_parts(p), c(S::log2e), p.e);
        }
        if constexpr (lean) {
            // float lanes: e + log2(1 + f), the latter from log(1 + f) as hi + lo and 1 / ln 2 in
            // two parts, rounded once before e is added
            V hi, lo;
            log1p_hilo(p, hi, lo);
            return O::add(p.e, O::fmadd(hi, c(1.4426950216293335), O::fmadd(lo, c(1.4426950216293335), O::mul(hi, c(1.925963033500011e-08)))));
        }
        V hi, lo;
        log_split(p, hi, lo);
        const V vhi = O::mul(hi, c(S::ivln2_hi));
        V       vlo = O::fmadd(O::add(lo, hi), c(S::ivln2_lo), O::mul(lo, c(S::ivln2_hi)));
        const V w   = O::add(p.e, vhi);
        vlo         = O::add(vlo, O::add(O::sub(p.e, w), vhi));
        return O::add(vlo, w);
    }

    SIMD_ELEMENTARY_INLINE static V log2_(V x) noexcept { return log_special(x, log2_value(log_reduce(x))); }

    // log10: at most 1 ulp (double), 2 ulp (float). e log10(2) + (hi + lo) / ln 10, each constant
    // in two parts.
    SIMD_ELEMENTARY_INLINE static V log10_value(const log_parts& p) noexcept {
        if constexpr (widened) {
            return O::fmadd(p.e, c(0.3010299956639812), O::mul(log1p_parts(p), c(0.4342944819032518)));
        }
        if constexpr (lean) {
            // float lanes: e log10(2) + log10(1 + f), the same way, e log10(2) in two parts with the
            // first's product exact
            V hi, lo;
            log1p_hilo(p, hi, lo);
            const V t = O::fmadd(hi, c(0.4342944920063019), O::fmadd(lo, c(0.4342944920063019), O::mul(hi, c(-1.0103049952192578e-08))));
            return O::fmadd(p.e, c(S::log10_2_hi), O::fmadd(p.e, c(S::log10_2_lo), t));
        }
        V hi, lo;
        log_split(p, hi, lo);
        const V vhi = O::mul(hi, c(S::ivln10_hi));
        const V y2  = O::mul(p.e, c(S::log10_2_hi));
        V       vlo = O::fmadd(p.e, c(S::log10_2_lo), O::fmadd(O::add(lo, hi), c(S::ivln10_lo), O::mul(lo, c(S::ivln10_hi))));
        const V w   = O::add(y2, vhi);
        vlo         = O::add(vlo, O::add(O::sub(y2, w), vhi));
        return O::add(vlo, w);
    }

    SIMD_ELEMENTARY_INLINE static V log10_(V x) noexcept { return log_special(x, log10_value(log_reduce(x))); }

    // sin, cos, tan. x = k pi/2 + r with k = round(2 x / pi), pi/2 in the parts of pio2, all but
    // the last short enough that k times each is exact up to |x| = trig_max (2^26 for double,
    // 8192 for float); r is kept as r + rlo. Lanes beyond that and the infinities go to F's scalar
    // form through O::fallback.

    struct trig_parts {
        V k;   // round(2 x / pi)
        V r;   // the reduced argument, |r| <= pi/4 up to rounding
        V rlo; // its rounding error
    };

    // a + b as hi + lo, exactly (two-sum)
    SIMD_ELEMENTARY_INLINE static void two_sum(V a, V b, V& hi, V& lo) noexcept {
        hi         = O::add(a, b);
        const V bb = O::sub(hi, a);
        lo         = O::add(O::sub(a, O::sub(hi, bb)), O::sub(b, bb));
    }

    // h + b as the new h, its rounding error added to l
    SIMD_ELEMENTARY_INLINE static void two_sum_into(V& h, V& l, V b) noexcept {
        V hn, ln;
        two_sum(h, b, hn, ln);
        h = hn;
        l = O::add(l, ln);
    }

    // The parts are taken away in order, each product exact and each difference a two-sum, so that
    // where x is near a multiple of pi/2 and the leading parts cancel, the rounding of the last
    // part happens at the scale of r. I indexes the middle parts after the second.
    template<std::size_t... I>
    SIMD_ELEMENTARY_INLINE static trig_parts trig_reduce(V x, V k, std::index_sequence<I...>) noexcept {
        constexpr std::size_t N  = sizeof(S::pio2) / sizeof(S::pio2[0]);
        const V               nk = O::mul(k, c(-1));
        V                     h, l;
        two_sum(O::fnmadd(k, c(S::pio2[0]), x), O::mul(nk, c(S::pio2[1])), h, l); // first exact
        (two_sum_into(h, l, O::mul(nk, c(S::pio2[I + 2]))), ...);
        const V lo = O::fnmadd(k, c(S::pio2[N - 1]), l);
        const V r  = O::add(h, lo);
        return {k, r, O::sub(lo, O::sub(r, h))};
    }

    SIMD_ELEMENTARY_INLINE static trig_parts trig_reduce(V x) noexcept {
        const V k = O::round(O::mul(x, c(S::two_over_pi)));
        if constexpr (widened) {
            // float accuracy: each difference is exact where the parts cancel and rounds at the
            // scale of r where they do not, which double leaves 29 bits below float
            V r = O::fnmadd(k, c(S::pio2[0]), x);
            r   = O::fnmadd(k, c(S::pio2[1]), r);
            r   = O::fnmadd(k, c(S::pio2[2]), r);
            return {k, O::fnmadd(k, c(S::pio2[3]), r), c(0)};
        } else {
            return trig_reduce(x, k, std::make_index_sequence<sizeof(S::pio2) / sizeof(S::pio2[0]) - 3>());
        }
    }

    // sin(r + rlo) = r + r^3 q(r^2) + rlo (1 - r^2 / 2)
    SIMD_ELEMENTARY_INLINE static V sin_poly(V r, V rlo, V z) noexcept {
        if constexpr (widened) {
            return O::fmadd(O::mul(r, z), poly(z, C::sin_pi4), r);
        }
        const V tail = O::fnmadd(O::mul(c(0.5), z), rlo, rlo);
        return O::add(r, O::fmadd(O::mul(r, z), poly(z, C::sin_pi4), tail));
    }

    // cos(r + rlo) = w + ((1 - w) - z/2) + z^2 q(z) - r rlo, w = 1 - z/2 rounded
    SIMD_ELEMENTARY_INLINE static V cos_poly(V r, V rlo, V z) noexcept {
        if constexpr (widened) {
            return O::fmadd(O::mul(z, z), poly(z, C::cos_pi4), O::fnmadd(c(0.5), z, c(1)));
        }
        const V hz   = O::mul(c(0.5), z);
        const V w    = O::sub(c(1), hz);
        const V tail = O::fmadd(O::mul(z, z), poly(z, C::cos_pi4), O::mul(O::mul(c(-1), r), rlo));
        return O::add(w, O::add(O::sub(O::sub(c(1), w), hz), tail));
    }

    // sin(r + rlo) and cos(r + rlo) as sh + sl and ch + cl: the last sums of sin_poly and cos_poly
    // taken exactly, as r and w are the larger addends
    SIMD_ELEMENTARY_INLINE static void sincos_parts(V r, V rlo, V z, V& sh, V& sl, V& ch, V& cl) noexcept {
        const V tail = O::fnmadd(O::mul(c(0.5), z), rlo, rlo);
        const V u    = O::fmadd(O::mul(r, z), poly(z, C::sin_pi4), tail);
        sh           = O::add(r, u);
        sl           = O::add(O::sub(r, sh), u);
        const V hz   = O::mul(c(0.5), z);
        const V w    = O::sub(c(1), hz);
        const V v    = O::add(O::sub(O::sub(c(1), w), hz), O::fmadd(O::mul(z, z), poly(z, C::cos_pi4), O::mul(O::mul(c(-1), r), rlo)));
        ch           = O::add(w, v);
        cl           = O::add(O::sub(w, ch), v);
    }

    // sin, cos and tan in float lanes: x = h pi + r with h = k / 2, k = round(2 x / pi), and
    // |r| <= pi/4 up to rounding; h times each part is k times half the part, both exact. The
    // first two differences are exact up to |x| = trig_max, and so is the third where it falls
    // below 2^-10, which leaves one rounding for the fourth; above 2^-10 the last two parts are
    // taken away as one, whose rounding is below a fifth of r's last bit there. rlo is the
    // rounding of the last difference: |s| exceeds the part taken from it above 2^-10.
    SIMD_ELEMENTARY_INLINE static trig_parts reduce_lean_parts(V x, V k) noexcept {
        V s          = O::fnmadd(k, c(L::pi[0] / 2), x);
        s            = O::fnmadd(k, c(L::pi[1] / 2), s);
        const V r1   = O::fnmadd(k, c(L::pi[2] / 2), s);
        const V near = O::fnmadd(k, c(L::pi[3] / 2), r1);
        const V t    = O::fmadd(k, c(L::pi[3] / 2), O::mul(k, c(L::pi[2] / 2)));
        const V far  = O::sub(s, t);
        const M m    = O::lt(O::abs(r1), c(0x1p-10));
        return {k, O::select(m, near, far), O::select(m, O::fnmadd(k, c(L::pi[3] / 2), O::sub(r1, near)), O::sub(O::sub(s, far), t))};
    }

    SIMD_ELEMENTARY_INLINE static V reduce_lean(V x, V k) noexcept { return reduce_lean_parts(x, k).r; }

    // x itself where the function rounds to its argument, raising underflow where the C library
    // does for such a result. The UCRT raises it where 0 < |x| <= m (m a normal power of two): x s
    // with s = (n / m) (1 - 2^-p), n the smallest normal and p the precision of T, falls below n
    // there both before and after rounding, so either tininess rule detects it, and is inexact.
    // glibc and musl raise it only below the accuracy type's smallest normal, whatever m: there
    // the same product with m that normal, and from it on a zero of x's sign in place of x, which
    // raises nothing. Elsewhere the product neither underflows nor overflows. x takes the
    // product's sign, which keeps the product without a select of the result for Clang to move.
    SIMD_ELEMENTARY_INLINE static V tiny_result(V x, double m) noexcept {
        using NL = std::numeric_limits<T>;
#if defined _UCRT
        const double s = static_cast<double>(NL::min()) / m * (1 - 1 / double(1ull << NL::digits));
        return O::copysign(x, O::mul(x, c(s)));
#else
        static_cast<void>(m);
        const double s = static_cast<double>(NL::min()) / min_normal() * (1 - 1 / double(1ull << NL::digits));
        const V      t = O::select(O::lt(O::abs(x), c(min_normal())), x, O::copysign(c(0), x));
        return O::copysign(x, O::mul(t, c(s)));
#endif
    }

    // sin x and cos x in float lanes for |x| <= trig_max: sin r, cos r, -sin r, -cos r by k mod 4
    // for sin, from k + 1 for cos, each polynomial within |r| <= pi/4
    SIMD_ELEMENTARY_INLINE static void sincos_lean(V x, V& k, V& s, V& co) noexcept {
        k         = O::round(O::mul(x, c(S::two_over_pi)));
        const V r = reduce_lean(x, k);
        const V z = O::mul(r, r);
        s         = O::fmadd(O::mul(r, z), poly(z, C::sin_pi4), r);
        co        = O::fmadd(O::mul(z, z), poly(z, C::cos_pi4), O::fnmadd(c(0.5), z, c(1)));
    }

    SIMD_ELEMENTARY_INLINE static V sin_lean(V x) noexcept {
        V k, s, co;
        sincos_lean(x, k, s, co);
        return O::flip(O::select(O::template kbit<0>(k), co, s), O::template kbit<1>(k));
    }

    SIMD_ELEMENTARY_INLINE static V cos_lean(V x) noexcept {
        V k, s, co;
        sincos_lean(x, k, s, co);
        return O::flip(O::select(O::template kbit<0>(k), s, co), O::template kbit<1>(O::add(k, c(1))));
    }

    // tan x in float lanes for |x| <= trig_max: tan(r + rlo) = r + c with c = r^3 q(r^2) + rlo (1
    // + r^2), as th + tl exactly; th itself for even k, and -1 / (th + tl) for odd k: d = -1 / th
    // rounded, e = 1 + d th exactly, fused or from the 12-bit halves of d and th, and d + d (e +
    // d tl). The numerator and the denominator of the division are both selected, which keeps
    // Clang from moving it past the select.
    SIMD_ELEMENTARY_INLINE static V tan_lean(V x) noexcept {
        const trig_parts p   = reduce_lean_parts(x, O::round(O::mul(x, c(S::two_over_pi))));
        const V          z   = O::mul(p.r, p.r);
        const V          cr  = O::fmadd(O::mul(p.r, z), poly(z, L::tan_pi4), O::fmadd(p.rlo, z, p.rlo));
        const V          th  = O::add(p.r, cr);
        const V          tl  = O::add(O::sub(p.r, th), cr);
        const M          odd = O::template kbit<0>(p.k);
        const V          den = O::select(odd, th, c(1));
        const V          d   = O::div(O::select(odd, c(-1), th), den);
        V                e;
        if constexpr (O::fused) {
            e = O::fmadd(d, den, c(1));
        } else {
            // each product exact, and each sum, taken from the largest term down
            const V dh = O::clear_low(d), dl = O::sub(d, dh);
            const V nh = O::clear_low(den), nl = O::sub(den, nh);
            e = O::fmadd(dl, nl, O::fmadd(dl, nh, O::fmadd(dh, nl, O::fmadd(dh, nh, c(1)))));
        }
        return O::select(odd, O::fmadd(d, O::fmadd(d, tl, e), d), th);
    }

    // The lanes beyond the reduction's range and the infinities, which go to the C library;
    // the lanes below 2^-27, where sin and tan round to x and cos to 1; and the argument the
    // reduction takes: 0 for the first, and 2^-30 for the second but 0 itself, so that cos raises
    // the inexact C raises for a tiny x.
    struct trig_lanes {
        M outside;
        M tiny;
        V arg;
    };

    SIMD_ELEMENTARY_INLINE static trig_lanes trig_split(V x) noexcept {
        const V a        = O::abs(x);
        const M outside  = O::lt(c(S::trig_max), a);
        const M tiny     = O::lt(a, c(0x1p-27));
        const V tiny_arg = O::select(O::eq(a, c(0)), c(0), c(0x1p-30));
        return {outside, tiny, O::select(outside, c(0), O::select(tiny, tiny_arg, x))};
    }

    // sin: at most 1 ulp (double), 2 ulp (float). sin r, cos r, -sin r, -cos r by k mod 4.
    template<typename F>
    SIMD_ELEMENTARY_INLINE static V sin_(V x) noexcept {
        const trig_lanes l = trig_split(x);
        if constexpr (lean) {
            return O::template fallback<F>(x, O::select(l.tiny, tiny_result(x, min_normal()), sin_lean(l.arg)), l.outside);
        }
        const trig_parts p   = trig_reduce(l.arg);
        const V          z   = O::mul(p.r, p.r);
        const V          s   = sin_poly(p.r, p.rlo, z);
        const V          co  = cos_poly(p.r, p.rlo, z);
        V                res = O::select(O::template kbit<0>(p.k), co, s);
        res                  = O::flip(res, O::template kbit<1>(p.k));
        res                  = O::select(l.tiny, tiny_result(x, min_normal()), res); // keeps the sign of a zero
        return O::template fallback<F>(x, res, l.outside);
    }

    // cos: at most 1 ulp (double), 2 ulp (float). cos r, -sin r, -cos r, sin r by k mod 4.
    template<typename F>
    SIMD_ELEMENTARY_INLINE static V cos_(V x) noexcept {
        const trig_lanes l = trig_split(x);
        if constexpr (lean) {
            return O::template fallback<F>(x, O::select(l.tiny, c(1), cos_lean(l.arg)), l.outside);
        }
        const trig_parts p   = trig_reduce(l.arg);
        const V          z   = O::mul(p.r, p.r);
        const V          s   = sin_poly(p.r, p.rlo, z);
        const V          co  = cos_poly(p.r, p.rlo, z);
        V                res = O::select(O::template kbit<0>(p.k), s, co);
        res                  = O::flip(res, O::template kbit<1>(O::add(p.k, c(1))));
        res                  = O::select(l.tiny, c(1), res);
        return O::template fallback<F>(x, res, l.outside);
    }

    // tan: at most 1.1 ulp (double), 2 ulp (float). sin r / cos r, or -cos r / sin r for odd k.
    template<typename F>
    SIMD_ELEMENTARY_INLINE static V tan_(V x) noexcept {
        const trig_lanes l = trig_split(x);
        if constexpr (lean) {
            return O::template fallback<F>(x, O::select(l.tiny, tiny_result(x, min_normal()), tan_lean(l.arg)), l.outside);
        }
        const trig_parts p = trig_reduce(l.arg);
        const V          z = O::mul(p.r, p.r);
        V                res;
        if constexpr (widened) {
            const V s   = sin_poly(p.r, p.rlo, z);
            const V co  = cos_poly(p.r, p.rlo, z);
            const M odd = O::template kbit<0>(p.k);
            res         = O::div(O::select(odd, O::flip(co, odd), s), O::select(odd, s, co));
        } else {
            // double: n / d with n = nh + nl and d = dh + dl, q = nh (1 / dh) rounded, e = n -
            // q d with nh - q dh exact, fused or from the 21-bit high parts of q and dh, and the
            // result q + e / dh
            V sh, sl, ch, cl;
            sincos_parts(p.r, p.rlo, z, sh, sl, ch, cl);
            const M odd = O::template kbit<0>(p.k);
            const V nh  = O::select(odd, O::flip(ch, odd), sh);
            const V nl  = O::select(odd, O::flip(cl, odd), sl);
            const V dh  = O::select(odd, sh, ch);
            const V dl  = O::select(odd, sl, cl);
            const V inv = O::div(c(1), dh);
            const V q   = O::mul(nh, inv);
            V       e;
            if constexpr (O::fused) {
                e = O::fnmadd(q, dh, nh);
            } else {
                // each product exact, ql split again for ql el, and each difference, taken from
                // the largest term down
                const V qh = O::clear_low(q), ql = O::sub(q, qh);
                const V qlh = O::clear_low(ql), qll = O::sub(ql, qlh);
                const V eh = O::clear_low(dh), el = O::sub(dh, eh);
                e = O::fnmadd(ql, eh, O::fnmadd(qh, el, O::fnmadd(qh, eh, nh)));
                e = O::fnmadd(qll, el, O::fnmadd(qlh, el, e));
            }
            res = O::fmadd(O::add(e, O::fnmadd(q, dl, nl)), inv, q);
        }
        res = O::select(l.tiny, tiny_result(x, min_normal()), res);
        return O::template fallback<F>(x, res, l.outside);
    }

    // atan, atan2. For 0 <= t, atan t = b + atan u over five ranges with the bounds 7/16, 11/16,
    // 19/16 and 39/16: b = 0, atan(1/2), atan(1), atan(3/2), pi/2 and u = t, (2t - 1) / (2 + t),
    // (t - 1) / (t + 1), (2t - 3) / (2 + 3t), -1/t, whose numerators are exact; |u| <= 7/16.

    struct atan_parts {
        V hi;  // b
        V low; // atan u plus the low part of b
    };

    SIMD_ELEMENTARY_INLINE static atan_parts atan_reduce(V t) noexcept {
        if constexpr (lean) {
            // float lanes: above tan(3 pi / 8), pi/2 + atan(-1/t); above tan(pi/8), pi/4 + atan((t -
            // 1) / (t + 1)); |u| <= tan(pi/8), within the float table's 7/16. b + u is taken exactly
            // as s + e (|b| > |u|); the numerator and the denominator are both selected, which keeps
            // Clang from moving the division past the select onto -1/0
            const M far = O::lt(c(2.414213562373095), t);
            const M mid = O::lt(c(0.41421356237309503), t);
            const V num = O::select(far, c(-1), O::select(mid, O::sub(t, c(1)), t));
            const V den = O::select(far, t, O::select(mid, O::add(t, c(1)), c(1)));
            const V u   = O::div(num, den);
            const V b   = O::select(far, c(S::pi2_hi), O::select(mid, c(S::pi4_hi), c(0)));
            const V blo = O::select(far, c(S::pi2_lo), O::select(mid, c(S::pi4_lo), c(0)));
            const V sum = O::add(b, u);
            const V e   = O::add(O::sub(b, sum), u);
            const V z   = O::mul(u, u);
            return {sum, O::add(e, O::fmadd(O::mul(u, z), poly(z, C::atan), blo))};
        }
        if constexpr (widened) {
            // float accuracy: above tan(3 pi / 8), pi/2 + atan(-1/t); above tan(pi/8),
            // pi/4 + atan((t - 1) / (t + 1)); the cancellation there costs bits double can spare
            const M far = O::lt(c(2.414213562373095), t);
            const M mid = O::lt(c(0.41421356237309503), t);
            const V num = O::select(far, c(-1), O::select(mid, O::sub(t, c(1)), t));
            const V den = O::select(far, t, O::select(mid, O::add(t, c(1)), c(1)));
            const V u   = O::div(num, den);
            const V z   = O::mul(u, u);
            return {O::select(far, c(1.5707963267948966), O::select(mid, c(0.7853981633974483), c(0))), O::fmadd(O::mul(u, z), poly(z, C::atan), u)};
        }
        const M r1 = O::le(c(0.4375), t);
        const M r2 = O::le(c(0.6875), t);
        const M r3 = O::le(c(1.1875), t);
        const M r4 = O::le(c(2.4375), t);
        // numerator p t - q, denominator r + s t: (1, 0, 1, 0), (2, 1, 2, 1), (1, 1, 1, 1),
        // (2, 3, 2, 3); the last range is -1 / t
        const V p   = O::select(r3, c(2), O::select(r2, c(1), O::select(r1, c(2), c(1))));
        const V q   = O::select(r3, c(3), O::select(r2, c(1), O::select(r1, c(1), c(0))));
        const V s   = O::select(r3, c(3), O::select(r2, c(1), O::select(r1, c(1), c(0))));
        const V rr  = O::select(r3, c(2), O::select(r2, c(1), O::select(r1, c(2), c(1))));
        const V num = O::select(r4, c(-1), O::fmadd(p, t, O::mul(c(-1), q)));
        const V den = O::select(r4, t, O::fmadd(s, t, rr));
        const V u   = O::div(num, den);
        const V bhi = O::select(r4, c(S::pi2_hi), O::select(r3, c(S::atan_3half_hi), O::select(r2, c(S::pi4_hi), O::select(r1, c(S::atan_half_hi), c(0)))));
        const V blo = O::select(r4, c(S::pi2_lo), O::select(r3, c(S::atan_3half_lo), O::select(r2, c(S::pi4_lo), O::select(r1, c(S::atan_half_lo), c(0)))));
        const V z   = O::mul(u, u);
        return {bhi, O::add(u, O::fmadd(O::mul(u, z), poly(z, C::atan), blo))};
    }

    // atan: at most 1 ulp (double), 2 ulp (float). |x| is limited to atan_max (2^60 for double,
    // 2^26 for float), where atan rounds to pi/2, so that no range's formula overflows or
    // underflows, and below 2^-28, where it rounds to x, taken as 0.
    SIMD_ELEMENTARY_INLINE static V atan_(V x) noexcept {
        const V          a    = O::abs(x);
        const M          tiny = O::lt(a, c(0x1p-28));
        const atan_parts p    = atan_reduce(O::min(O::select(tiny, c(0), a), c(S::atan_max)));
        // the UCRT raises underflow for double atan of |x| up to twice the smallest normal and for
        // float atan not at all; glibc and musl raise it below the smallest normal for both, which
        // float atan leaves out
        const V small = std::is_same_v<Acc, double> ? tiny_result(x, 2 * min_normal()) : x;
        return O::select(tiny, small, O::copysign(O::add(p.hi, p.low), x));
    }

    // atan2: at most 2 ulp. t = min(|x|, |y|) / max(|x|, |y|), at most 1, so that the division
    // neither overflows nor divides by zero; atan t, then pi/2 minus it where |y| > |x| and pi
    // minus that where x is negative or -0, each difference compensated, and the sign of y. Both
    // zero gives t = 0 and both infinite t = 1, C's values for those cases.
    SIMD_ELEMENTARY_INLINE static V atan2_(V y, V x) noexcept {
        const V ay       = O::abs(y);
        const V ax       = O::abs(x);
        const M swap     = O::lt(ax, ay);
        const V hi       = O::max(ax, ay);
        const V lo       = O::min(ax, ay);
        const M both_inf = O::mand(O::eq(ax, infinity()), O::eq(ay, infinity()));
        // where the result is pi/2 or pi minus a ratio below 2^-60, the ratio is taken as 0, which
        // spares its underflow
        const M negligible = O::mand(O::mor(swap, O::signbit(x)), O::lt(O::add(O::exponent(lo), c(60)), O::exponent(hi)));
        const V num        = O::select(both_inf, c(1), O::select(negligible, c(0), lo));
        const V den        = O::select(O::mor(both_inf, O::eq(hi, c(0))), c(1), hi);
        return atan2_from_ratio(y, x, swap, O::div(num, den));
    }

    // atan2 from t = min(|x|, |y|) / max(|x|, |y|) and where |y| > |x|
    SIMD_ELEMENTARY_INLINE static V atan2_from_ratio(V y, V x, M swap, V t) noexcept {
        const M    tiny = O::lt(t, c(0x1p-28)); // atan t rounds to t; t^2 would underflow
        atan_parts p;
        if constexpr (lean) {
            // float lanes: t <= 1, the polynomial alone
            const V tt = O::select(tiny, c(0), t);
            const V z  = O::mul(tt, tt);
            p          = {c(0), O::fmadd(O::mul(tt, z), poly(z, L::atan_1), tt)};
        } else {
            p = atan_reduce(O::select(tiny, c(0), t));
        }
        p.low = O::select(tiny, t, p.low);
        // pi/2 - (hi + low), where |pi/2_hi| > |hi| makes the first difference a fast two-sum
        const V d1 = O::sub(c(S::pi2_hi), p.hi);
        const V e1 = O::sub(O::sub(c(S::pi2_hi), d1), p.hi);
        const V h  = O::select(swap, d1, p.hi);
        const V l  = O::select(swap, O::sub(O::add(e1, c(S::pi2_lo)), p.low), p.low);
        // pi - (h + l), the same way
        const V d   = O::sub(c(S::pi_hi), h);
        const V de  = O::sub(O::sub(c(S::pi_hi), d), h);
        const V neg = O::add(d, O::sub(O::add(de, c(S::pi_lo)), l));
        return O::copysign(O::select(O::signbit(x), neg, O::add(h, l)), y);
    }

    // hypot, pow.

    // hypot at float accuracy: at most 2 ulp in float lanes. An infinite argument gives +inf even
    // beside a NaN, as C has it. Double hypot is left to the caller's library.
    // In double lanes, sqrt(x^2 + y^2), where the squares of float values are exact and their sum
    // rounds once in double. In float lanes, the squares as they are where every lane's are normal;
    // otherwise both scaled by the power of two that brings the larger into [1, 4) (a subnormal
    // stays below 1), so that neither square overflows or underflows, the smaller taken as 0 below
    // 2^-30 of the larger, where its square falls below the sum's last bit, and the infinities as
    // 0, since the select decides their lanes.
    SIMD_ELEMENTARY_INLINE static V hypot_(V x, V y) noexcept {
        static_assert(std::is_same_v<Acc, float>, "hypot at double accuracy is the simd library's");
        const M inf = O::mor(O::eq(O::abs(x), infinity()), O::eq(O::abs(y), infinity()));
        if constexpr (widened) {
            return O::select(inf, infinity(), O::sqrt(O::fmadd(x, x, O::mul(y, y))));
        } else {
            const V ax = O::select(inf, c(0), O::abs(x));
            const V ay = O::select(inf, c(0), O::abs(y));
            const V hi = O::max(ax, ay);
            const V lo = O::min(ax, ay);
            // when no lane's square can overflow or underflow, the squares as they are
            const M rare = O::mor(O::mor(O::lt(c(0x1p60), hi), O::mand(O::lt(c(0), hi), O::lt(hi, c(0x1p-60)))), O::mand(O::lt(c(0), lo), O::lt(lo, c(0x1p-60))));
            if (!O::any(rare)) {
                return O::select(inf, infinity(), O::sqrt(O::fmadd(hi, hi, O::mul(lo, lo))));
            }
            const V eh  = O::exponent(hi); // -127 for 0 and the subnormals
            const V e   = O::min(O::max(eh, c(-126)), c(126));
            const V s   = O::pow2(O::mul(e, c(-1)));
            const V his = O::mul(hi, s);
            const V los = O::mul(O::select(O::lt(O::add(O::exponent(lo), c(30)), eh), c(0), lo), s);
            const V h   = O::sqrt(O::fmadd(his, his, O::mul(los, los)));
            return O::select(inf, infinity(), O::mul(h, O::pow2(e)));
        }
    }

    // a b as p - n exactly in float lanes, n = p - a b taken with its sign so that no step
    // negates: n from one fused multiply-add, or from the 12-bit halves of clear_low, whose
    // products are exact, taken away from the largest term down (Dekker); bh and bl are b's halves
    SIMD_ELEMENTARY_INLINE static void two_prod(V a, V b, V bh, V bl, V& p, V& n) noexcept {
        p = O::mul(a, b);
        if constexpr (O::fused) {
            n = O::fnmadd(a, b, p);
        } else {
            const V ah = O::clear_low(a), al = O::sub(a, ah);
            n = O::fnmadd(al, bl, O::fnmadd(al, bh, O::fnmadd(ah, bl, O::fnmadd(ah, bh, p))));
        }
    }

    SIMD_ELEMENTARY_INLINE static void two_prod(V a, V b, V& p, V& n) noexcept {
        const V bh = O::clear_low(b);
        two_prod(a, b, bh, O::sub(b, bh), p, n);
    }

    // log2 x for pow in float lanes, x positive and finite, as hi + lo within about 2^-33 of it:
    // x = 2^e m with m in [sqrt(1/2), sqrt 2] and c the nearest of 1 and 2^(k / 5), so that
    // |s| <= 0.035 for s = (m - c) / (m + c), and log2 m = log2 c + 2 atanh(s) / ln 2. m - c is
    // exact, m + c and s are taken as hi + lo, 2 / ln 2 and log2 c in two parts each; the terms
    // past s, below 0.0004 of the sum, in float from s's hi part alone.
    SIMD_ELEMENTARY_INLINE static void log2_hilo(V x, V& hi, V& lo) noexcept {
        const M sub = O::lt(x, c(min_normal()));
        const V xs  = O::mul(x, O::select(sub, c(16777216.), c(1))); // 2^24
        log2_hilo(O::sub(O::exponent(xs), O::select(sub, c(24), c(0))), O::mantissa(xs), hi, lo);
    }

    // the same from x's exponent e and mantissa m in [1, 2). Above sqrt 2 m stands for m / 2 with
    // e + 1, and c for twice its center, which leaves s as it is, as both scale by 2 exactly: the
    // center chosen from m itself, without m / 2 on the way.
    SIMD_ELEMENTARY_INLINE static void log2_hilo(V e, V m, V& hi, V& lo) noexcept {
        const M top = O::lt(c(1.4142135623730951), m);
        e           = O::select(top, O::add(e, c(1)), e);
        // the bounds 2^(1/10), 2^(3/10) below sqrt 2, and twice 2^(-3/10), 2^(-1/10) above
        const M    b0   = O::lt(m, c(2 * 0.8122523963562356));
        const M    b1   = O::lt(m, c(2 * 0.9330329915368074));
        const M    b2   = O::lt(c(1.0717734625362931), m);
        const M    b3   = O::lt(c(1.2311444133449163), m);
        const auto pick = [&](const double* k, double mid, double scale) {
            const V below = O::select(b3, c(k[3]), O::select(b2, c(k[2]), c(mid)));
            const V above = O::select(b0, c(scale * k[0]), O::select(b1, c(scale * k[1]), c(scale * mid)));
            return O::select(top, above, below);
        };
        const V cm  = pick(L::pow_c, 1, 2);
        const V num = O::sub(m, cm);
        V       dh, dl;
        two_sum(m, cm, dh, dl);
        const V inv = O::div(c(1), dh);
        const V sh  = O::mul(num, inv);
        // sh's residual num - sh (dh + dl): with FMA two fused steps, each exact or nearly; without,
        // through sh dh as an exact product. s's low part, times 2 / ln 2, is the residual times
        // that multiple of inv.
        V res;
        if constexpr (O::fused) {
            res = O::fnmadd(sh, dl, O::fnmadd(sh, dh, num));
        } else {
            V p, pn;
            two_prod(sh, dh, p, pn);
            res = O::fnmadd(sh, dl, O::add(O::sub(num, p), pn));
        }
        const V z    = O::mul(sh, sh);
        const V tail = O::mul(O::mul(sh, z), horner(z, L::log2_atanh));
        // 2 / ln 2 in two parts, the first split into its 12-bit halves
        V ph, pn;
        two_prod(sh, c(2.885390043258667), c(2.884765625), c(0.0006244182586669922), ph, pn);
        const V low = O::fmadd(O::mul(inv, c(2.885390043258667)), res, O::sub(O::fmadd(c(3.851926067000022e-08), sh, pick(L::pow_lc_lo, 0, 1)), pn));
        // e + log2 c's high part, exact; hi = that + ph with its error exactly, as the sum is 0 or
        // at least 0.2, above |ph|; the tail, the latest term, added last
        const V ec = O::add(e, pick(L::pow_lc_hi, 0, 1));
        hi         = O::add(ec, ph);
        lo         = O::add(O::add(O::sub(ec, hi), ph), O::add(low, tail));
    }

    // 2^r = 1 + r q(r) for |r| <= 1/2 in float lanes, rounded once with FMA. With FMA q is its
    // constant term plus r Y(r), rounded once at q's magnitude, with Y by Estrin's scheme from
    // r^2 + 2^-60 and its square: neither underflows, and the fused product r r raises nothing for
    // a tiny r, while 2^-60 is far below Y's last bit; without, q by the two chains of poly, which
    // keep their products normal.
    SIMD_ELEMENTARY_INLINE static V exp2_lean(V r) noexcept {
        if constexpr (O::fused) {
            const auto& k   = L::exp2;
            const V     r2  = O::fmadd(r, r, c(0x1p-60));
            const V     r4  = O::mul(r2, r2);
            const V     y01 = O::fmadd(c(k[4]), r, c(k[5]));
            const V     y23 = O::fmadd(c(k[2]), r, c(k[3]));
            const V     y45 = O::fmadd(c(k[0]), r, c(k[1]));
            const V     q   = O::fmadd(r, O::fmadd(r4, y45, O::fmadd(r2, y23, y01)), c(k[6]));
            return O::fmadd(r, q, c(1));
        } else {
            return O::fmadd(r, poly(r, L::exp2), c(1));
        }
    }

    // pow in float lanes: at most 2 ulp. 2^t with t = y log2 |x|, y times log2_hilo's hi as an
    // exact product; t = k + r with k = round(t), |r| <= 1/2, and 2^k as 2^k1 2^(k - k1) as in
    // exp. A y below 2^-17 is rounded to a multiple of 2^-40, as y + 2^-17 - 2^-17: t moves by
    // less than 2^-33, and a tiny y's products do not underflow. |y| is limited to 2^32, which
    // leaves every finite result that is not 0 or infinite as it is, as |log2 |x|| >= 2^-24 for
    // x != 1, and keeps the product finite; t to [-160, 130], past which the result has
    // underflowed or overflowed, and r to [-1, 1] there. The lanes where t is infinite, x zero or
    // infinite or y infinite, go in as 1^0 and take +inf or 0 by the sign of t; then the sign and
    // the special values of C's pow.
    SIMD_ELEMENTARY_INLINE static V pow_lean(V x, V y) noexcept {
        const V one     = c(1);
        const V ax      = O::abs(x);
        const M yint    = O::is_int(y);
        const M xinf    = O::eq(ax, infinity());
        const M yinf    = O::eq(O::abs(y), infinity());
        const M zero    = O::eq(ax, c(0));
        const M no_real = O::mandnot(O::mandnot(O::mandnot(O::lt(x, c(0)), xinf), yinf), yint);
        V       hi, lo;
        log2_hilo(O::select(O::mor(zero, xinf), one, ax), hi, lo);
        const V y0 = O::select(O::mor(yinf, no_real), c(0), y);
        const V ys = O::select(O::lt(O::abs(y0), c(0x1p-17)), O::sub(O::add(y0, c(0x1p-17)), c(0x1p-17)), y0);
        const V yc = O::min(O::max(ys, c(-0x1p32)), c(0x1p32));
        V       th, tn;
        two_prod(yc, hi, th, tn);
        const V tc = O::min(O::max(th, c(-160)), c(130));
        const V k  = O::round(tc);
        const V r  = O::min(O::max(O::sub(O::sub(tc, k), O::fnmadd(yc, lo, tn)), c(-1)), one);
        const V k1 = O::min(O::max(k, c(-126)), c(127));
        // 2^r 2^k1, with no product below the normal range
        const V res = O::mul(O::mul(exp2_lean(r), O::pow2(k1)), O::pow2(O::sub(k, k1)));
        // the special lanes' values, taken by one select at the end: 1 for |x| = 1 with an infinite
        // y, and x itself for y = 1, exactly as C has it; y = 0 and x = 1 are the caller's. y is
        // halved from |y| >= 1, as a subnormal y halved would underflow and no odd y is below 1.
        const M yodd = O::mandnot(yint, O::is_int(O::mul(O::max(O::abs(y), one), c(0.5))));
        const M neg  = O::mand(O::signbit(x), yodd);
        const M edge = O::mor(O::mor(zero, xinf), yinf);
        // t's sign: y's, flipped where |x| < 1
        const M tneg = O::signbit(O::flip(y, O::signbit(O::sub(ax, one))));
        V       sv   = O::flip(O::select(tneg, c(0), infinity()), neg);
        sv           = O::select(no_real, quiet_nan(), sv);
        sv           = O::select(O::mand(O::eq(ax, one), yinf), one, sv);
        const M y1   = O::eq(y, one);
        sv           = O::select(y1, x, sv);
        return O::select(O::mor(O::mor(edge, no_real), y1), sv, O::flip(res, neg));
    }

    // pow at float accuracy, in double lanes: at most 1 ulp. e^(y log |x|), with log |x| at
    // double accuracy, which leaves the 29 bits beyond float that the exponential's sensitivity to
    // its argument needs (|y log x| reaches 104 before the result leaves float); then the sign and
    // the special values of C's pow.
    SIMD_ELEMENTARY_INLINE static V pow_(V x, V y) noexcept {
        static_assert(widened, "pow at float accuracy computes in double lanes; at double accuracy "
                               "it needs log beyond double");
        using D     = elementary<O, double>;
        const V one = c(1);
        const V ax  = O::abs(x);
        const V lg  = D::log_special(ax, D::log_finite(ax));
        // a negative base: an integer exponent keeps the magnitude, negated for an odd one; any
        // other finite exponent of a finite negative base has no real result, and its lane goes into
        // the exponential as 0, which spares the overflow or underflow C does not raise there
        const M yint       = O::is_int(y);
        const M yodd       = O::mandnot(yint, O::is_int(O::mul(y, c(0.5))));
        const M finite_neg = O::mand(O::lt(x, c(0)), O::lt(ax, infinity()));
        const M no_real    = O::mandnot(O::mand(finite_neg, O::lt(O::abs(y), infinity())), yint);
        const M unit_inf   = O::mand(O::eq(ax, one), O::eq(O::abs(y), infinity()));
        // y as 0 there and in |x| = 1 with an infinite y, log |x| as 0 for y = 0, so that 0 times an
        // infinity raises nothing either; the two masks differ, which keeps Clang from merging the
        // selects into one past the product
        V r = exp_(O::mul(O::select(O::mor(unit_inf, no_real), c(0), y), O::select(O::eq(y, c(0)), c(0), lg)));
        r   = O::flip(r, O::mand(O::signbit(x), yodd));
        r   = O::select(no_real, quiet_nan(), r);
        // |x| = 1 with an infinite exponent, any base to the power 0 (NaN included) and a base of
        // +1 to any power give 1
        r = O::select(O::mand(O::eq(ax, one), O::eq(O::abs(y), infinity())), one, r);
        r = O::select(O::eq(y, one), x, r); // exactly, as C has it
        return O::select(O::mor(O::eq(y, c(0)), O::eq(x, one)), one, r);
    }
    // The functions. A NaN argument goes into the forms above as an ordinary number and its result
    // is chosen at the end, so that no ordered comparison (lt, le) and no min or max sees a NaN:
    // those raise invalid for one, where eq and isnan do not.

    SIMD_ELEMENTARY_INLINE static V nan_free(M nan, V x, double value) noexcept { return O::select(nan, c(value), x); }

    // Float lanes take a shorter path when every lane's argument lies where no special value,
    // flag or fallback needs a select: the same operations without them, which give the full
    // form's value unless the compiler contracts a product into a sum across statements (GCC's
    // -ffp-contract=fast), where a lane may differ by an ulp within the bound.
    SIMD_ELEMENTARY_INLINE static bool all_within(V a, double lo, double hi) noexcept { return O::all_within(a, lo, hi); }

    SIMD_ELEMENTARY_INLINE static V exp(V x) noexcept {
        if constexpr (lean) {
            if (all_within(O::abs(x), 0x1p-60, 87)) {
                return exp_normal(x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, exp_(nan_free(nan, x, 0)));
    }

    // sinh and cosh up to |x| = 86, where e^|x| / 2 <= 2^124 keeps 1 / (4 h) normal; from 20 on h
    // -+ 1 / (4 h) rounds to h, which the full forms take there
    SIMD_ELEMENTARY_INLINE static V sinh(V x) noexcept {
        if constexpr (lean) {
            const V a = O::abs(x);
            if (all_within(a, 0x1p-28, 86)) {
                // the polynomial on |x| itself, as odd_poly's limits change nothing here, and x's
                // sign once at the end: the polynomial of -|x| is that of |x| negated exactly
                const V h     = half_exp_normal(a);
                const V m     = O::min(a, c(1));
                const V z     = O::mul(m, m);
                const V small = O::fmadd(O::mul(m, z), poly(z, C::sinh), m);
                return O::copysign(O::select(O::lt(a, c(1)), small, O::sub(h, O::div(c(0.25), h))), x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, sinh_(nan_free(nan, x, 0)));
    }

    SIMD_ELEMENTARY_INLINE static V cosh(V x) noexcept {
        if constexpr (lean) {
            const V a = O::abs(x);
            if (all_within(a, 0x1p-60, 86)) {
                const V h = half_exp_normal(a);
                return O::add(h, O::div(c(0.25), h));
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, cosh_(nan_free(nan, x, 0)));
    }

    // up to |x| = 43, where e^(2|x|) is normal and 2 / (e^(2|x|) + 1) too; from 20 on the result
    // is 1, as in the full form, which limits |x| to 20
    SIMD_ELEMENTARY_INLINE static V tanh(V x) noexcept {
        if constexpr (lean) {
            const V a = O::abs(x);
            if (all_within(a, 0x1p-28, 43)) {
                // as in sinh, the polynomial on |x| and the sign at the end
                const V e2    = exp_normal(O::add(a, a));
                const V m     = O::min(a, c(0.75));
                const V z     = O::mul(m, m);
                const V small = O::fmadd(O::mul(m, z), poly(z, C::tanh), m);
                const V large = O::sub(c(1), O::div(c(2), O::add(e2, c(1))));
                return O::copysign(O::select(O::lt(a, c(0.75)), small, large), x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, tanh_(nan_free(nan, x, 0)));
    }

    // positive normal arguments
    SIMD_ELEMENTARY_INLINE static bool all_normal(V x) noexcept { return all_within(x, static_cast<double>(std::numeric_limits<T>::min()), static_cast<double>(std::numeric_limits<T>::max())); }

    SIMD_ELEMENTARY_INLINE static V log(V x) noexcept {
        if constexpr (lean) {
            if (all_normal(x)) {
                return log_value(log_reduce_normal(x, c(0)));
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, log_(nan_free(nan, x, 0)));
    }

    SIMD_ELEMENTARY_INLINE static V log2(V x) noexcept {
        if constexpr (lean) {
            if (all_normal(x)) {
                return log2_value(log_reduce_normal(x, c(0)));
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, log2_(nan_free(nan, x, 0)));
    }

    SIMD_ELEMENTARY_INLINE static V log10(V x) noexcept {
        if constexpr (lean) {
            if (all_normal(x)) {
                return log10_value(log_reduce_normal(x, c(0)));
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, log10_(nan_free(nan, x, 0)));
    }

    SIMD_ELEMENTARY_INLINE static V atan(V x) noexcept {
        if constexpr (lean) {
            if (all_within(O::abs(x), 0x1p-28, S::atan_max)) {
                const atan_parts p = atan_reduce(O::abs(x));
                return O::copysign(O::add(p.hi, p.low), x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, atan_(nan_free(nan, x, 0)));
    }

    template<typename F>
    SIMD_ELEMENTARY_INLINE static V sin(V x) noexcept {
        if constexpr (lean) {
            if (all_within(O::abs(x), 0x1p-27, S::trig_max)) {
                return sin_lean(x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, sin_<F>(nan_free(nan, x, 0)));
    }

    template<typename F>
    SIMD_ELEMENTARY_INLINE static V cos(V x) noexcept {
        if constexpr (lean) {
            if (all_within(O::abs(x), 0x1p-27, S::trig_max)) {
                return cos_lean(x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, cos_<F>(nan_free(nan, x, 0)));
    }

    template<typename F>
    SIMD_ELEMENTARY_INLINE static V tan(V x) noexcept {
        if constexpr (lean) {
            if (all_within(O::abs(x), 0x1p-27, S::trig_max)) {
                return tan_lean(x);
            }
        }
        const M nan = O::isnan(x);
        return O::select(nan, x, tan_<F>(nan_free(nan, x, 0)));
    }

    // |x| and |y| within [2^-28, 2^28]: no zero, infinity or NaN, and their ratio at least 2^-56,
    // which atan2_ would not take as negligible
    SIMD_ELEMENTARY_INLINE static V atan2(V y, V x) noexcept {
        if constexpr (lean) {
            const V ax = O::abs(x);
            const V ay = O::abs(y);
            if (all_within(ax, 0x1p-28, 0x1p28) && all_within(ay, 0x1p-28, 0x1p28)) {
                return atan2_from_ratio(y, x, O::lt(ax, ay), O::div(O::min(ax, ay), O::max(ax, ay)));
            }
        }
        const M nx  = O::isnan(x);
        const M ny  = O::isnan(y);
        const M nan = O::mor(nx, ny);
        const V r   = atan2_(nan_free(nan, y, 0), nan_free(nan, x, 0));
        return O::select(nx, x, O::select(ny, y, r));
    }

    // an infinite argument gives +inf even beside a NaN
    SIMD_ELEMENTARY_INLINE static V hypot(V x, V y) noexcept {
        if constexpr (lean) {
            // |x| and |y| within [2^-60, 2^60], where no square overflows or underflows
            const V ax = O::abs(x);
            const V ay = O::abs(y);
            if (all_within(ax, 0x1p-60, 0x1p60) && all_within(ay, 0x1p-60, 0x1p60)) {
                const V lo = O::min(ax, ay);
                const V hi = O::max(ax, ay);
                return O::sqrt(O::fmadd(hi, hi, O::mul(lo, lo)));
            }
        }
        const M nx  = O::isnan(x);
        const M ny  = O::isnan(y);
        const V r   = hypot_(nan_free(nx, x, 0), nan_free(ny, y, 0));
        const M inf = O::mor(O::eq(O::abs(x), infinity()), O::eq(O::abs(y), infinity()));
        return O::select(inf, infinity(), O::select(nx, x, O::select(ny, y, r)));
    }

    // pow(NaN, 0) and pow(1, NaN) are 1, other NaN arguments give a NaN. Float lanes with every x
    // positive and normal and every |y| within [2^-17, 2^32] skip the special values, and with
    // every |t| <= 125 also pow_lean's limits on t and r and its second power of two.
    SIMD_ELEMENTARY_INLINE static V pow(V x, V y) noexcept {
        if constexpr (lean) {
            if (all_normal(x) && all_within(O::abs(y), 0x1p-17, 0x1p32)) {
                V hi, lo, th, tn;
                log2_hilo(O::exponent(x), O::mantissa(x), hi, lo);
                two_prod(y, hi, th, tn);
                if (all_within(O::add(O::abs(th), c(1)), 1, 126)) {
                    const V k = O::round(th);
                    const V r = O::sub(O::sub(th, k), O::fnmadd(y, lo, tn));
                    return O::select(O::eq(y, c(1)), x, O::mul(exp2_lean(r), O::pow2(k)));
                }
            }
        }
        const M nx = O::isnan(x);
        const M ny = O::isnan(y);
        V       r;
        if constexpr (lean) {
            r = pow_lean(nan_free(nx, x, 1), nan_free(ny, y, 1)); // 1^y and x^1 raise nothing
        } else {
            r = pow_(nan_free(nx, x, 1), nan_free(ny, y, 1));
        }
        r = O::select(nx, x, O::select(ny, y, r));
        return O::select(O::mor(O::eq(y, c(0)), O::eq(x, c(1))), c(1), r);
    }
};
