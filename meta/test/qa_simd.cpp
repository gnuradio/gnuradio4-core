#include <boost/ut.hpp>

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include <gnuradio-4.0/meta/simd.hpp>

namespace qa_simd {

// The eligibility surface, pinned shape by shape. gr::meta::simdize<T> resolves either to a vir
// simd type, when the shape was vectorized, or to T itself, when it was not, so std::is_same_v
// reports the decision. These are compile-time assertions so that a future vir-simd version, or a
// change to the predicate, cannot move a shape between the two outcomes without failing here.

struct ArithmeticAggregate {
    double a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p;
};

struct ComplexAggregate {
    std::complex<float> c;
    float               f;
};

// The predicate below admits this shape, and it is deliberately not instantiated here: vir
// decomposes the aggregate and then fails inside vir::struct_get on the std::array member, which is
// a compile error rather than a declined vectorization. That behavior belongs to vir-simd and
// predates the predicate; before it, this header passed every type to vir::simdize unconditionally
// and the same shape failed the same way.
struct ArrayMemberAggregate {
    std::uint32_t             id;
    std::array<std::byte, 64> payload;
};

template<typename T>
inline constexpr bool isSimdized = !std::is_same_v<gr::meta::simdize<T>, T>;

static_assert(isSimdized<float>, "an arithmetic scalar must still vectorize");
static_assert(isSimdized<double>, "an arithmetic scalar must still vectorize");
static_assert(isSimdized<std::tuple<float, float>>, "a tuple of arithmetic members must still vectorize");
static_assert(isSimdized<std::pair<float, float>>, "a pair of arithmetic members must still vectorize");
static_assert(isSimdized<ArithmeticAggregate>, "an aggregate of arithmetic members must still vectorize");

static_assert(!isSimdized<std::complex<float>>, "std::complex is not an aggregate and must stay scalar");
static_assert(!isSimdized<ComplexAggregate>, "an aggregate containing std::complex must stay scalar");

// The prefilter admits this shape and vir rejects it by failing to compile. Narrowing the predicate
// to exclude it would also exclude ArithmeticAggregate above, which vectorizes today, so the gap is
// recorded here rather than closed at that cost.
static_assert(gr::meta::detail::vectorizable_shape<ArrayMemberAggregate>::value, "the prefilter's reach is wider than vir's");

} // namespace qa_simd

const boost::ut::suite<"simdize eligibility"> simdEligibilityTests = [] {
    using namespace boost::ut;

    "the eligibility surface is fixed at compile time"_test = [] {
        expect(qa_simd::isSimdized<float>);
        expect(not qa_simd::isSimdized<std::complex<float>>);
        expect(qa_simd::isSimdized<qa_simd::ArithmeticAggregate>);
        expect(not qa_simd::isSimdized<qa_simd::ComplexAggregate>);
    };
};

int main() { /* tests are statically registered */ }
