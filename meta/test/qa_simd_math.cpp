#include <boost/ut.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/meta/simd_math.hpp>

namespace qa_simd_math {

namespace stdx = gr::meta::stdx;

// The largest error over the arguments in ulp of the result, against a reference of higher
// precision (double for float, long double for double), and the number of special results (NaN,
// an infinity, a zero) that are not the C library's exactly: the same NaN-ness, infinity or sign.
struct Accuracy {
    long double maxUlp     = 0;
    long double worstArg   = 0;
    std::size_t mismatches = 0;
};

// A long double no wider than double makes the double reference the C library's double function,
// with an error of its own of up to about 1 ulp beside the function's.
template<typename T>
constexpr double kReferenceSlack = (sizeof(T) == sizeof(double) && std::numeric_limits<long double>::digits <= 53) ? 1.0 : 0.0;

template<typename T>
long double ulpOf(long double reference) {
    using L                = std::numeric_limits<T>;
    const long double size = std::fabs(reference);
    if (size < static_cast<long double>(L::min())) {
        return static_cast<long double>(L::denorm_min());
    }
    return std::ldexp(1.0L, std::ilogb(size) - (L::digits - 1));
}

template<typename T>
bool sameSpecial(T got, T want) {
    if (std::isnan(want)) {
        return std::isnan(got);
    }
    return got == want && std::signbit(got) == std::signbit(want);
}

template<typename T>
void accumulate(Accuracy& accuracy, T arg, T got, long double want) {
    if (!std::isfinite(want) || want == 0 || !std::isfinite(got)) {
        accuracy.mismatches += static_cast<std::size_t>(!sameSpecial(got, static_cast<T>(want)));
        return;
    }
    const long double error = std::fabs(static_cast<long double>(got) - want) / ulpOf<T>(want);
    if (error > accuracy.maxUlp) {
        accuracy.maxUlp   = error;
        accuracy.worstArg = static_cast<long double>(arg);
    }
}

template<typename V, typename F, typename R>
Accuracy measure(const std::vector<typename V::value_type>& args, F function, R reference) {
    using T = typename V::value_type;
    Accuracy accuracy;
    for (std::size_t i = 0UZ; i < args.size(); i += V::size()) {
        V       x([&](std::size_t lane) { return args[std::min(i + lane, args.size() - 1UZ)]; });
        const V result = function(x);
        for (std::size_t lane = 0UZ; lane < V::size() && i + lane < args.size(); ++lane) {
            accumulate<T>(accuracy, x[lane], result[lane], reference(x[lane]));
        }
    }
    return accuracy;
}

template<typename V, typename F, typename R>
Accuracy measure2(const std::vector<typename V::value_type>& first, const std::vector<typename V::value_type>& second, F function, R reference) {
    using T = typename V::value_type;
    Accuracy accuracy;
    for (std::size_t i = 0UZ; i < first.size(); i += V::size()) {
        const auto at = [&](const std::vector<T>& v) { return V([&](std::size_t lane) { return v[std::min(i + lane, v.size() - 1UZ)]; }); };
        const V    a  = at(first);
        const V    b  = at(second);
        const V    r  = function(a, b);
        for (std::size_t lane = 0UZ; lane < V::size() && i + lane < first.size(); ++lane) {
            accumulate<T>(accuracy, a[lane], r[lane], reference(a[lane], b[lane]));
        }
    }
    return accuracy;
}

template<typename T>
std::vector<T> specials() {
    using L = std::numeric_limits<T>;
    return {T(0), T(-0.), L::infinity(), -L::infinity(), L::quiet_NaN(), T(1), T(-1), T(0.5), T(-0.5), T(2), T(-2), L::max(), -L::max(), L::min(), -L::min(), L::denorm_min(), -L::denorm_min(), L::epsilon(), T(1000), T(-1000)};
}

template<typename T>
std::vector<T> uniform(std::size_t n, double lo, double hi, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> distribution(lo, hi);
    std::vector<T>                         v(n);
    for (T& x : v) {
        x = static_cast<T>(distribution(rng));
    }
    return v;
}

// magnitudes 2^[lo, hi], with a random sign when isSigned
template<typename T>
std::vector<T> logSpread(std::size_t n, double lo, double hi, std::mt19937_64& rng, bool isSigned = true) {
    std::uniform_real_distribution<double> exponent(lo, hi);
    std::bernoulli_distribution            negative(0.5);
    std::vector<T>                         v(n);
    for (T& x : v) {
        x = static_cast<T>(std::exp2(exponent(rng))) * (isSigned && negative(rng) ? T(-1) : T(1));
    }
    return v;
}

// the values of T nearest to multiples of pi/2 up to limit, where the reduction's error counts
// most: about 2000 multiples spread over the range, or every one where there are fewer
template<typename T>
std::vector<T> nearMultiplesOfHalfPi(double limit) {
    std::vector<T>  v;
    const long long kMax = static_cast<long long>(limit / 1.5707963267948966);
    const long long step = std::max(kMax / 2000, 1LL);
    for (long long k = 1; k <= kMax; k += step) {
        const T x = static_cast<T>(static_cast<long double>(k) * 1.5707963267948966192313216916397514L);
        for (T y : {x, std::nextafter(x, T(0)), std::nextafter(x, std::numeric_limits<T>::max())}) {
            v.push_back(y);
            v.push_back(-y);
        }
    }
    return v;
}

template<typename T, typename... TMore>
std::vector<T> concatenated(std::vector<T> first, const TMore&... more) {
    (first.insert(first.end(), more.begin(), more.end()), ...);
    return first;
}

template<typename T>
long double referenceOf(long double (*precise)(long double), double (*plain)(double), T x) {
    if constexpr (sizeof(T) == sizeof(float)) {
        return static_cast<long double>(plain(static_cast<double>(x)));
    } else {
        return precise(static_cast<long double>(x));
    }
}

template<typename T>
long double referenceOf(long double (*precise)(long double, long double), double (*plain)(double, double), T x, T y) {
    if constexpr (sizeof(T) == sizeof(float)) {
        return static_cast<long double>(plain(static_cast<double>(x), static_cast<double>(y)));
    } else {
        return precise(static_cast<long double>(x), static_cast<long double>(y));
    }
}

template<typename T>
concept TakesExp = requires(T x) { gr::meta::exp(x); };

// every function on V with float lanes computing as L says
template<typename V, gr::meta::FloatLanes L>
void checkAll() {
    using namespace boost::ut;
    using T          = typename V::value_type;
    constexpr bool f = sizeof(T) == sizeof(float);
    // the float bound: 2 in float lanes, 1 in double lanes
    constexpr double fb = L == gr::meta::FloatLanes::Float ? 2 : 1;
    constexpr auto   n  = 20000UZ;
    std::mt19937_64  rng(20261030);

    const auto check = [](std::string_view name, const Accuracy& accuracy, double bound) {
        expect(le(accuracy.maxUlp, static_cast<long double>(bound + kReferenceSlack<T>))) << name << "at" << static_cast<double>(accuracy.worstArg);
        expect(eq(accuracy.mismatches, 0UZ)) << name << "special values";
    };

    // bounds in ulp: every float function fb; double exp, log, log2, log10, sin, cos and atan 1,
    // tan 1.1, sinh, cosh, tanh and atan2 2; double hypot and pow are the simd library's
    // (libstdc++'s own simd hypot reaches 1.12), within 2 and 1
    const auto exps = concatenated(specials<T>(), uniform<T>(n, f ? -110 : -750, f ? 92 : 712, rng), uniform<T>(n, -2, 2, rng));
    check("exp", measure<V>(exps, [](V x) { return gr::meta::exp<L>(x); }, [](T x) { return referenceOf<T>(::expl, ::exp, x); }), f ? fb : 1);
    const auto hyperbolic = concatenated(specials<T>(), uniform<T>(n, f ? -95 : -715, f ? 95 : 715, rng), logSpread<T>(n, -40, 1, rng));
    check("sinh", measure<V>(hyperbolic, [](V x) { return gr::meta::sinh<L>(x); }, [](T x) { return referenceOf<T>(::sinhl, ::sinh, x); }), f ? fb : 2);
    check("cosh", measure<V>(hyperbolic, [](V x) { return gr::meta::cosh<L>(x); }, [](T x) { return referenceOf<T>(::coshl, ::cosh, x); }), f ? fb : 2);
    const auto tanhs = concatenated(specials<T>(), uniform<T>(n, -25, 25, rng), logSpread<T>(n, -40, 1, rng));
    check("tanh", measure<V>(tanhs, [](V x) { return gr::meta::tanh<L>(x); }, [](T x) { return referenceOf<T>(::tanhl, ::tanh, x); }), f ? fb : 2);

    const auto logs = concatenated(specials<T>(), logSpread<T>(n, f ? -149 : -1074, f ? 128 : 1024, rng, false), uniform<T>(n, 0.5, 2, rng));
    check("log", measure<V>(logs, [](V x) { return gr::meta::log<L>(x); }, [](T x) { return referenceOf<T>(::logl, ::log, x); }), f ? fb : 1);
    check("log2", measure<V>(logs, [](V x) { return gr::meta::log2<L>(x); }, [](T x) { return referenceOf<T>(::log2l, ::log2, x); }), f ? fb : 1);
    check("log10", measure<V>(logs, [](V x) { return gr::meta::log10<L>(x); }, [](T x) { return referenceOf<T>(::log10l, ::log10, x); }), f ? fb : 1);

    const auto trig = concatenated(specials<T>(), uniform<T>(n, -10, 10, rng), uniform<T>(n, f ? -1e7 : -1e8, f ? 1e7 : 1e8, rng), logSpread<T>(n / 4, -40, 0, rng), nearMultiplesOfHalfPi<T>(67108864.0), nearMultiplesOfHalfPi<T>(8192.0));
    check("sin", measure<V>(trig, [](V x) { return gr::meta::sin<L>(x); }, [](T x) { return referenceOf<T>(::sinl, ::sin, x); }), f ? fb : 1);
    check("cos", measure<V>(trig, [](V x) { return gr::meta::cos<L>(x); }, [](T x) { return referenceOf<T>(::cosl, ::cos, x); }), f ? fb : 1);
    check("tan", measure<V>(trig, [](V x) { return gr::meta::tan<L>(x); }, [](T x) { return referenceOf<T>(::tanl, ::tan, x); }), f ? fb : 1.1);
    const auto atans = concatenated(specials<T>(), logSpread<T>(n, -40, 40, rng), uniform<T>(n, -3, 3, rng));
    check("atan", measure<V>(atans, [](V x) { return gr::meta::atan<L>(x); }, [](T x) { return referenceOf<T>(::atanl, ::atan, x); }), f ? fb : 1);

    // two arguments: every pair of special values, then magnitudes over 2^[-30, 30]
    std::vector<T> first;
    std::vector<T> second;
    for (T a : specials<T>()) {
        for (T b : specials<T>()) {
            first.push_back(a);
            second.push_back(b);
        }
    }
    first  = concatenated(first, logSpread<T>(n, -30, 30, rng));
    second = concatenated(second, logSpread<T>(n, -30, 30, rng));
    check("atan2", measure2<V>(first, second, [](V y, V x) { return gr::meta::atan2<L>(y, x); }, [](T y, T x) { return referenceOf<T>(::atan2l, ::atan2, y, x); }), f ? fb : 2);
    check("hypot", measure2<V>(first, second, [](V x, V y) { return gr::meta::hypot<L>(x, y); }, [](T x, T y) { return referenceOf<T>(::hypotl, ::hypot, x, y); }), f ? fb : 2);
    // and bases near 1 with exponents up to 150, where an error in log x is multiplied by |y|
    const auto bases     = concatenated(first, logSpread<T>(n, -20, 20, rng, false), uniform<T>(n, 0.5, 2, rng));
    const auto exponents = concatenated(second, uniform<T>(n, -6, 6, rng), uniform<T>(n, -150, 150, rng));
    check("pow", measure2<V>(bases, exponents, [](V x, V y) { return gr::meta::pow<L>(x, y); }, [](T x, T y) { return referenceOf<T>(::powl, ::pow, x, y); }), f ? fb : 1);
}

} // namespace qa_simd_math

const boost::ut::suite<"simd elementary functions"> simdMathTests = [] {
    using namespace boost::ut;
    using namespace qa_simd_math;

    "each function stays within its bound in ulp and gives C's special values"_test = []<typename V>(const V&) {
        checkAll<V, gr::meta::FloatLanes::Float>();
        if constexpr (std::same_as<typename V::value_type, float>) {
            checkAll<V, gr::meta::FloatLanes::Double>();
        }
    } | std::tuple<stdx::native_simd<float>, stdx::fixed_size_simd<float, 7>, stdx::native_simd<double>, stdx::fixed_size_simd<double, 7>>{};

    "an integer argument is refused"_test = [] {
        static_assert(!TakesExp<int>);
        static_assert(TakesExp<double>);
    };

    "a sample goes to the C library"_test = [] {
        expect(eq(gr::meta::exp(1.5), std::exp(1.5)));
        expect(eq(gr::meta::atan2(1.0f, -2.0f), std::atan2(1.0f, -2.0f)));
        expect(eq(gr::meta::pow(2.0, 0.5), std::pow(2.0, 0.5)));
    };
};

int main() { /* tests are statically registered */ }
