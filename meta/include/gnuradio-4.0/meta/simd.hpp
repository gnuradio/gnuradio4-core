#ifndef GNURADIO_META_SIMD_HPP
#define GNURADIO_META_SIMD_HPP

#include <concepts>
#include <type_traits>

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

template<typename T, int N = 0>
using simdize = vir::simdize<T, N>;

} // namespace gr::meta

#endif // GNURADIO_META_SIMD_HPP
