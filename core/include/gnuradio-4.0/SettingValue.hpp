#ifndef GNURADIO_SETTING_VALUE_HPP
#define GNURADIO_SETTING_VALUE_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gnuradio-4.0/Exception.hpp>
#include <gnuradio-4.0/Export.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Tensor.hpp>
#include <gnuradio-4.0/Value.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

namespace gr::settings {

// one entry of a refusal: the call was refused and no key of it took effect, or a read named an undeclared key
struct Rejection {
    std::string key; // empty for an entry that names no key, such as a faulty description
    std::string reason;

    bool operator==(const Rejection&) const = default;
};

using Rejected = std::vector<Rejection>;

// the entries as one text, `key, key: reason` for the keys that share a reason, or the reason alone for an entry that
// names no key
[[nodiscard]] inline std::string describeRejected(const Rejected& rejected) {
    std::vector<std::pair<std::string, std::string>> byReason; // the keys of one reason, in the order the reasons come
    for (const Rejection& entry : rejected) {
        const auto same = std::ranges::find(byReason, entry.reason, &std::pair<std::string, std::string>::second);
        if (same == byReason.end()) {
            byReason.emplace_back(entry.key, entry.reason);
        } else if (!entry.key.empty()) {
            same->first += same->first.empty() ? entry.key : ", " + entry.key;
        }
    }
    std::string text;
    for (const auto& [keys, reason] : byReason) {
        text += std::format("{}{}{}{}", text.empty() ? "" : "; ", keys, keys.empty() ? "" : ": ", reason);
    }
    return text;
}

// the refusal of settings where no result carries one: a constructor's, or the loader's while it builds a graph. The
// message names the refused entries, which `rejected` holds, and `block` names the block or scheduler that refused
// them. Its destructor is defined in gnuradio-shared-state, as that of gr::exception, so that a catch in one shared
// object recognizes the type thrown in another
struct GNURADIO_EXPORT RefusedSettings : gr::exception {
    Rejected    rejected;
    std::string block;                        // empty when the thrower names no block
    bool        underNestedScheduler = false; // set by the level of a nested scheduler: no outer level prefixes `block`

    RefusedSettings(std::string_view msg, Rejected entries, std::string_view blockName = {}, std::source_location location = std::source_location::current()) : gr::exception(msg, location), rejected(std::move(entries)), block(blockName) {}
    RefusedSettings(const RefusedSettings&)            = default;
    RefusedSettings(RefusedSettings&&)                 = default;
    RefusedSettings& operator=(const RefusedSettings&) = default;
    RefusedSettings& operator=(RefusedSettings&&)      = default;
    ~RefusedSettings() override;
};

// the causes of a call's pending value; where several apply, the first is given. A `noWait` call, or one on a scheduler's
// worker thread, waits for no apply, since the apply may need that thread. It names the work call, or the queue when
// another thread held the block's settings; the next set, read, save, start or apply, or a tag the block reads, checks
// the queued keys. A set() on a running scheduler returns each of its keys pending for its next start and waits for no
// apply. A read's pending value gives no cause
inline constexpr std::string_view kPendingNotRunning = "not running";
inline constexpr std::string_view kPendingOnWorker   = "applies at the block's next work call";
inline constexpr std::string_view kPendingQueued     = "queued while the block's settings are busy";
inline constexpr std::string_view kPendingTimeout    = "timeout";
inline constexpr std::string_view kPendingNextStart  = "applies at the scheduler's next start";

// a call's timeout: `noWait` and any negative duration do not wait, and no timeout given waits the scheduler's
// default, else `kDefaultTimeout`
inline constexpr std::chrono::milliseconds noWait{0};
inline constexpr std::chrono::milliseconds kDefaultTimeout{1000};
using Timeout = std::optional<std::chrono::milliseconds>;

namespace detail {

template<typename T>
inline constexpr bool kIsComplex = false;
template<typename T>
inline constexpr bool kIsComplex<std::complex<T>> = true;

template<typename T>
concept SettingScalar = std::same_as<T, bool> || std::integral<T> || std::floating_point<T> || kIsComplex<T>;

// two values are the same when equal or both not-a-number, part by part for a complex number and element by element
// for a list of numbers
template<typename T>
[[nodiscard]] constexpr bool sameNumber(const T& left, const T& right) {
    if constexpr (std::floating_point<T>) {
        return left == right || (std::isnan(left) && std::isnan(right));
    } else if constexpr (kIsComplex<T>) {
        return sameNumber(left.real(), right.real()) && sameNumber(left.imag(), right.imag());
    } else if constexpr (requires { requires std::floating_point<std::ranges::range_value_t<T>> || kIsComplex<std::ranges::range_value_t<T>>; }) {
        return std::ranges::equal(left, right, [](const auto& one, const auto& two) { return sameNumber(one, two); });
    } else {
        return left == right;
    }
}

// the reason `nearestNumber` gives for a number outside T, which the settings word with T's name
inline constexpr std::string_view kOutsideType = "out of range for the type";

// whether a finite floating-point value lies within the range of the integer type T
template<std::integral T, std::floating_point F>
[[nodiscard]] bool fitsIntegral(F value) noexcept {
    if (!std::isfinite(value)) {
        return false;
    }
    const F upper = std::ldexp(F{1}, std::numeric_limits<T>::digits);
    const F lower = std::is_signed_v<T> ? -upper : F{0};
    return value >= lower && value < upper;
}

// a number as the nearest value of a floating-point T, or as an integer T when it is whole; either within T
template<typename T, typename S>
[[nodiscard]] std::expected<T, std::string_view> nearestNumber(const S& source) noexcept {
    if constexpr (std::same_as<T, S>) {
        return source;
    } else if constexpr (std::same_as<S, bool> || std::same_as<T, bool> || !SettingScalar<S> || !SettingScalar<T> || kIsComplex<S> || kIsComplex<T>) {
        return std::unexpected(std::string_view{});
    } else if constexpr (std::floating_point<T> && std::integral<S>) {
        return static_cast<T>(source);
    } else if constexpr (std::floating_point<T>) {
        if constexpr (std::numeric_limits<S>::max_exponent > std::numeric_limits<T>::max_exponent) { // a source with a wider exponent range than T can exceed T's maximum
            if (std::isfinite(source) && std::fabs(source) > static_cast<S>(std::numeric_limits<T>::max())) {
                return std::unexpected(kOutsideType);
            }
        }
        return static_cast<T>(source);
    } else if constexpr (std::integral<S>) {
        if (!std::in_range<T>(source)) {
            return std::unexpected(kOutsideType);
        }
        return static_cast<T>(source);
    } else {
        if (std::trunc(source) != source) {
            return std::unexpected(std::string_view("not a whole number"));
        }
        if (!fitsIntegral<T>(source)) {
            return std::unexpected(kOutsideType);
        }
        return static_cast<T>(source);
    }
}

// the source as a T when T holds it exactly; a NaN stays a NaN, a bool and a complex value convert only to their own kind
template<typename T, typename S>
[[nodiscard]] std::optional<T> exactCast(const S& source) noexcept {
    if constexpr (std::same_as<T, S>) {
        return source;
    } else if constexpr (kIsComplex<T> && kIsComplex<S>) {
        const std::optional<typename T::value_type> real = exactCast<typename T::value_type>(source.real());
        const std::optional<typename T::value_type> imag = exactCast<typename T::value_type>(source.imag());
        return real && imag ? std::optional<T>(T{*real, *imag}) : std::nullopt;
    } else if constexpr (std::convertible_to<const S&, std::string_view> && std::constructible_from<T, std::string_view>) { // a text of another allocator
        return T(std::string_view(source));
    } else if constexpr (!SettingScalar<S> || !SettingScalar<T> || kIsComplex<S> || kIsComplex<T> || std::same_as<S, bool> || std::same_as<T, bool>) {
        return std::nullopt;
    } else {
        const std::expected<T, std::string_view> nearest = nearestNumber<T>(source);
        if (!nearest) {
            return std::nullopt;
        }
        if constexpr (std::floating_point<S>) {
            if (std::isnan(source)) {
                return *nearest;
            }
        }
        if constexpr (std::integral<S> && std::floating_point<T>) {
            if (!fitsIntegral<S>(*nearest)) { // casting back is undefined outside the integer's range
                return std::nullopt;
            }
        }
        return static_cast<S>(*nearest) == source ? std::optional<T>(*nearest) : std::nullopt;
    }
}

// calls `visitor` with the stored number, bool or complex value, or with `std::monostate`
template<typename TVisitor>
decltype(auto) visitStoredScalar(const pmt::Value& value, TVisitor&& visitor) {
    using enum pmt::Value::ValueType;
    if (value.container_type() == pmt::Value::ContainerType::Scalar || value.container_type() == pmt::Value::ContainerType::Complex) {
        switch (value.value_type()) {
        case Bool: return visitor(*value.get_if<bool>());
        case Int8: return visitor(*value.get_if<std::int8_t>());
        case Int16: return visitor(*value.get_if<std::int16_t>());
        case Int32: return visitor(*value.get_if<std::int32_t>());
        case Int64: return visitor(*value.get_if<std::int64_t>());
        case UInt8: return visitor(*value.get_if<std::uint8_t>());
        case UInt16: return visitor(*value.get_if<std::uint16_t>());
        case UInt32: return visitor(*value.get_if<std::uint32_t>());
        case UInt64: return visitor(*value.get_if<std::uint64_t>());
        case Float32: return visitor(*value.get_if<float>());
        case Float64: return visitor(*value.get_if<double>());
        case ComplexFloat32: return visitor(*value.get_if<std::complex<float>>());
        case ComplexFloat64: return visitor(*value.get_if<std::complex<double>>());
        default: break;
        }
    }
    return visitor(std::monostate{});
}

// calls `visitor` with the stored tensor, or with `std::monostate`; a list of texts is a `Tensor<pmt::Value>`
template<typename TVisitor>
decltype(auto) visitStoredTensor(const pmt::Value& value, TVisitor&& visitor) {
    using enum pmt::Value::ValueType;
    if (value.is_tensor()) {
        switch (value.value_type()) {
        case Bool: return visitor(*value.get_if<Tensor<bool>>());
        case Int8: return visitor(*value.get_if<Tensor<std::int8_t>>());
        case Int16: return visitor(*value.get_if<Tensor<std::int16_t>>());
        case Int32: return visitor(*value.get_if<Tensor<std::int32_t>>());
        case Int64: return visitor(*value.get_if<Tensor<std::int64_t>>());
        case UInt8: return visitor(*value.get_if<Tensor<std::uint8_t>>());
        case UInt16: return visitor(*value.get_if<Tensor<std::uint16_t>>());
        case UInt32: return visitor(*value.get_if<Tensor<std::uint32_t>>());
        case UInt64: return visitor(*value.get_if<Tensor<std::uint64_t>>());
        case Float32: return visitor(*value.get_if<Tensor<float>>());
        case Float64: return visitor(*value.get_if<Tensor<double>>());
        case ComplexFloat32: return visitor(*value.get_if<Tensor<std::complex<float>>>());
        case ComplexFloat64: return visitor(*value.get_if<Tensor<std::complex<double>>>());
        case String: return visitor(*value.get_if<Tensor<std::pmr::string>>());
        case Value: return visitor(*value.get_if<Tensor<pmt::Value>>());
        default: break;
        }
    }
    return visitor(std::monostate{});
}

template<typename T>
inline constexpr bool kIsSequence = gr::meta::vector_type<T> || gr::meta::array_type<T> || is_tensor<T>;

template<typename T>
[[nodiscard]] std::optional<T> losslessAs(const pmt::Value& value);

template<typename T>
[[nodiscard]] std::optional<T> losslessSequenceAs(const pmt::Value& value) {
    using TElement                                = typename T::value_type;
    std::optional<std::vector<TElement>> elements = visitStoredTensor(value, []<typename TStored>(const TStored& stored) -> std::optional<std::vector<TElement>> {
        if constexpr (std::same_as<TStored, std::monostate>) {
            return std::nullopt;
        } else {
            std::vector<TElement> out;
            out.reserve(stored.size());
            for (const auto& element : stored) {
                std::optional<TElement> converted;
                if constexpr (std::same_as<typename TStored::value_type, pmt::Value>) {
                    converted = losslessAs<TElement>(element);
                } else {
                    converted = exactCast<TElement>(element);
                }
                if (!converted) {
                    return std::nullopt;
                }
                out.push_back(std::move(*converted));
            }
            return out;
        }
    });
    if (!elements) {
        return std::nullopt;
    }
    if constexpr (gr::meta::vector_type<T>) {
        return T(elements->begin(), elements->end());
    } else if constexpr (gr::meta::array_type<T>) {
        if (elements->size() != std::tuple_size_v<T>) {
            return std::nullopt;
        }
        T out{};
        std::ranges::move(*elements, out.begin());
        return out;
    } else {
        T out(extents_from, {elements->size()});
        std::ranges::move(*elements, out.begin());
        return out;
    }
}

// the value as a T when T holds it without loss; a text reads as a text or an enumerator's name, a list element by element
template<typename T>
[[nodiscard]] std::optional<T> losslessAs(const pmt::Value& value) {
    if constexpr (std::same_as<T, pmt::Value>) {
        return value;
    } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::pmr::string>) {
        return value.is_string() ? std::optional<T>(T(value.value_or(std::string_view{}))) : std::nullopt;
    } else if constexpr (std::is_enum_v<T>) {
        return value.is_string() ? gr::meta::parseEnum<T>(value.value_or(std::string_view{})) : std::nullopt;
    } else if constexpr (std::same_as<T, property_map>) {
        const property_map* map = value.get_if<property_map>();
        return map != nullptr ? std::optional<T>(*map) : std::nullopt;
    } else if constexpr (SettingScalar<T>) {
        return visitStoredScalar(value, [](const auto& stored) { return exactCast<T>(stored); });
    } else if constexpr (kIsSequence<T>) {
        return losslessSequenceAs<T>(value);
    } else {
        static_assert(gr::meta::always_false<T>, "a setting reads as a number, a bool, a complex number, a text, an enumeration, a map, a list of these or a pmt::Value");
    }
}

} // namespace detail

/**
 * @brief One entry of a settings result or read: a value, a pending value with its cause, or a failed key with its
 * reason.
 *
 * A value entry carries the block's reason when the block substituted the value in force; a recipe composite's
 * parameter carries its stored value, with the values its inner blocks run in `reason`.
 */
struct Value {
    enum class State : std::uint8_t { value, pending, failed };

    State       state = State::value;
    pmt::Value  value{};
    std::string reason{}; // pending: the cause; failed: the reason; value: the block's reason for a substitute

    [[nodiscard]] bool isValue() const noexcept { return state == State::value; }
    [[nodiscard]] bool isPending() const noexcept { return state == State::pending; }
    [[nodiscard]] bool isFailed() const noexcept { return state == State::failed; }

    // the value as T, refused unless T holds it without loss
    template<typename T>
    [[nodiscard]] std::expected<T, Rejected> as() const {
        const auto refuse = [](std::string why) { return std::unexpected(Rejected{Rejection{.key = {}, .reason = std::move(why)}}); };
        switch (state) {
        case State::pending:
        case State::failed: return refuse(reason);
        case State::value: break;
        }
        if (value.is_monostate()) {
            return refuse(reason.empty() ? std::string("no value") : reason);
        }
        std::optional<T> converted = detail::losslessAs<T>(value);
        if (!converted) {
            return refuse(std::format("not convertible to {} without loss: {}", gr::meta::type_name<T>(), value));
        }
        return std::move(*converted);
    }

    bool operator==(const Value&) const = default;
};

using Values = std::map<std::string, Value, std::less<>>;

// each key of a read that holds a value, at that value; empty for a refused read
[[nodiscard]] property_map heldValues(const std::expected<Values, Rejected>& read);

namespace detail {
// each failed key of a result with its reason, for a caller that takes a failed key as an error of the whole call
[[nodiscard]] Rejected failedKeys(const Values& values);
} // namespace detail

} // namespace gr::settings

#endif // GNURADIO_SETTING_VALUE_HPP
