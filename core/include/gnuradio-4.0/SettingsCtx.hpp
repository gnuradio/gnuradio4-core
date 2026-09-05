#ifndef GNURADIO_SETTINGS_CTX_HPP
#define GNURADIO_SETTINGS_CTX_HPP

#include <gnuradio-4.0/Tag.hpp>

#include <compare>
#include <cstdint>
#include <functional>
#include <string>

namespace gr {

namespace settings {

// out-of-line so that this header stays independent of gr::exception and the message plane
[[noreturn]] void throwInvalidContextType(const pmt::Value& value);

inline std::strong_ordering comparePmt(const pmt::Value& lhs, const pmt::Value& rhs) {
    // If the types are different, cast rhs to the type of lhs and compare
    if (lhs.container_type() != rhs.container_type()) {
        // TODO: throw if types are not the same?
        return lhs.container_type() <=> rhs.container_type();
    } else if (lhs.value_type() != rhs.value_type()) {
        return lhs.value_type() <=> rhs.value_type();
    } else {
        if (lhs.holds<std::string_view>()) {
            return lhs.value_or(std::string_view{}) <=> rhs.value_or(std::string_view{});
        } else if (lhs.holds<int>()) {
            return lhs.value_or(0) <=> rhs.value_or(0);
        } else {
            throwInvalidContextType(lhs);
        }
    }
}

// pmt::Value comparison is needed to use it as a key of std::map
struct PMTCompare {
    bool operator()(const pmt::Value& lhs, const pmt::Value& rhs) const { return comparePmt(lhs, rhs) == std::strong_ordering::less; }
};

} // namespace settings

namespace detail {

std::size_t computeHash(const pmt::Value& value);

template<class T>
constexpr std::size_t hash_combine(std::size_t seed, const T& v) noexcept {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9UZ + (seed << 6) + (seed >> 2);
    return seed;
}

inline const auto computeValueHash = meta::overloaded([](const std::string_view& sv) { return std::hash<std::string_view>()(sv); }, //
    []<typename T>(const gr::Tensor<T>& tensor) {
        std::size_t seed = 9UZ;
        for (const auto& v : tensor) {
            seed = detail::hash_combine(seed, computeHash(pmt::Value(v)));
        }
        return seed;
    }, //
    [](const gr::property_map& map) {
        std::size_t seed = 0UZ;
        for (const auto& [k, v] : map) {
            std::size_t kv_seed = std::hash<std::string_view>()(k);
            seed                = detail::hash_combine(kv_seed, computeHash(v));
            seed                = detail::hash_combine(seed, kv_seed);
        }
        return seed;
    }, //
    [](const std::monostate) {
        // arbitrary constant seed
        return 0x9e3779b9UZ;
    }, //
    []<typename VT>(const std::complex<VT>& v) {
        std::hash<VT> hasher;
        std::size_t   seed = hasher(v.real());
        seed ^= hasher(v.imag()) + 0x9e3779b9UZ + (seed << 6) + (seed >> 2);
        return seed;
    }, //
    []<typename T>(const T& v) {
        if constexpr (gr::meta::complex_like<std::remove_cvref_t<T>>) {
            using value_t           = typename T::value_type;
            std::size_t        seed = std::hash<value_t>()(v.real());
            std::hash<value_t> hasher;
            seed ^= hasher(v.imag()) + 0x9e3779b9UZ + (seed << 6) + (seed >> 2);
            return seed;
        } else {
            return std::hash<T>()(v);
        }
    });

inline std::size_t computeHash(const pmt::Value& value) {
    std::size_t result = 0UZ;
    pmt::ValueVisitor([&result](const auto& v) { result = computeValueHash(v); }).visit(value);
    return result;
}

} // namespace detail

struct SettingsCtx {
    std::uint64_t time    = 0ULL;          // UTC-based time-stamp in ns, time from which the setting is valid, 0U is undefined time
    pmt::Value    context = std::string(); // user-defined multiplexing context for which the setting is valid

    bool operator==(const SettingsCtx&) const = default;

    std::partial_ordering operator<=>(const SettingsCtx& other) const {
        // First compare time
        if (auto cmp = time <=> other.time; cmp != std::strong_ordering::equal) {
            return cmp;
        }
        // Then compare context
        return settings::comparePmt(context, other.context);
    }

    [[nodiscard]] std::size_t hash() const noexcept { return detail::hash_combine(std::hash<std::uint64_t>()(time), detail::computeHash(context)); }
};

} // namespace gr

#endif // GNURADIO_SETTINGS_CTX_HPP
