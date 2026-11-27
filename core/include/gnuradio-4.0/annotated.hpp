#ifndef GNURADIO_ANNOTATED_HPP
#define GNURADIO_ANNOTATED_HPP

#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <memory_resource>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/meta/formatter.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

namespace gr {

/**
 * @brief a template wrapping structure, which holds a static documentation (e.g. mark down) string as its value.
 * It's used as a trait class to annotate other template classes (e.g. blocks or fields).
 */
template<gr::meta::fixed_string doc_string>
struct Doc {
    static constexpr gr::meta::fixed_string value = doc_string;
};

using EmptyDoc = Doc<"">; // nomen-est-omen

template<typename T>
struct is_doc : std::false_type {};

template<gr::meta::fixed_string N>
struct is_doc<Doc<N>> : std::true_type {};

template<typename T>
concept Documentation = is_doc<T>::value;

/**
 * @brief Unit is a template structure, which holds a static physical-unit (i.e. SI unit) string as its value.
 * It's used as a trait class to annotate other template classes (e.g. blocks or fields).
 */
template<gr::meta::fixed_string doc_string>
struct Unit {
    static constexpr gr::meta::fixed_string value = doc_string;
};

using EmptyUnit = Unit<"">; // nomen-est-omen

template<typename T>
struct is_unit : std::false_type {};

template<gr::meta::fixed_string N>
struct is_unit<Unit<N>> : std::true_type {};

template<typename T>
concept UnitType = is_unit<T>::value;

static_assert(Documentation<EmptyDoc>);
static_assert(UnitType<EmptyUnit>);
static_assert(!UnitType<EmptyDoc>);
static_assert(!Documentation<EmptyUnit>);

/**
 * @brief Annotates field etc. that the entity is visible from a UI perspective.
 */
struct Visible {};

/**
 * @brief Annotates a setting the block takes only while the graph is stopped: its settings refuse a change of it while
 * the graph runs or is paused. A setting without it applies while the block runs.
 */
struct StoppedOnly {};

/**
 * @brief Annotates an integer setting as the port count of one or more dynamic port collections of its block.
 *
 * The block resizes each named collection to the setting's value in its settings hook; the setting changes only while
 * the graph is stopped.
 */
template<gr::meta::fixed_string... collections>
requires(sizeof...(collections) > 0UZ)
struct PortCountFor {
    static constexpr std::array<std::string_view, sizeof...(collections)> value{std::string_view(collections)...};
};

template<typename T>
struct is_port_count_for : std::false_type {};

template<gr::meta::fixed_string... collections>
struct is_port_count_for<PortCountFor<collections...>> : std::true_type {};

/**
 * @brief Disable the default tag forwarder.
 *
 * The default forwarder passes every key of every input tag to every synchronous output. On a block with only
 * synchronous stream ports and without `Resampling<>` or `Stride<>`, a tag leaves at the offset it arrived at. On a
 * block with `Stride<>` or an asynchronous stream port, the framework ends a call at the next tag, and the tag leaves
 * on the call's first output sample. There, a tag inside a chunk that an input `min_samples` keeps whole leaves on the
 * first output sample of a later call. A block whose output samples do not stand where its input samples stood declares
 * `NoTagPropagation` and publishes its own tags, or supplies `forwardTags()`. A delay, a block that drops samples
 * without declaring `Resampling<>` and a block that reframes its input are such blocks.
 */
struct NoTagPropagation {};

/**
 * @brief Every tag of a chunk leaves at the chunk's first output sample.
 *
 * Without it, the default forwarder of a block declaring `Resampling<>` places a tag at input index `i` of the samples a
 * call consumed on output index `floor(i * output_chunk_size / input_chunk_size)`, after the call. A block whose
 * output describes its whole input chunk, a spectrum or a DataSet for instance, declares `BackwardTagPropagation`.
 */
struct BackwardTagPropagation {};

/**
 * @brief Drop the listed tag keys in the default forwarder and forward every other key.
 *
 * A block lists the keys whose values do not describe its output. A block that shifts the carrier frequency, for
 * instance, lists the keys that state a frequency estimate. A listed key is dropped at the top level of a tag and
 * inside the map its `gr:trigger_meta_info` key holds. A `forwardTags()` override publishes what it writes. The list
 * reaches it only through `filterAndSubstituteTag()`. A block composed at compile time drops the keys of every member.
 * A block declares at most one list.
 *
 * @tparam Keys the dropped keys
 */
template<gr::meta::fixed_string... Keys>
struct DroppedTagKeys {
    static constexpr std::array<std::string_view, sizeof...(Keys)> kKeys{std::string_view(Keys)...};
};

template<typename T>
struct is_dropped_tag_keys : std::false_type {};

template<gr::meta::fixed_string... Keys>
struct is_dropped_tag_keys<DroppedTagKeys<Keys...>> : std::true_type {};

/**
 * @brief Annotates block, indicating to perform resampling based on the provided `inputChunkSize` and `outputChunkSize`.
 * For each `inputChunkSize` input samples, `outputChunkSize` output samples are published.
 * Thus the total number of input/output samples can be calculated as `nInput = k * inputChunkSize` and `nOutput = k * outputChunkSize`.
 * They also act as constraints for the minimum number of input samples (`inputChunkSize`)  and output samples (`outputChunkSize`).
 *
 * │inputChunkSize│...│inputChunkSize│ ───► │outputChunkSize│...│outputChunkSize│
 * └──────────────┘   └──────────────┘      └───────────────┘   └───────────────┘
 *    nInputs = k * inputChunkSize              nOutputs = k * outputChunkSize
 *
 * The comparison between `inputChunkSize` and `outputChunkSize` determines whether to perform interpolation or decimation.
 * - If `inputChunkSize` > `outputChunkSize`, decimation occurs.
 * - If `inputChunkSize` < `outputChunkSize`, interpolation occurs.
 * - If `inputChunkSize` == `outputChunkSize`, there is no effect on the sampling rate.
 *
 * @tparam inputChunkSize input chunk size.
 * @tparam outputChunkSize output chunk size.
 * @tparam isConst Specifies if the resampling is constant or can be modified during run-time.
 */
template<gr::Size_t inputChunkSize = 1U, gr::Size_t outputChunkSize = 1U, bool isConst = false>
struct Resampling {
    static_assert(outputChunkSize > 0, "outputChunkSize in ResamplingRatio must be >= 0");
    static constexpr gr::Size_t kInputChunkSize  = inputChunkSize;
    static constexpr gr::Size_t kOutputChunkSize = outputChunkSize;
    static constexpr bool       kIsConst         = isConst;
    static constexpr bool       kEnabled         = !isConst || (kOutputChunkSize != 1LU) || (kInputChunkSize != 1LU);
};

template<typename T>
concept IsResampling = requires {
    T::kInputChunkSize;
    T::kOutputChunkSize;
    T::kIsConst;
    T::kEnabled;
} && std::is_base_of_v<Resampling<T::kInputChunkSize, T::kOutputChunkSize, T::kIsConst>, T>;

template<typename T>
using is_resampling = std::bool_constant<IsResampling<T>>;

static_assert(is_resampling<Resampling<1024, 1>>::value);
static_assert(!is_resampling<int>::value);

/**
 * @brief Annotates block, indicating the stride control for data processing.
 *
 * Stride determines the number of samples between consecutive data processing events:
 * - If stride is less than N, it indicates overlap.
 * - If stride is greater than N, it indicates skipped samples.
 * - If stride is equal to 0, it indicates back-to-back processing without skipping.
 *
 * @tparam stride The number of samples between data processing events.
 * @tparam isConst Specifies if the stride is constant or can be modified during run-time.
 */
template<std::uint64_t stride = 0U, bool isConst = false>
struct Stride {
    static_assert(stride >= 0U, "Stride must be >= 0");

    static constexpr gr::Size_t kStride  = stride;
    static constexpr bool       kIsConst = isConst;
    static constexpr bool       kEnabled = !isConst || (stride > 0U);
};

template<typename T>
concept IsStride = requires {
    T::kStride;
    T::kIsConst;
    T::kEnabled;
} && std::is_base_of_v<Stride<T::kStride, T::kIsConst>, T>;

template<typename T>
using is_stride = std::bool_constant<IsStride<T>>;

static_assert(is_stride<Stride<10, true>>::value);
static_assert(!is_stride<int>::value);

enum class IncompleteFinalUpdateEnum { DROP, PULL_FORWARD, PUSH_BACKWARD };

template<IncompleteFinalUpdateEnum updatePolicy>
struct IncompleteFinalUpdatePolicy {
    static constexpr IncompleteFinalUpdateEnum kIncompleteFinalUpdatePolicy = updatePolicy;
};

template<typename T>
concept IsIncompleteFinalUpdatePolicy = requires { T::kIncompleteFinalUpdatePolicy; } && std::is_base_of_v<IncompleteFinalUpdatePolicy<T::kIncompleteFinalUpdatePolicy>, T>;

template<typename T>
using is_incompleteFinalUpdatePolicy = std::bool_constant<IsIncompleteFinalUpdatePolicy<T>>;

static_assert(is_incompleteFinalUpdatePolicy<IncompleteFinalUpdatePolicy<IncompleteFinalUpdateEnum::DROP>>::value);

enum class UICategory {
    None,        /// No UI contribution (default).
    MenuBar,     /// Global app menu bar items (File/Edit/View…).
    Toolbar,     /// Compact, frequently used actions/toggles.
    StatusBar,   /// Always-visible, low-interaction status readouts.
    Content,     /// Primary viewport output (plots/canvases/dashboards).
    Panel,       /// Secondary panels (inspectors, settings, lists, logs).
    Overlay,     /// Layered HUD over Content (cursors/markers/annotations).
    ContextMenu, /// Right-click / long-press contextual popup menus.
    Dialog,      /// Modal/semi-modal short workflows (export/config/confirm).
    Notification /// Transient non-modal feedback (toast/banner).
};

} // namespace gr

// Compile-time performance override; phased out with C++26 reflection.
namespace gr::meta::detail {
template<>
struct EnumTraits<gr::UICategory> {
    static constexpr std::array<std::pair<gr::UICategory, std::string_view>, 10> entries = {{
        {gr::UICategory::None, "None"},
        {gr::UICategory::MenuBar, "MenuBar"},
        {gr::UICategory::Toolbar, "Toolbar"},
        {gr::UICategory::StatusBar, "StatusBar"},
        {gr::UICategory::Content, "Content"},
        {gr::UICategory::Panel, "Panel"},
        {gr::UICategory::Overlay, "Overlay"},
        {gr::UICategory::ContextMenu, "ContextMenu"},
        {gr::UICategory::Dialog, "Dialog"},
        {gr::UICategory::Notification, "Notification"},
    }};
};
} // namespace gr::meta::detail

namespace gr {

/**
 * @brief Annotates block, indicating that it is drawable and provides a  mandatory `void draw()` method.
 *
 * @tparam category_ ui category where it
 * @tparam toolkit_ specifies the applicable UI toolkit (e.g. 'console', 'ImGui', 'Qt', etc.)
 */
template<UICategory category_, gr::meta::fixed_string toolkit_ = "">
struct Drawable {
    static constexpr UICategory             kCategory = category_;
    static constexpr gr::meta::fixed_string kToolkit  = toolkit_;
};

template<typename T>
concept IsDrawable = requires {
    T::kCategory;
    T::kToolkit;
} && std::is_base_of_v<Drawable<T::kCategory, T::kToolkit>, T>;

template<typename T>
using is_drawable = std::bool_constant<IsDrawable<T>>;

using NotDrawable = Drawable<UICategory::None, "">; // nomen-est-omen
static_assert(is_drawable<NotDrawable>::value);
static_assert(is_drawable<Drawable<UICategory::Content, "console">>::value);
static_assert(!is_drawable<int>::value);

/**
 * @brief Annotates templated block, indicating which port data types are supported.
 */
template<typename... Ts>
struct SupportedTypes {};

template<typename T>
struct is_supported_types : std::false_type {};

template<typename... Ts>
struct is_supported_types<SupportedTypes<Ts...>> : std::true_type {};

using DefaultSupportedTypes = SupportedTypes<>;

static_assert(gr::meta::is_instantiation_of<DefaultSupportedTypes, SupportedTypes>);
static_assert(gr::meta::is_instantiation_of<SupportedTypes<float, double>, SupportedTypes>);

namespace detail {
template<typename T>
concept LimitNumber = std::is_arithmetic_v<T> && !std::same_as<T, bool>;

// the character types, which std::cmp_less and the other std::cmp_* functions refuse
template<typename T>
concept CharacterType = std::same_as<T, char> || std::same_as<T, wchar_t> || std::same_as<T, char8_t> || std::same_as<T, char16_t> || std::same_as<T, char32_t>;

// a character as its promoted integer type, which the std::cmp_* functions take; any other integer unchanged
template<std::integral T>
[[nodiscard]] constexpr auto comparableInteger(T value) noexcept {
    if constexpr (CharacterType<T>) {
        return +value;
    } else {
        return value;
    }
}

template<typename To, typename From>
[[nodiscard]] constexpr To castUnlessSame(From value) noexcept {
    if constexpr (std::same_as<To, From>) {
        return value;
    } else {
        return static_cast<To>(value);
    }
}

// 2^digits of Integer, one above its maximum: a power of two that Floating holds exactly
template<std::floating_point Floating, std::integral Integer>
inline constexpr Floating kTwoToDigits = castUnlessSame<Floating>(std::numeric_limits<Integer>::max() / 2 + 1) * Floating{2};

// true when value lies in [lower, upper]; a pair with integral bounds compares exactly, any other pair in the common type
template<LimitNumber Source, LimitNumber Bound>
[[nodiscard]] constexpr bool withinBounds(Source value, Bound lower, Bound upper) noexcept {
    if constexpr (std::is_integral_v<Source> && std::is_integral_v<Bound>) {
        return std::cmp_greater_equal(comparableInteger(value), comparableInteger(lower)) && std::cmp_less_equal(comparableInteger(value), comparableInteger(upper));
    } else if constexpr (std::is_floating_point_v<Source> && std::is_integral_v<Bound>) {
        // NaN and a value outside [-2^digits, 2^digits) of Bound lie outside any bound of that type
        // for an unsigned Bound, a negative value lies below any lower bound
        constexpr Source twoToDigits = kTwoToDigits<Source, Bound>;
        constexpr Source lowest      = std::is_signed_v<Bound> ? -twoToDigits : Source{0};
        if (!(value >= lowest && value < twoToDigits)) {
            return false;
        }
        // value >= lower exactly when floor(value) >= lower, and value <= upper exactly when ceil(value) <= upper
        // a ceiling of 2^digits lies above any upper bound
        const Source ceiling = std::ceil(value);
        return static_cast<Bound>(std::floor(value)) >= lower && ceiling < twoToDigits && static_cast<Bound>(ceiling) <= upper;
    } else {
        using Common        = std::common_type_t<Source, Bound>;
        const Common common = castUnlessSame<Common>(value);
        return common >= castUnlessSame<Common>(lower) && common <= castUnlessSame<Common>(upper);
    }
}

// true when static_cast<Target>(value) has defined behavior
template<LimitNumber Target, LimitNumber Source>
[[nodiscard]] constexpr bool castKeepsValue(Source value) noexcept {
    if constexpr (std::is_integral_v<Source> && std::is_integral_v<Target>) {
        return withinBounds(value, std::numeric_limits<Target>::lowest(), std::numeric_limits<Target>::max());
    } else if constexpr (std::is_integral_v<Source>) {
        return true;
    } else if constexpr (std::is_integral_v<Target>) {
        constexpr Source upper = kTwoToDigits<Source, Target>;
        if constexpr (std::is_signed_v<Target>) {
            return value >= -upper && value < upper;
        } else {
            return value > Source{-1} && value < upper;
        }
    } else {
        constexpr Source infinity = std::numeric_limits<Source>::infinity();
        return value == infinity || value == -infinity || !(value < castUnlessSame<Source>(std::numeric_limits<Target>::lowest()) || value > castUnlessSame<Source>(std::numeric_limits<Target>::max()));
    }
}
// value >= bound, or value > bound where kExcluded, compared exactly whatever the two number types; false for a NaN
template<bool kExcluded, typename TValue, typename TBound>
[[nodiscard]] constexpr bool aboveLowerEnd(const TValue& value, const TBound& bound) noexcept {
    if constexpr (std::is_integral_v<TValue> && std::is_integral_v<TBound> && LimitNumber<TValue> && LimitNumber<TBound>) {
        if constexpr (kExcluded) {
            return std::cmp_greater(comparableInteger(value), comparableInteger(bound));
        } else {
            return std::cmp_greater_equal(comparableInteger(value), comparableInteger(bound));
        }
    } else if constexpr (std::is_floating_point_v<TValue> && std::is_integral_v<TBound> && LimitNumber<TBound>) {
        constexpr TValue twoToDigits = kTwoToDigits<TValue, TBound>;
        constexpr TValue lowest      = std::is_signed_v<TBound> ? -twoToDigits : TValue{0};
        if (!(value >= lowest)) { // a NaN, or a value below every bound of TBound
            return false;
        }
        if (value >= twoToDigits) {
            return true;
        }
        // value >= bound exactly when floor(value) >= bound, and value > bound exactly when ceil(value) > bound; the value
        // truncated toward zero is its floor unless the value lies below it, and its ceil unless the value lies above it
        // (Clang with libstdc++ cannot evaluate std::floor or std::ceil in a constant expression)
        const TBound truncated = static_cast<TBound>(value);
        if constexpr (kExcluded) {
            return value > static_cast<TValue>(truncated) ? truncated >= bound : truncated > bound;
        } else {
            return value < static_cast<TValue>(truncated) ? truncated > bound : truncated >= bound;
        }
    } else if constexpr (LimitNumber<TValue> && LimitNumber<TBound>) {
        using Common = std::common_type_t<TValue, TBound>;
        if constexpr (kExcluded) {
            return castUnlessSame<Common>(value) > castUnlessSame<Common>(bound);
        } else {
            return castUnlessSame<Common>(value) >= castUnlessSame<Common>(bound);
        }
    } else if constexpr (kExcluded) {
        return value > bound;
    } else {
        return value >= bound;
    }
}

// value <= bound, or value < bound where kExcluded, compared exactly whatever the two number types; false for a NaN
template<bool kExcluded, typename TValue, typename TBound>
[[nodiscard]] constexpr bool belowUpperEnd(const TValue& value, const TBound& bound) noexcept {
    if constexpr (std::is_integral_v<TValue> && std::is_integral_v<TBound> && LimitNumber<TValue> && LimitNumber<TBound>) {
        if constexpr (kExcluded) {
            return std::cmp_less(comparableInteger(value), comparableInteger(bound));
        } else {
            return std::cmp_less_equal(comparableInteger(value), comparableInteger(bound));
        }
    } else if constexpr (std::is_floating_point_v<TValue> && std::is_integral_v<TBound> && LimitNumber<TBound>) {
        constexpr TValue twoToDigits = kTwoToDigits<TValue, TBound>;
        constexpr TValue lowest      = std::is_signed_v<TBound> ? -twoToDigits : TValue{0};
        if (!(value < twoToDigits)) { // a NaN, or a value above every bound of TBound
            return false;
        }
        if (value < lowest) {
            return true;
        }
        // value <= bound exactly when ceil(value) <= bound, and value < bound exactly when floor(value) < bound
        const TBound truncated = static_cast<TBound>(value);
        if constexpr (kExcluded) {
            return value < static_cast<TValue>(truncated) ? truncated <= bound : truncated < bound;
        } else {
            return value > static_cast<TValue>(truncated) ? truncated < bound : truncated <= bound;
        }
    } else if constexpr (LimitNumber<TValue> && LimitNumber<TBound>) {
        using Common = std::common_type_t<TValue, TBound>;
        if constexpr (kExcluded) {
            return castUnlessSame<Common>(value) < castUnlessSame<Common>(bound);
        } else {
            return castUnlessSame<Common>(value) <= castUnlessSame<Common>(bound);
        }
    } else if constexpr (kExcluded) {
        return value < bound;
    } else {
        return value <= bound;
    }
}

// an enumerator equals an enumerator of its own enumeration alone
template<typename TValue, typename TListed>
[[nodiscard]] constexpr bool equalsListed(const TValue& value, const TListed& listed) noexcept {
    if constexpr (std::is_enum_v<TValue> || std::is_enum_v<TListed>) {
        if constexpr (std::same_as<TValue, TListed>) {
            return value == listed;
        } else {
            return false;
        }
    } else {
        return aboveLowerEnd<false>(value, listed) && belowUpperEnd<false>(value, listed);
    }
}

// a limit's value as a host reads it: a number, never a character; a floating-point value in the shortest text that
// reads back the same value; an enumerator by its name
template<typename TValue>
[[nodiscard]] std::string limitValueText(TValue value) {
    if constexpr (std::is_enum_v<TValue>) {
        return std::string(gr::meta::enumName(value).value_or(std::string_view{}));
    } else if constexpr (std::is_integral_v<TValue>) {
        return std::format("{}", comparableInteger(value));
    } else {
        return std::format("{}", value);
    }
}

// whether a setting of type T holds `value` exactly; an enumeration holds its own enumerators alone, and a type other
// than a number or an enumeration holds any
template<typename T, typename TValue>
[[nodiscard]] constexpr bool typeHolds(const TValue& value) noexcept {
    if constexpr (std::is_enum_v<T> || std::is_enum_v<TValue>) {
        return std::same_as<T, TValue>;
    } else if constexpr (LimitNumber<T> && LimitNumber<TValue>) {
        return castKeepsValue<T>(value) && equalsListed(castUnlessSame<T>(value), value);
    } else {
        return true;
    }
}
} // namespace detail

enum class LimitEnd : std::uint8_t { none, lower, upper };

/**
 * @brief A predicate of `Limits` on one end of a value's range: `Min` and `Max` include the bound, `Over` and `Under`
 * exclude it. A NaN fails each of them.
 *
 * A predicate states its test, `holds`, and the text a host reads in the limits, `text`, in one place; `end` tells
 * the settings which end of the range it is, and `none` marks a predicate that is no end, as `OneOf`.
 */
template<LimitEnd kEnd, bool kExcluded, auto kBound>
struct EndPredicate {
    static_assert(detail::LimitNumber<decltype(kBound)>, "an end of Limits is a number");

    static constexpr LimitEnd end      = kEnd;
    static constexpr bool     excluded = kExcluded;
    static constexpr auto     bound    = kBound;

    template<typename T>
    [[nodiscard]] static constexpr bool holds(const T& value) noexcept {
        if constexpr (kEnd == LimitEnd::lower) {
            return detail::aboveLowerEnd<kExcluded>(value, kBound);
        } else {
            return detail::belowUpperEnd<kExcluded>(value, kBound);
        }
    }

    [[nodiscard]] static std::string text() {
        constexpr std::string_view relation = kEnd == LimitEnd::lower ? (kExcluded ? ">" : ">=") : (kExcluded ? "<" : "<=");
        return std::format("{} {}", relation, detail::limitValueText(kBound));
    }

    template<typename T>
    [[nodiscard]] static constexpr bool fitsIn() noexcept {
        return detail::typeHolds<T>(kBound);
    }
};

template<auto kBound>
inline constexpr EndPredicate<LimitEnd::lower, false, kBound> Min{};
template<auto kBound>
inline constexpr EndPredicate<LimitEnd::upper, false, kBound> Max{};
template<auto kBound>
inline constexpr EndPredicate<LimitEnd::lower, true, kBound> Over{};
template<auto kBound>
inline constexpr EndPredicate<LimitEnd::upper, true, kBound> Under{};

namespace detail {
template<auto... kValues>
[[nodiscard]] consteval bool listedOnce() {
    if constexpr (sizeof...(kValues) < 2UZ) {
        return true;
    } else {
        return []<auto kFirst, auto... kRest>() { return (!equalsListed(kFirst, kRest) && ...) && listedOnce<kRest...>(); }.template operator()<kValues...>();
    }
}

template<auto... kValues>
[[nodiscard]] consteval bool listsOneEnumeration() {
    if constexpr (sizeof...(kValues) == 0UZ) {
        return false;
    } else {
        return []<auto kFirst, auto... kRest>() { return std::is_enum_v<decltype(kFirst)> && (std::same_as<decltype(kFirst), decltype(kRest)> && ...); }.template operator()<kValues...>();
    }
}
} // namespace detail

/**
 * @brief The predicate of `Limits` that a value equals one of `kValues`: numbers, or enumerators of one enumeration,
 * for a setting that takes part of a shared enumeration. A NaN equals none.
 */
template<auto... kValues>
struct OneOfPredicate {
    static constexpr bool kEnumerators = detail::listsOneEnumeration<kValues...>();

    static_assert(sizeof...(kValues) > 0UZ, "OneOf lists at least one value");
    static_assert((detail::LimitNumber<decltype(kValues)> && ...) || kEnumerators, "OneOf lists numbers, or enumerators of one enumeration");
    static_assert(detail::listedOnce<kValues...>(), "OneOf lists each value once");

    static constexpr LimitEnd end = LimitEnd::none;

    template<typename T>
    [[nodiscard]] static constexpr bool holds(const T& value) noexcept {
        return (detail::equalsListed(value, kValues) || ...);
    }

    [[nodiscard]] static std::string text() {
        std::string listed;
        ((listed += std::format("{}{}", listed.empty() ? "" : ", ", detail::limitValueText(kValues))), ...);
        return std::format("one of: {}", listed);
    }

    template<typename T>
    [[nodiscard]] static constexpr bool fitsIn() noexcept {
        return (detail::typeHolds<T>(kValues) && ...);
    }

    // whether each listed value passes the predicate `TOther`; an end bounds a number, so no enumerator passes it
    template<typename TOther>
    [[nodiscard]] static constexpr bool listedPass() noexcept {
        if constexpr (kEnumerators && TOther::end != LimitEnd::none) {
            return false;
        } else {
            return (TOther::holds(kValues) && ...);
        }
    }

    template<typename T>
    [[nodiscard]] static std::vector<T> listedAs() {
        return {detail::castUnlessSame<T>(kValues)...};
    }
};

template<auto... kValues>
inline constexpr OneOfPredicate<kValues...> OneOf{};

namespace detail {

template<typename T>
concept LimitPredicate = requires {
    { T::end } -> std::convertible_to<LimitEnd>;
    { T::text() } -> std::convertible_to<std::string>;
};

// One template argument of `Limits`: a type, a value or a text. It takes a text so that `Limits` refuses it with its
// own message.
template<typename T>
struct LimitArgument {
    using Type = T;
    T value;

    explicit(false) constexpr LimitArgument(T argument) noexcept(std::is_nothrow_copy_constructible_v<T>) : value(argument) {}

    template<std::size_t N>
    requires std::same_as<T, gr::meta::fixed_string<N - 1UZ>>
    explicit(false) consteval LimitArgument(const char (&text)[N]) noexcept : value(text) {}
};

template<std::size_t N>
LimitArgument(const char (&)[N]) -> LimitArgument<gr::meta::fixed_string<N - 1UZ>>;

template<LimitArgument kArgument>
using LimitArgumentType = typename std::remove_cvref_t<decltype(kArgument)>::Type;

template<LimitArgument... kArguments>
inline constexpr bool kPredicatesAlone = (LimitPredicate<LimitArgumentType<kArguments>> && ...);

template<typename TLower, typename TUpper>
[[nodiscard]] consteval bool endsInOrder() {
    if constexpr (TLower::excluded || TUpper::excluded) {
        return belowUpperEnd<true>(TLower::bound, TUpper::bound);
    } else {
        return belowUpperEnd<false>(TLower::bound, TUpper::bound);
    }
}

// whether each lower end of the list lies below each upper end, or at it where both include it
template<typename... TPredicates>
requires(sizeof...(TPredicates) == 0UZ)
[[nodiscard]] consteval bool limitEndsInOrder() {
    return true;
}

template<typename... TPredicates>
[[nodiscard]] consteval bool limitEndsInOrder() {
    bool       inOrder = true;
    const auto against = [&inOrder]<typename TLower>() {
        if constexpr (TLower::end == LimitEnd::lower) {
            const auto pair = [&inOrder]<typename TUpper>() {
                if constexpr (TUpper::end == LimitEnd::upper) {
                    inOrder = inOrder && endsInOrder<TLower, TUpper>();
                }
            };
            (pair.template operator()<TPredicates>(), ...);
        }
    };
    (against.template operator()<TPredicates>(), ...);
    return inOrder;
}

template<typename T>
inline constexpr bool kIsOneOf = false;
template<auto... kValues>
inline constexpr bool kIsOneOf<OneOfPredicate<kValues...>> = true;

// whether each value a `OneOf` of the list names passes every other predicate of the list
template<typename... TPredicates>
requires(sizeof...(TPredicates) == 0UZ)
[[nodiscard]] consteval bool listedValuesPass() {
    return true;
}

template<typename... TPredicates>
[[nodiscard]] consteval bool listedValuesPass() {
    bool       pass  = true;
    const auto check = [&pass]<typename TListing>() {
        if constexpr (kIsOneOf<TListing>) {
            ((pass = pass && (std::same_as<TListing, TPredicates> || TListing::template listedPass<TPredicates>())), ...);
        }
    };
    (check.template operator()<TPredicates>(), ...);
    return pass;
}

// the arguments of `Limits` where one of them is no predicate: `Limits` refuses them, and they name no value
template<bool kPredicates, typename... TArguments>
struct PredicateLimits {
    using Predicates                   = gr::meta::typelist<>;
    static constexpr bool kOneOfAlone  = true;
    static constexpr bool kNoPredicate = true;

    template<typename T>
    [[nodiscard]] static constexpr bool fitsIn() noexcept {
        return true;
    }
};

template<typename... TPredicates>
struct PredicateLimits<true, TPredicates...> {
    using Predicates                   = gr::meta::typelist<TPredicates...>;
    static constexpr bool kOneOfAlone  = (kIsOneOf<TPredicates> && ...);
    static constexpr bool kNoPredicate = sizeof...(TPredicates) == 0UZ;

    static_assert((0UZ + ... + (TPredicates::end == LimitEnd::lower ? 1UZ : 0UZ)) <= 1UZ, "Limits holds one lower end at most: Min or Over");
    static_assert((0UZ + ... + (TPredicates::end == LimitEnd::upper ? 1UZ : 0UZ)) <= 1UZ, "Limits holds one upper end at most: Max or Under");
    static_assert(limitEndsInOrder<TPredicates...>(), "the lower end of Limits lies below its upper end, or at it where Min and Max both include it");
    static_assert(listedValuesPass<TPredicates...>(), "each value OneOf lists passes the other predicates of Limits");

    template<typename T>
    [[nodiscard]] static constexpr bool fitsIn() noexcept {
        return (TPredicates::template fitsIn<T>() && ...);
    }
};

} // namespace detail

/**
 * @brief The limits of an Annotated<..> value: predicates that must all hold, each beside the value's declaration.
 *
 * `Min<v>` and `Max<v>` include their value, `Over<v>` and `Under<v>` exclude it, and `OneOf<a, b, c>` holds for a
 * value equal to one of them. A NaN fails every predicate, and limits with any predicate refuse positive and negative
 * infinity. On a setting of an enumeration type, `OneOf` lists the enumerators the block takes, and is the one
 * predicate allowed. The compiler refuses a list of predicates that contradict each other, an end or a listed value the
 * setting's type cannot hold, and any argument other than a predicate; a rule no predicate states belongs in the
 * block's `checkSettings`.
 *
 * Example:
 * ```
 * Annotated<gr::Size_t, Limits<Min<1U>, Max<64U>>>          taps;
 * Annotated<float, Limits<Over<0.f>, Under<0.5f>>>        cutoff;
 * Annotated<gr::Size_t, Limits<OneOf<1U, 2U, 4U>>>    decimation;
 * Annotated<Window, Limits<OneOf<Window::hann, Window::hamming>>> window;
 * ```
 */
template<detail::LimitArgument... kArguments>
struct Limits : detail::PredicateLimits<detail::kPredicatesAlone<kArguments...>, detail::LimitArgumentType<kArguments>...> {
    static_assert(detail::kPredicatesAlone<kArguments...>, "Limits takes predicates alone: Min, Max, Over, Under and OneOf");

    using Predicates = typename detail::PredicateLimits<detail::kPredicatesAlone<kArguments...>, detail::LimitArgumentType<kArguments>...>::Predicates;

    // a number of another type is compared with each predicate before it converts to the setting's type: -0.5 lies below
    // a limit of 0
    template<typename U>
    [[nodiscard]] static constexpr bool validate(const U& value) noexcept {
        if constexpr (!Limits::kNoPredicate && std::is_floating_point_v<U>) {
            if (value == std::numeric_limits<U>::infinity() || value == -std::numeric_limits<U>::infinity()) {
                return false;
            }
        }
        return []<typename... TPredicates>(gr::meta::typelist<TPredicates...>, const U& held) { return (TPredicates::holds(held) && ...); }(Predicates{}, value);
    }
};

template<typename T>
struct is_limits : std::false_type {};

template<detail::LimitArgument... kArguments>
struct is_limits<Limits<kArguments...>> : std::true_type {};

template<typename T>
concept Limit = is_limits<T>::value;

using EmptyLimit = Limits<>; // nomen-est-omen

static_assert(Limit<EmptyLimit>);

namespace settings {
template<typename TBlock>
struct BlockSettingsOf;
}

/**
 * @brief Annotated is a template class that acts as a transparent wrapper around another type.
 * It allows adding additional meta-information to a type, such as documentation, unit, and visibility.
 * The meta-information is supplied as template parameters.
 *
 * As a member of a block's `Settings`, `changed()` tells the block's `applySettings` whether the batch it applies
 * changed this setting; at the block's setup every setting is changed. Core sets the flags of the block's own
 * `Settings` for the hook's call and clears them when it returns. A copy constructed from a setting keeps its flag;
 * an assignment keeps the flag of the setting assigned to.
 */
template<typename T, typename... Arguments>
struct Annotated {
    using value_type = T;
    using LimitType  = typename gr::meta::typelist<Arguments...>::template find_or_default<is_limits, EmptyLimit>;

    static_assert(!std::is_enum_v<std::remove_cv_t<T>> || LimitType::kOneOfAlone, "Limits on an enumeration takes OneOf alone: Min, Max, Over and Under bound a number");
    static_assert(std::is_enum_v<std::remove_cv_t<T>> || LimitType::kNoPredicate || detail::LimitNumber<std::remove_cv_t<T>>, "Limits annotates a number setting, or an enumeration with OneOf");
    static_assert((std::is_enum_v<std::remove_cv_t<T>> && !LimitType::kOneOfAlone) || LimitType::template fitsIn<std::remove_cv_t<T>>(), "a predicate of Limits names a value the setting's type cannot hold");

    T value;

private:
    template<typename TBlock>
    friend struct settings::BlockSettingsOf;

    bool _changed = false; // core sets it for the apply hook's call and clears it when the hook returns or throws

public:
    Annotated()                 = default;
    Annotated(const Annotated&) = default;
    Annotated(Annotated&&)      = default;

    // an assignment from another setting takes its value and leaves this setting's flag, which tells the hook what the
    // batch changed
    constexpr Annotated& operator=(const Annotated& other) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        this->value = other.value;
        return *this;
    }

    constexpr Annotated& operator=(Annotated&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        this->value = std::move(other.value);
        return *this;
    }

    template<typename U>
    requires std::constructible_from<T, U> && (!std::same_as<std::remove_cvref_t<U>, Annotated>)
    explicit(false) Annotated(U&& input) noexcept(std::is_nothrow_constructible_v<T, U>) : value(static_cast<T>(std::forward<U>(input))) {}

    template<typename U>
    requires std::assignable_from<T&, U>
    Annotated& operator=(U&& input) noexcept(std::is_nothrow_assignable_v<T, U>) {
        this->value = static_cast<T>(std::forward<U>(input));
        return *this;
    }

    inline explicit(false) constexpr operator T&() noexcept { return this->value; }

    inline explicit(false) constexpr operator const T&() const noexcept { return this->value; }

    [[nodiscard]] constexpr bool changed() const noexcept { return _changed; }

    constexpr bool operator==(const Annotated& other) const noexcept { return this->value == other.value; }

    template<typename U>
    constexpr bool operator==(const U& other) const noexcept {
        if constexpr (requires { other.value; }) {
            return this->value == other.value;
        } else if constexpr (std::is_convertible_v<const T&, std::string_view> && std::is_convertible_v<const U&, std::string_view>) {
            return std::string_view(this->value) == std::string_view(other);
        } else {
            return this->value == other;
        }
    }

    template<typename U>
    requires std::is_same_v<std::remove_cvref_t<U>, T>
    [[nodiscard]] constexpr bool validate_and_set(U&& value_) {
        if constexpr (std::is_same_v<LimitType, EmptyLimit>) {
            this->value = std::forward<U>(value_);
            return true;
        } else {
            if (LimitType::validate(value_)) {
                this->value = std::forward<U>(value_);
                return true;
            } else {
                return false;
            }
        }
    }

    operator std::string_view() const noexcept
    requires(std::is_same_v<T, std::string> || std::is_same_v<T, std::pmr::string>)
    {
        return std::string_view(this->value);
    }

    // meta-information
    inline static constexpr std::string_view documentation() noexcept {
        using Documentation = typename gr::meta::typelist<Arguments...>::template find_or_default<is_doc, EmptyDoc>;
        return std::string_view{Documentation::value};
    }

    inline static constexpr std::string_view unit() noexcept {
        using PhysicalUnit = typename gr::meta::typelist<Arguments...>::template find_or_default<is_unit, EmptyUnit>;
        return std::string_view{PhysicalUnit::value};
    }

    inline static constexpr bool visible() noexcept { return gr::meta::typelist<Arguments...>::template contains<Visible>; }

    inline static constexpr bool stoppedOnly() noexcept { return gr::meta::typelist<Arguments...>::template contains<StoppedOnly>; }

    // the dynamic port collections the setting sizes; empty for any other setting
    inline static constexpr std::span<const std::string_view> portCountFor() noexcept {
        static_assert((0UZ + ... + std::size_t{is_port_count_for<Arguments>::value}) <= 1UZ, "a setting declares at most one PortCountFor");
        using Collections = typename gr::meta::typelist<Arguments...>::template find_or_default<is_port_count_for, void>;
        if constexpr (std::is_void_v<Collections>) {
            return {};
        } else {
            return Collections::value;
        }
    }

    // forwarding member functions
    template<typename... Args>
    constexpr auto operator()(Args&&... args) -> decltype(auto) {
        return this->value(std::forward<Args>(args)...);
    }

    template<typename... Args>
    constexpr auto operator()(Args&&... args) const -> decltype(auto) {
        return this->value(std::forward<Args>(args)...);
    }

    template<typename Arg>
    constexpr auto operator[](Arg&& arg) -> decltype(auto) {
        return this->value[std::forward<Arg>(arg)];
    }

    template<typename Arg>
    constexpr auto operator[](Arg&& arg) const -> decltype(auto) {
        return this->value[std::forward<Arg>(arg)];
    }

    template<typename Arg>
    constexpr auto operator->*(Arg&& arg) -> decltype(auto) {
        return this->value.*std::forward<Arg>(arg);
    }

    template<typename Arg>
    constexpr auto operator->*(Arg&& arg) const -> decltype(auto) {
        return this->value.*std::forward<Arg>(arg);
    }

    constexpr T*       operator->() noexcept { return &this->value; }
    constexpr const T* operator->() const noexcept { return &this->value; }
};

template<typename T>
struct is_annotated : std::false_type {};

template<typename T, typename... Args>
struct is_annotated<gr::Annotated<T, Args...>> : std::true_type {};

template<typename T>
concept AnnotatedType = is_annotated<T>::value;

template<typename T>
struct unwrap_if_wrapped {
    using type = T;
};

template<typename U, typename... Args>
struct unwrap_if_wrapped<gr::Annotated<U, Args...>> {
    using type = U;
};

/**
 * @brief A type trait class that extracts the underlying type `T` from an `Annotated` instance.
 * If the given type is not an `Annotated`, it returns the type itself.
 */
template<typename T>
using unwrap_if_wrapped_t = typename unwrap_if_wrapped<T>::type;

} // namespace gr

template<typename... Ts>
struct gr::meta::typelist<gr::SupportedTypes<Ts...>> : gr::meta::typelist<Ts...> {};

template<typename T, typename... Arguments>
struct std::formatter<gr::Annotated<T, Arguments...>> {
    using Type = std::remove_const_t<T>;
    std::formatter<Type> value_formatter;

    template<typename FormatContext>
    constexpr auto parse(FormatContext& ctx) {
        return value_formatter.parse(ctx);
    }

    template<typename FormatContext>
    constexpr auto format(const gr::Annotated<T, Arguments...>& annotated, FormatContext& ctx) const {
        return value_formatter.format(annotated.value, ctx);
    }
};

namespace gr {
template<typename T, typename... Arguments>
inline std::ostream& operator<<(std::ostream& os, const gr::Annotated<T, Arguments...>& v) {
    // TODO: add switch for printing only brief and/or meta-information
    return os << std::format("{}", v.value);
}
} // namespace gr

#endif // GNURADIO_ANNOTATED_HPP
