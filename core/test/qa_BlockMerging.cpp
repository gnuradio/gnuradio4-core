#include <type_traits>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/meta/simd.hpp>

/**
 * @brief A merged block's processOne resolves for both a scalar and a SIMD argument pack.
 *
 * The merged return type is `meta::simdize<ReturnType, detail::simdWidth<Ts...>()>`, so naming it is what instantiates
 * the width helper. Nothing else in this repository instantiates the helper's SIMD branch: the example in
 * `core/src/main.cpp` merges scalar blocks, and that file is not built. A fault confined to that branch therefore
 * reaches no compiler until a downstream project merges SIMD-capable blocks.
 *
 * The checks are compile-time only. The blocks are defined here because gnuradio4-core carries no standard block
 * library, so a core test may not depend on one.
 */

namespace qa_block_merging {

using namespace gr;

template<typename T, T kScale, typename R = decltype(std::declval<T>() * std::declval<T>())>
struct Scale : Block<Scale<T, kScale, R>> {
    PortIn<T>  original;
    PortOut<R> scaled;

    GR_MAKE_REFLECTABLE(Scale, original, scaled);

    template<meta::t_or_simd<T> V>
    [[nodiscard]] constexpr auto processOne(V value) const noexcept {
        return value * kScale;
    }
};

template<typename T, typename R = decltype(std::declval<T>() + std::declval<T>())>
struct Adder : Block<Adder<T>> {
    PortIn<T>  addend0;
    PortIn<T>  addend1;
    PortOut<R> sum;

    GR_MAKE_REFLECTABLE(Adder, addend0, addend1, sum);

    template<meta::t_or_simd<T> V>
    [[nodiscard]] constexpr auto processOne(V lhs, V rhs) const noexcept {
        return lhs + rhs;
    }
};

using Merged = decltype(MergeByIndex<Scale<int, -1>, 0, MergeByIndex<Scale<int, 2>, 0, Adder<int>, 0>, 0>());
using Simd   = meta::simdize<int>;

using SimdResult = decltype(std::declval<const Merged&>().processOne(std::declval<Simd>(), std::declval<Simd>()));
static_assert(meta::any_simd<SimdResult>, "a SIMD argument pack keeps the result a SIMD vector");
static_assert(SimdResult::size() == Simd::size(), "the merged width is the width the inputs share");

using ScalarResult = decltype(std::declval<const Merged&>().processOne(0, 0));
static_assert(std::is_same_v<ScalarResult, int>, "a scalar argument pack keeps the result scalar");

} // namespace qa_block_merging

int main() { /* tests are statically executed */ }
