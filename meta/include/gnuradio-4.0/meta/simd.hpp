#ifndef GNURADIO_META_SIMD_HPP
#define GNURADIO_META_SIMD_HPP

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <vir/simd.h>
#include <vir/simdize.h>
#pragma GCC diagnostic pop

namespace gr::meta {

namespace stdx = vir::stdx;

template<typename V, typename T = void>
concept any_simd = stdx::is_simd_v<V> && (std::same_as<T, void> || std::same_as<T, typename V::value_type>);

template<typename V, typename T>
concept t_or_simd = std::same_as<V, T> || any_simd<V, T>;

template<typename T, typename U = void>
concept constexpr_value = vir::constexpr_value<T, U>;

template<auto V>
inline constexpr vir::constexpr_wrapper<V> cw{};

namespace detail {

// vir::simdize decides whether a class can be vectorized by searching for its aggregate arity:
// it brace-initialises the type at candidate member counts over [0, sizeof(T) * CHAR_BIT] until
// one succeeds. For a type it cannot vectorize the search still runs to completion, and its cost
// grows with the type's size -- a record type of a dozen container-typed fields costs gigabytes
// of compiler memory to report that it cannot. Nothing outside a trivially copyable aggregate, or a
// tuple-like of such types, survives that search, so refusing the rest here costs two type traits
// and saves the search.
//
// This is a cheap prefilter, not a decision about what vir can vectorize. A type that passes is
// still handed to vir, which makes the structural decision and may report a compile error rather
// than declining: an aggregate with a std::array<std::byte, N> member fails inside
// vir::struct_get. That is a property of vir-simd rather than of this predicate. Before the
// prefilter existed, every type reached vir::simdize unconditionally and the same shapes failed
// the same way. meta/test/qa_simd.cpp pins the resolution of each shape.
//
// vectorizable_shape is the customization point, and the only way past this predicate: specializing
// it to std::true_type for a type admits that type to vir, which is what a type needs when it wants
// structural SIMD in a shape the set below does not name, and what any type needs whose shape a
// later vir-simd learns to vectorize before the set below is widened to match. Nothing here decides
// what vir can do; it decides only what is worth asking vir about, so the escape hatch has to stay.
template<typename T>
struct vectorizable_shape : std::bool_constant<std::is_aggregate_v<T> && std::is_trivially_copyable_v<T>> {};

template<typename T>
requires std::is_arithmetic_v<T>
struct vectorizable_shape<T> : std::true_type {};

template<typename... Ts>
struct vectorizable_shape<std::tuple<Ts...>> : std::bool_constant<(vectorizable_shape<std::remove_cvref_t<Ts>>::value && ...)> {};

template<typename TFirst, typename TSecond>
struct vectorizable_shape<std::pair<TFirst, TSecond>> : std::bool_constant<vectorizable_shape<std::remove_cvref_t<TFirst>>::value && vectorizable_shape<std::remove_cvref_t<TSecond>>::value> {};

template<typename T, int N, bool = vectorizable_shape<T>::value>
struct simdize_if_vectorizable {
    using type = T;
};

template<typename T, int N>
struct simdize_if_vectorizable<T, N, true> {
    using type = vir::simdize<T, N>;
};

} // namespace detail

template<typename T, int N = 0>
using simdize = typename detail::simdize_if_vectorizable<T, N>::type;

} // namespace gr::meta

#endif // GNURADIO_META_SIMD_HPP
