#ifndef GNURADIO_SETTING_DECLARATION_HPP
#define GNURADIO_SETTING_DECLARATION_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <gnuradio-4.0/PortTraits.hpp>
#include <gnuradio-4.0/SettingValue.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Tensor.hpp>
#include <gnuradio-4.0/Value.hpp>
#include <gnuradio-4.0/annotated.hpp>
#include <gnuradio-4.0/meta/immutable.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>
#include <gnuradio-4.0/meta/utils.hpp>

namespace gr::settings {

namespace detail {

/**
 * @brief The type a setting holds, in the terms of a stored `pmt::Value`.
 *
 * A scalar setting has the container `Scalar`, `Complex` or `String`; a list has `Tensor` and its element type, a text
 * list the element `String`; a map has `Map`. The element `Monostate` marks a setting that takes any value.
 */
struct SettingType {
    pmt::Value::ValueType     element   = pmt::Value::ValueType::Monostate;
    pmt::Value::ContainerType container = pmt::Value::ContainerType::Scalar;
    std::size_t               extent    = 0UZ; // the element count of a fixed-size list; 0 for any count

    [[nodiscard]] bool isInteger() const noexcept { return container == pmt::Value::ContainerType::Scalar && element >= pmt::Value::ValueType::Int8 && element <= pmt::Value::ValueType::UInt64; }
    [[nodiscard]] bool isNumber() const noexcept { return isInteger() || (container == pmt::Value::ContainerType::Scalar && (element == pmt::Value::ValueType::Float32 || element == pmt::Value::ValueType::Float64)); }
    [[nodiscard]] bool isText() const noexcept { return container == pmt::Value::ContainerType::String; }
    [[nodiscard]] bool isBool() const noexcept { return container == pmt::Value::ContainerType::Scalar && element == pmt::Value::ValueType::Bool; }

    bool operator==(const SettingType&) const = default;
};

// the description's spelling of a type: `float32`, `str`, `map`, `float32_vector`
[[nodiscard]] std::string                typeName(const SettingType& type);
[[nodiscard]] std::optional<SettingType> parseTypeName(std::string_view name);

enum class AccessMode : std::uint8_t { readWrite, readOnly };
enum class DeclarationSource : std::uint8_t { reflected, described, core };

/**
 * @brief Everything the settings read about one key: a block-wide setting, or a setting of a member group, which
 * stands for that key of every member.
 *
 * A reflected key reads its field through `readField`, and the field holds its default; a reflected member the core
 * can write also stores a converted value into its field through `writeField`, and a `const` or immutable member has
 * no writer and no `fieldAddress`. A described key lives in the block's `describedSettings` under its tag key, with
 * `defaultValue` as its default when that holds a value. A caller writes a read-write key; only the block writes a
 * read-only key, by `publish`. A published key's field is the block's: a read returns it, and in force holds the
 * value last set beside it; a read-only key is published, and in force holds no value for it. A member of the block's
 * `Status` is a read-only key with no field functions: a read returns the copy the core published last.
 */
struct Declaration {
    using FieldAddress = void* (*)(void* block);
    using ValueAddress = void* (*)(void* block); // the value a reflected field holds, inside its decoration
    using LimitsCheck  = std::optional<std::string> (*)(const pmt::Value& converted);

    // reads a field in two steps: the member's function finds its value, and the value type's function reads it
    struct FieldReader {
        ValueAddress valueAddress             = nullptr;
        pmt::Value (*read)(const void* value) = nullptr;

        [[nodiscard]] pmt::Value operator()(const void* block) const { return read(valueAddress(const_cast<void*>(block))); }
        [[nodiscard]] bool       operator==(std::nullptr_t) const noexcept { return valueAddress == nullptr; }
    };

    // writes a field in the same two steps; false: the field's type cannot hold the value
    struct FieldWriter {
        ValueAddress valueAddress                               = nullptr;
        bool (*write)(void* value, const pmt::Value& converted) = nullptr;

        [[nodiscard]] bool operator()(void* block, const pmt::Value& converted) const { return write(valueAddress(block), converted); }
        [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return valueAddress == nullptr; }
    };

    std::string              name;
    SettingType              type{};
    DeclarationSource        source      = DeclarationSource::reflected;
    AccessMode               access      = AccessMode::readWrite;
    bool                     stoppedOnly = false; // a change waits for the graph to stop
    bool                     published   = false; // the block writes the field by publish; a read returns the field
    bool                     reported    = false; // a member of the block's `Status`; a read returns the copy core made last
    std::vector<std::string> portCollections{};   // the dynamic port collections a port key sizes; empty for any other key

    pmt::Value               defaultValue{}; // empty: no default
    pmt::Value               minimum{};
    pmt::Value               maximum{};
    bool                     minimumExcluded = false;
    bool                     maximumExcluded = false;
    pmt::Value               oneOf{};  // the values a `OneOf` names, in the setting's type; empty: none
    std::string              limits{}; // the limits as a host reads them, "> 0 and < 0.5"; empty: the minimum and maximum say all
    pmt::Value               step{};   // a hint for an application; no check reads it
    std::vector<std::string> choices{};

    bool                annotated = false;
    std::string         unit{};
    std::string         description{};
    std::string         documentation{};
    std::optional<bool> visible{};
    std::string         enumType{};
    std::string         enabledWhenParameter{};
    pmt::Value          enabledWhenValue{};

    FieldAddress fieldAddress = nullptr;
    FieldReader  readField{};
    FieldWriter  writeField{};
    LimitsCheck  checkFieldLimit = nullptr; // a reflected field's `Limits<>`

    [[nodiscard]] bool isPortKey() const noexcept { return !portCollections.empty(); }
    [[nodiscard]] bool isReadOnly() const noexcept { return access == AccessMode::readOnly; }
    // takes effect only at a start; a port key follows the port rules instead
    [[nodiscard]] bool atStart() const noexcept { return stoppedOnly && !isPortKey(); }
};

using Declarations = std::map<std::string, Declaration, std::less<>>;

struct MemberGroup {
    std::string  name;
    std::string  countKey{}; // the block-wide port key that counts the members; empty: any index
    bool         bare = false;
    Declarations settings{};
};

// a key resolved against a declaration table
struct KeyRef {
    const Declaration* declaration = nullptr;
    const MemberGroup* group       = nullptr; // null for a block-wide key
    std::size_t        member      = 0UZ;
    bool               bareName    = false; // the key named member 0 of the bare group by the setting's name alone
};

/**
 * @brief The declarations of one block type, read once from its reflected fields with their annotations and from its
 * description, and the faults found while reading them.
 */
struct DeclarationTable {
    using CollectionResize = void (*)(void* block, std::size_t size);
    using CollectionSize   = std::size_t (*)(void* block);

    std::string                                          blockType;
    bool                                                 described   = false;
    bool                                                 isScheduler = false; // a running scheduler holds each key for its next start
    std::string                                          documentation{};
    Declarations                                         keys{};   // block-wide settings
    std::map<std::string, MemberGroup, std::less<>>      groups{}; // member groups by name
    std::string                                          bareGroup{};
    std::vector<std::string>                             portCollections{};   // the block's dynamic port collections
    std::map<std::string, CollectionResize, std::less<>> collectionResizes{}; // each reflected collection's resize, by name
    std::map<std::string, CollectionSize, std::less<>>   collectionSizes{};   // each reflected collection's size, by name
    Rejected                                             faults{};            // one entry per fault, each with an empty key
    property_map                                         keyMetadata{};       // the `<key>::<field>` metadata of every key, the same for each block of the type

    // a block-wide key, a bare name, or a member key `group.member.setting`; allocates nothing
    [[nodiscard]] std::optional<KeyRef> find(std::string_view key) const noexcept;
};

inline constexpr std::string_view kNotificationLevelKey = "notification_level";

// the sent value in the key's type, or the reason it does not convert
[[nodiscard]] std::expected<pmt::Value, std::string> convert(const Declaration& declaration, const pmt::Value& sent);

// the reason the declaration's limits or list of values refuse a value `convert` returned, or nothing
[[nodiscard]] std::optional<std::string> refuseByLimits(const Declaration& declaration, const pmt::Value& converted);

void addDescription(DeclarationTable& table, const property_map& description);
// checks the table and formats its `keyMetadata`; a scheduler's table gains the notification level
void finishDeclarationTable(DeclarationTable& table, bool isScheduler);
void writeMetadata(const DeclarationTable& table, std::string_view blockDescription, property_map& metaInformation);
void fillDescribedDefaults(const DeclarationTable& table, const void* block, property_map& describedSettings);

template<typename T>
struct FieldValue {
    using type = T;
};
template<typename T>
requires gr::meta::is_immutable<T>::value
struct FieldValue<T> {
    using type = typename T::value_type;
};

template<typename T>
inline constexpr bool kIsText = std::same_as<T, std::string> || std::same_as<T, std::pmr::string>;

template<typename T>
inline constexpr bool kIsSettingElement = SettingScalar<T> || kIsText<T> || std::is_enum_v<T> || std::same_as<T, pmt::Value>;

template<typename T>
inline constexpr bool kIsSettingSequence = false;
template<typename T>
requires kIsSequence<T>
inline constexpr bool kIsSettingSequence<T> = kIsSettingElement<typename T::value_type>;

template<typename T>
inline constexpr bool kIsSettingField = kIsSettingElement<T> || std::same_as<T, property_map> || kIsSettingSequence<T>;

// the fixed-width integer of a field's size and signedness; `std::size_t` is `uint64` on a 64-bit platform and `uint32` on wasm32
template<typename T>
using StoredScalar = std::conditional_t<std::same_as<T, bool> || !std::integral<T>, T,
    std::conditional_t<std::is_signed_v<T>,                                                                                                                                      //
        std::conditional_t<sizeof(T) == 1UZ, std::int8_t, std::conditional_t<sizeof(T) == 2UZ, std::int16_t, std::conditional_t<sizeof(T) == 4UZ, std::int32_t, std::int64_t>>>, //
        std::conditional_t<sizeof(T) == 1UZ, std::uint8_t, std::conditional_t<sizeof(T) == 2UZ, std::uint16_t, std::conditional_t<sizeof(T) == 4UZ, std::uint32_t, std::uint64_t>>>>>;

template<typename T>
[[nodiscard]] consteval pmt::Value::ValueType elementTypeOf() {
    using enum pmt::Value::ValueType;
    using S = StoredScalar<T>;
    if constexpr (std::same_as<S, bool>) {
        return Bool;
    } else if constexpr (std::same_as<S, std::int8_t>) {
        return Int8;
    } else if constexpr (std::same_as<S, std::int16_t>) {
        return Int16;
    } else if constexpr (std::same_as<S, std::int32_t>) {
        return Int32;
    } else if constexpr (std::same_as<S, std::int64_t>) {
        return Int64;
    } else if constexpr (std::same_as<S, std::uint8_t>) {
        return UInt8;
    } else if constexpr (std::same_as<S, std::uint16_t>) {
        return UInt16;
    } else if constexpr (std::same_as<S, std::uint32_t>) {
        return UInt32;
    } else if constexpr (std::same_as<S, std::uint64_t>) {
        return UInt64;
    } else if constexpr (std::same_as<S, float>) {
        return Float32;
    } else if constexpr (std::same_as<S, double>) {
        return Float64;
    } else if constexpr (std::same_as<S, std::complex<float>>) {
        return ComplexFloat32;
    } else if constexpr (std::same_as<S, std::complex<double>>) {
        return ComplexFloat64;
    } else if constexpr (kIsText<S> || std::is_enum_v<S>) {
        return String;
    } else {
        return Monostate;
    }
}

template<typename T>
[[nodiscard]] consteval pmt::Value::ContainerType scalarContainerOf() {
    if constexpr (kIsComplex<T>) {
        return pmt::Value::ContainerType::Complex;
    } else if constexpr (kIsText<T> || std::is_enum_v<T>) {
        return pmt::Value::ContainerType::String;
    } else {
        return pmt::Value::ContainerType::Scalar;
    }
}

template<typename T>
[[nodiscard]] consteval SettingType settingTypeOf() {
    if constexpr (std::same_as<T, property_map>) {
        return {.element = pmt::Value::ValueType::Value, .container = pmt::Value::ContainerType::Map};
    } else if constexpr (kIsSequence<T>) {
        std::size_t extent = 0UZ;
        if constexpr (gr::meta::array_type<T>) {
            extent = std::tuple_size_v<T>;
        }
        return {.element = elementTypeOf<typename T::value_type>(), .container = pmt::Value::ContainerType::Tensor, .extent = extent};
    } else {
        return {.element = elementTypeOf<T>(), .container = scalarContainerOf<T>()};
    }
}

template<typename T>
[[nodiscard]] auto storedElement(const T& element) {
    if constexpr (std::is_enum_v<T>) {
        return pmt::Value(gr::meta::enumName(element).value_or(std::string_view{}));
    } else if constexpr (kIsText<T>) {
        return pmt::Value(std::string_view(element));
    } else if constexpr (std::same_as<T, pmt::Value>) {
        return element;
    } else {
        return static_cast<StoredScalar<T>>(element);
    }
}

// a field's value as the settings store it: integers in fixed width, enumerators and texts as texts, lists as tensors
template<typename T>
[[nodiscard]] pmt::Value storedValue(const T& field) {
    if constexpr (kIsSequence<T>) {
        using TElement = typename T::value_type;
        using TStored  = std::conditional_t<kIsText<TElement> || std::is_enum_v<TElement> || std::same_as<TElement, pmt::Value>, pmt::Value, StoredScalar<TElement>>;
        Tensor<TStored> tensor(extents_from, {static_cast<std::size_t>(std::ranges::size(field))});
        std::ranges::transform(field, tensor.begin(), [](const TElement& element) { return TStored(storedElement(element)); });
        return pmt::Value(std::move(tensor));
    } else if constexpr (std::same_as<T, property_map> || std::same_as<T, pmt::Value>) {
        return pmt::Value(field);
    } else {
        return pmt::Value(storedElement(field));
    }
}

template<typename T>
[[nodiscard]] const auto& undecorated(const T& member) {
    if constexpr (AnnotatedType<T>) {
        return member.value;
    } else if constexpr (gr::meta::is_immutable<T>::value) {
        return member.value();
    } else {
        return member;
    }
}

template<typename TBlock, std::size_t kIdx>
using MemberRawType = std::remove_cvref_t<refl::data_member_type<TBlock, kIdx>>;

// the type of a block's own settings struct, `TBlock::Settings`; void for a block that declares none
template<typename TBlock>
struct SettingsStructOf {
    using type = void;
};
template<typename TBlock>
requires requires { typename TBlock::Settings; }
struct SettingsStructOf<TBlock> {
    using type = typename TBlock::Settings;
};

// the type of the values a block reports, `TBlock::Status`; void for a block that declares none
template<typename TBlock>
struct StatusStructOf {
    using type = void;
};
template<typename TBlock>
requires requires { typename TBlock::Status; }
struct StatusStructOf<TBlock> {
    using type = typename TBlock::Status;
};

inline constexpr std::size_t kDirectMember = std::numeric_limits<std::size_t>::max();

// what one reflected member of a block is to the settings: a member of its own, or the block's `Settings` or `Status`
// struct, whose members take its place
enum class OuterKind : std::uint8_t { direct, settings, status };

template<typename TBlock>
[[nodiscard]] consteval auto outerKindsOf() {
    using SettingsStruct = typename SettingsStructOf<TBlock>::type;
    using StatusStruct   = typename StatusStructOf<TBlock>::type;
    return []<std::size_t... kIdx>(std::index_sequence<kIdx...>) {
        std::array<OuterKind, sizeof...(kIdx)> kinds{};
        if constexpr (!std::is_void_v<SettingsStruct> || !std::is_void_v<StatusStruct>) {
            kinds = {(std::same_as<MemberRawType<TBlock, kIdx>, SettingsStruct> ? OuterKind::settings : (std::same_as<MemberRawType<TBlock, kIdx>, StatusStruct> ? OuterKind::status : OuterKind::direct))...};
        }
        return kinds;
    }(std::make_index_sequence<refl::data_member_count<TBlock>>());
}

// the kind of each reflected member of the block, by its index
template<typename TBlock>
inline constexpr auto kOuterKinds = outerKindsOf<TBlock>();

[[nodiscard]] consteval bool holdsKind(std::span<const OuterKind> kinds, OuterKind kind) { return std::ranges::find(kinds, kind) != kinds.end(); }

/**
 * @brief Where one flat member lives: the block's reflected member `outer`, and inside it the member `inner` where
 * `outer` is the block's `Settings` or `Status` struct.
 */
struct FlatIndex {
    std::size_t outer = 0UZ;
    std::size_t inner = kDirectMember;
};

// an initializer of any member, for unevaluated aggregate initialization alone: a member's own converting constructor
// takes it in preference to its conversion, which is `const&`-qualified, so a member with a constructor template takes
// it as a plain member does
struct AnyMember {
    template<typename T>
    operator T() const&;
};

template<typename TStruct, std::size_t... kIdx>
[[nodiscard]] consteval bool takesInitializers(std::index_sequence<kIdx...>) {
    return requires { TStruct{(static_cast<void>(kIdx), AnyMember{})...}; };
}

// whether aggregate initialization of the struct takes more initializers than its GR_MAKE_REFLECTABLE lists members,
// so that the list leaves out a member; a struct that is no aggregate is not judged
template<typename TStruct>
inline constexpr bool kListLeavesOutMember = takesInitializers<TStruct>(std::make_index_sequence<refl::data_member_count<TStruct> + 1UZ>());

template<typename TBlock>
[[nodiscard]] consteval std::size_t innerCountOf(OuterKind kind) {
    using SettingsStruct = typename SettingsStructOf<TBlock>::type;
    using StatusStruct   = typename StatusStructOf<TBlock>::type;
    if (kind == OuterKind::settings) {
        static_assert(std::is_void_v<SettingsStruct> || !holdsKind(kOuterKinds<TBlock>, OuterKind::settings) || refl::reflectable<SettingsStruct>, "a block's struct Settings or Status lists its members in its own GR_MAKE_REFLECTABLE");
        static_assert(std::is_void_v<SettingsStruct> || !refl::reflectable<SettingsStruct> || !kListLeavesOutMember<SettingsStruct>, "the GR_MAKE_REFLECTABLE of a block's struct Settings or Status lists each of its members");
        return refl::data_member_count<SettingsStruct>;
    }
    if (kind == OuterKind::status) {
        static_assert(std::is_void_v<StatusStruct> || !holdsKind(kOuterKinds<TBlock>, OuterKind::status) || refl::reflectable<StatusStruct>, "a block's struct Settings or Status lists its members in its own GR_MAKE_REFLECTABLE");
        static_assert(std::is_void_v<StatusStruct> || !refl::reflectable<StatusStruct> || !kListLeavesOutMember<StatusStruct>, "the GR_MAKE_REFLECTABLE of a block's struct Settings or Status lists each of its members");
        return refl::data_member_count<StatusStruct>;
    }
    return 1UZ;
}

template<typename TBlock>
[[nodiscard]] consteval std::size_t flatCountOf() {
    std::size_t count = 0UZ;
    for (OuterKind kind : kOuterKinds<TBlock>) {
        count += innerCountOf<TBlock>(kind);
    }
    return count;
}

// the count of the block's flat members
template<typename TBlock>
inline constexpr std::size_t kFlatCount = flatCountOf<TBlock>();

template<typename TBlock>
[[nodiscard]] consteval std::array<FlatIndex, kFlatCount<TBlock>> flatIndicesOf() {
    std::array<FlatIndex, kFlatCount<TBlock>> indices{};
    std::size_t                               flat = 0UZ;
    for (std::size_t outer = 0UZ; outer < kOuterKinds<TBlock>.size(); ++outer) {
        const OuterKind kind = kOuterKinds<TBlock>[outer];
        if (kind == OuterKind::direct) {
            indices[flat++] = FlatIndex{.outer = outer};
            continue;
        }
        for (std::size_t inner = 0UZ; inner < innerCountOf<TBlock>(kind); ++inner) {
            indices[flat++] = FlatIndex{.outer = outer, .inner = inner};
        }
    }
    return indices;
}

// the place of each flat member of the block, computed once per block type; every walk over the members reads it
template<typename TBlock>
inline constexpr std::array<FlatIndex, kFlatCount<TBlock>> kFlatIndices = flatIndicesOf<TBlock>();

/**
 * @brief One setting member as the settings see it: the block's reflected member `kIdx`, or, where that member is the
 * block's `Settings` or `Status` struct, the struct's reflected member `kInner`, presented under its own name.
 */
template<typename TBlock, std::size_t kIdx, std::size_t kInner = kDirectMember>
struct FlatMember {
    using Type                                 = refl::data_member_type<TBlock, kIdx>;
    static constexpr auto        kName         = refl::data_member_name<TBlock, kIdx>;
    static constexpr bool        kInStruct     = false;
    static constexpr bool        kInStatus     = false;
    static constexpr bool        kFromBaseType = kIdx < refl::data_member_count<refl::base_type<TBlock>>;
    static constexpr std::size_t kIndex        = kIdx;

    static decltype(auto) of(auto& block) { return refl::data_member<kIdx>(block); }
};
template<typename TBlock, std::size_t kIdx, std::size_t kInner>
requires(kInner != kDirectMember)
struct FlatMember<TBlock, kIdx, kInner> {
    using Struct                               = MemberRawType<TBlock, kIdx>;
    using Type                                 = refl::data_member_type<Struct, kInner>;
    static constexpr auto        kName         = refl::data_member_name<Struct, kInner>;
    static constexpr bool        kInStruct     = true;
    static constexpr bool        kInStatus     = kOuterKinds<TBlock>[kIdx] == OuterKind::status; // a value the block reports, which no caller sets
    static constexpr bool        kFromBaseType = false;
    static constexpr std::size_t kIndex        = kIdx;

    static decltype(auto) of(auto& block) { return refl::data_member<kInner>(refl::data_member<kIdx>(block)); }
    static decltype(auto) inStruct(auto& settings) { return refl::data_member<kInner>(settings); }
};

// the flat member `kFlat` of the block: its reflected members with its `Settings` and `Status` members replaced by those
// structs' members; only those two types are entered, since ports and tags are reflectable too
template<typename TBlock, std::size_t kFlat>
using FlatMemberAt = FlatMember<TBlock, kFlatIndices<TBlock>[kFlat].outer, kFlatIndices<TBlock>[kFlat].inner>;

template<typename TBlock, typename TFunction>
constexpr void forEachFlatMember(TFunction&& function) {
    [&function]<std::size_t... kFlat>(std::index_sequence<kFlat...>) { (function(FlatMemberAt<TBlock, kFlat>{}), ...); }(std::make_index_sequence<kFlatCount<TBlock>>());
}

// the block reflects a member of its type `Settings`
template<typename TBlock>
[[nodiscard]] consteval bool holdsSettingsStruct() {
    return holdsKind(kOuterKinds<TBlock>, OuterKind::settings);
}

// the block reflects a member of its type `Status`
template<typename TBlock>
[[nodiscard]] consteval bool holdsStatusStruct() {
    return holdsKind(kOuterKinds<TBlock>, OuterKind::status);
}

// the block's reflected member of its type `Settings`, whatever its name
template<typename TBlock>
requires(holdsSettingsStruct<std::remove_const_t<TBlock>>())
[[nodiscard]] constexpr auto& settingsStructOf(TBlock& block) {
    constexpr std::size_t kIdx = static_cast<std::size_t>(std::ranges::find(kOuterKinds<std::remove_const_t<TBlock>>, OuterKind::settings) - kOuterKinds<std::remove_const_t<TBlock>>.begin());
    return refl::data_member<kIdx>(block);
}

template<typename TMember>
bool writeMember(TMember& member, const pmt::Value& converted) {
    using Type                = unwrap_if_wrapped_t<std::remove_cvref_t<TMember>>;
    std::optional<Type> value = losslessAs<Type>(converted);
    if (!value) {
        return false;
    }
    if constexpr (AnnotatedType<std::remove_cvref_t<TMember>>) {
        member.value = std::move(*value);
    } else {
        member = std::move(*value);
    }
    return true;
}

template<typename TBlock, typename TMember>
void* reflectedFieldAddress(void* block) {
    return std::addressof(TMember::of(*static_cast<TBlock*>(block)));
}

// the value a reflected member holds, inside its `Annotated` or `immutable`; the core writes through it only where the
// member is writable
template<typename TBlock, typename TMember>
void* reflectedValueAddress(void* block) {
    return const_cast<void*>(static_cast<const void*>(std::addressof(undecorated(TMember::of(*static_cast<TBlock*>(block))))));
}

template<typename TBlock, typename TMember>
void resizePortCollection(void* block, std::size_t size) {
    TMember::of(*static_cast<TBlock*>(block)).resize(size);
}

template<typename TBlock, typename TMember>
std::size_t portCollectionSize(void* block) {
    return TMember::of(*static_cast<TBlock*>(block)).size();
}

template<typename T>
pmt::Value readStoredValue(const void* value) {
    return storedValue(*static_cast<const T*>(value));
}

template<typename T>
bool writeStoredValue(void* value, const pmt::Value& converted) {
    std::optional<T> held = losslessAs<T>(converted);
    if (!held) {
        return false;
    }
    *static_cast<T*>(value) = std::move(*held);
    return true;
}

// the value types of the base block's settings and the common setting types, compiled once in the core library
extern template pmt::Value readStoredValue<bool>(const void*);
extern template pmt::Value readStoredValue<std::int32_t>(const void*);
extern template pmt::Value readStoredValue<std::uint32_t>(const void*);
extern template pmt::Value readStoredValue<std::int64_t>(const void*);
extern template pmt::Value readStoredValue<std::uint64_t>(const void*);
extern template pmt::Value readStoredValue<float>(const void*);
extern template pmt::Value readStoredValue<double>(const void*);
extern template pmt::Value readStoredValue<std::complex<float>>(const void*);
extern template pmt::Value readStoredValue<std::complex<double>>(const void*);
extern template pmt::Value readStoredValue<std::string>(const void*);
extern template pmt::Value readStoredValue<property_map>(const void*);
extern template pmt::Value readStoredValue<std::vector<float>>(const void*);
extern template pmt::Value readStoredValue<std::vector<double>>(const void*);
extern template pmt::Value readStoredValue<std::vector<std::string>>(const void*);
extern template bool       writeStoredValue<bool>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::int32_t>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::uint32_t>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::int64_t>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::uint64_t>(void*, const pmt::Value&);
extern template bool       writeStoredValue<float>(void*, const pmt::Value&);
extern template bool       writeStoredValue<double>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::complex<float>>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::complex<double>>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::string>(void*, const pmt::Value&);
extern template bool       writeStoredValue<property_map>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::vector<float>>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::vector<double>>(void*, const pmt::Value&);
extern template bool       writeStoredValue<std::vector<std::string>>(void*, const pmt::Value&);

// `"out of range [lower, upper]: value"`, a bound given as an empty text, which no value breaks, read as an infinity
[[nodiscard]] std::string rangeRefusal(std::string_view lower, std::string_view upper, std::string_view value);

// whether no value of `TType` breaks the end `TPredicate`
template<typename TType, typename TPredicate>
[[nodiscard]] bool holdsEveryValue() {
    if constexpr (gr::detail::LimitNumber<TType> && TPredicate::end == LimitEnd::lower) {
        return TPredicate::holds(std::numeric_limits<TType>::lowest());
    } else if constexpr (gr::detail::LimitNumber<TType> && TPredicate::end == LimitEnd::upper) {
        return TPredicate::holds(std::numeric_limits<TType>::max());
    } else {
        return false;
    }
}

// the limits of `TLimit` as a host reads them: each predicate's text joined by "and", without an end that no value of
// `TType` breaks; "a number" where no predicate is left, since only a NaN fails such limits
template<typename TLimit, typename TType>
[[nodiscard]] std::string limitsText() {
    std::string text;
    [&text]<typename... TPredicates>(gr::meta::typelist<TPredicates...>) {
        const auto add = [&text]<typename TPredicate>() {
            if (!holdsEveryValue<TType, TPredicate>()) {
                text += std::format("{}{}", text.empty() ? "" : " and ", TPredicate::text());
            }
        };
        (add.template operator()<TPredicates>(), ...);
    }(typename TLimit::Predicates{});
    return text.empty() ? std::string("a number") : text;
}

// the refusal of a finite value `TLimit` refuses: `"not one of: 1, 2: 3"` for a `OneOf` it fails, else the range of the
// ends with the value, `"out of range (0, 0.5]: 0.7"`, an end no value of `TType` breaks read as an infinity
template<typename TLimit, typename TType>
[[nodiscard]] std::string limitsRefusal(const TType& value) {
    std::string notListed;
    std::string lower = "(-inf";
    std::string upper = "inf)";
    [&]<typename... TPredicates>(gr::meta::typelist<TPredicates...>) {
        const auto add = [&]<typename TPredicate>() {
            if constexpr (TPredicate::end == LimitEnd::none) {
                if (notListed.empty() && !TPredicate::holds(value)) {
                    notListed = std::format("not {}: {}", TPredicate::text(), gr::detail::limitValueText(value));
                }
            } else if (holdsEveryValue<TType, TPredicate>()) {
                return;
            } else if constexpr (TPredicate::end == LimitEnd::lower) {
                lower = std::format("{}{}", TPredicate::excluded ? "(" : "[", gr::detail::limitValueText(TPredicate::bound));
            } else {
                upper = std::format("{}{}", gr::detail::limitValueText(TPredicate::bound), TPredicate::excluded ? ")" : "]");
            }
        };
        (add.template operator()<TPredicates>(), ...);
    }(typename TLimit::Predicates{});
    return notListed.empty() ? std::format("out of range {}, {}: {}", lower, upper, gr::detail::limitValueText(value)) : notListed;
}

template<typename TLimit, typename TType>
std::optional<std::string> checkLimit(const pmt::Value& converted) {
    const std::optional<TType> field = losslessAs<TType>(converted);
    if (!field || TLimit::validate(*field)) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<TType>) {
        if (std::isnan(*field)) {
            return std::string("not a number");
        }
        if (std::isinf(*field)) {
            return std::string("not finite");
        }
    }
    return limitsRefusal<TLimit, TType>(*field);
}

// the ends of `TLimit` and the values of its `OneOf` as metadata reads them, an enumerator by its name, and its text
template<typename TType, typename TLimit>
void addLimits(Declaration& declaration) {
    [&declaration]<typename... TPredicates>(gr::meta::typelist<TPredicates...>) {
        const auto add = [&declaration]<typename TPredicate>() {
            if constexpr (TPredicate::end == LimitEnd::lower) {
                declaration.minimum         = storedValue(gr::detail::castUnlessSame<TType>(TPredicate::bound));
                declaration.minimumExcluded = TPredicate::excluded;
            } else if constexpr (TPredicate::end == LimitEnd::upper) {
                declaration.maximum         = storedValue(gr::detail::castUnlessSame<TType>(TPredicate::bound));
                declaration.maximumExcluded = TPredicate::excluded;
            } else if constexpr (gr::detail::kIsOneOf<TPredicate>) {
                declaration.oneOf = storedValue(TPredicate::template listedAs<TType>());
            }
        };
        (add.template operator()<TPredicates>(), ...);
    }(typename TLimit::Predicates{});
    declaration.limits = limitsText<TLimit, TType>();
}

template<typename TEnum>
void addEnumChoices(Declaration& declaration) {
    declaration.enumType = std::string(gr::meta::type_name<TEnum>());
    for (const TEnum enumerator : gr::meta::enumValues<TEnum>()) {
        if (const std::optional<std::string_view> enumeratorName = gr::meta::enumName(enumerator); enumeratorName && !enumeratorName->empty()) {
            declaration.choices.emplace_back(*enumeratorName);
        }
    }
}

// an enumeration's choices, all of its enumerators, beside the part of them its `OneOf` takes
template<typename TEnum, typename TLimit>
void addEnumChoicesAndLimits(Declaration& declaration) {
    addEnumChoices<TEnum>(declaration);
    addLimits<TEnum, TLimit>(declaration);
}

template<typename T>
inline constexpr bool kIsDynamicPortCollection = false;
template<typename T>
requires gr::meta::vector_type<T> && gr::traits::port::is_port_v<typename T::value_type>
inline constexpr bool kIsDynamicPortCollection<T> = true;

/**
 * @brief What the settings read of one flat member of a block, computed once per member at compile time.
 *
 * The static checks of a block read these entries, and `makeDeclarationTable` builds each declaration from them. A
 * member of the block's `Status` takes no field functions: a read returns the copy the core published last.
 */
struct ReflectedMember {
    enum class Role : std::uint8_t { other, portCollection, setting, status };
    using TypeDetails = void (*)(Declaration& declaration); // writes what needs the member's type: the bounds of its limits, an enumeration's choices

    std::string_view name;
    Role             role         = Role::other;
    std::size_t      outer        = 0UZ; // the block's reflected member that holds it
    bool             inStruct     = false;
    bool             inStatus     = false;
    bool             fromBaseType = false;
    bool             repeatedBase = false; // a base's member reflected again by a type without its own GR_MAKE_REFLECTABLE
    bool             integer      = false; // an integer type other than bool

    SettingType                       type{};
    bool                              readOnly    = false;
    bool                              published   = false;
    bool                              annotated   = false;
    bool                              stoppedOnly = false;
    std::optional<bool>               visible{};
    std::string_view                  documentation{};
    std::string_view                  unit{};
    std::span<const std::string_view> portCountFor{};

    Declaration::FieldAddress fieldAddress = nullptr;
    Declaration::FieldReader  readField{};
    Declaration::FieldWriter  writeField{};
    Declaration::LimitsCheck  checkFieldLimit = nullptr;
    TypeDetails               typeDetails     = nullptr;

    DeclarationTable::CollectionResize resizeCollection = nullptr; // a dynamic port collection's
    DeclarationTable::CollectionSize   sizeOfCollection = nullptr; // a dynamic port collection's
};

// the table of a block type from its members and its description, if it has one
[[nodiscard]] DeclarationTable makeDeclarationTable(std::string blockType, std::span<const ReflectedMember> members, const property_map* description, bool isScheduler);

// for each reflected member of `T`: true where `T` or a base without its own GR_MAKE_REFLECTABLE reflects its base's
// members a second time, as the same objects
template<typename T>
[[nodiscard]] consteval std::array<bool, refl::data_member_count<T>> repeatedBaseMembersOf() {
    std::array<bool, refl::data_member_count<T>> repeated{};
    if constexpr (refl::reflectable<T>) {
        using Base                    = refl::base_type<T>;
        constexpr std::size_t kInBase = refl::data_member_count<Base>;
        if constexpr (kInBase > 0UZ) {
            std::ranges::copy(repeatedBaseMembersOf<Base>(), repeated.begin());
        }
        if constexpr (!std::is_void_v<Base> && kInBase < refl::data_member_count<T>) {
            const bool reflectedAgain = static_cast<const void*>(&T::gr_refl_data_member_names) == static_cast<const void*>(&Base::gr_refl_data_member_names);
            std::fill(repeated.begin() + static_cast<std::ptrdiff_t>(kInBase), repeated.end(), reflectedAgain);
        }
    }
    return repeated;
}

template<typename T>
inline constexpr std::array<bool, refl::data_member_count<T>> kRepeatedBaseMembers = repeatedBaseMembersOf<T>();

// constexpr and not consteval: MSVC 19.51 refuses a consteval function that returns an aggregate holding an engaged std::optional
template<typename TBlock, typename TMember>
[[nodiscard]] constexpr ReflectedMember reflectedMemberOf() {
    using MemberType = typename TMember::Type;
    using RawType    = std::remove_cvref_t<MemberType>;
    using Declared   = typename FieldValue<unwrap_if_wrapped_t<RawType>>::type;
    using Type       = std::remove_cv_t<Declared>;

    ReflectedMember member{.name = TMember::kName.view(), .outer = TMember::kIndex, .inStruct = TMember::kInStruct, .inStatus = TMember::kInStatus, .fromBaseType = TMember::kFromBaseType, .integer = std::integral<Type> && !std::same_as<Type, bool>};
    if constexpr (!TMember::kInStruct) {
        member.repeatedBase = kRepeatedBaseMembers<TBlock>[TMember::kIndex];
    }
    if constexpr (AnnotatedType<RawType>) {
        member.annotated     = true;
        member.documentation = RawType::documentation();
        member.unit          = RawType::unit();
        member.visible       = RawType::visible();
        member.stoppedOnly   = RawType::stoppedOnly();
        member.portCountFor  = RawType::portCountFor();
    }
    if constexpr (kIsDynamicPortCollection<RawType>) {
        member.role = ReflectedMember::Role::portCollection;
        if constexpr (!TMember::kInStruct) {
            member.resizeCollection = &resizePortCollection<TBlock, TMember>;
            member.sizeOfCollection = &portCollectionSize<TBlock, TMember>;
        }
    } else if constexpr (kIsSettingField<Type>) {
        constexpr bool kUnwritable = std::is_const_v<Declared> || std::is_const_v<std::remove_reference_t<MemberType>> || gr::meta::is_immutable<RawType>::value;
        member.type                = settingTypeOf<Type>();
        if constexpr (TMember::kInStatus) {
            member.role     = ReflectedMember::Role::status;
            member.readOnly = true;
        } else {
            member.role      = ReflectedMember::Role::setting;
            member.readOnly  = kUnwritable;
            member.published = kUnwritable;
            using Stored     = std::remove_cvref_t<decltype(undecorated(std::declval<const RawType&>()))>;
            member.readField = {.valueAddress = &reflectedValueAddress<TBlock, TMember>, .read = &readStoredValue<Stored>};
            if constexpr (!kUnwritable) {
                member.fieldAddress = &reflectedFieldAddress<TBlock, TMember>;
                member.writeField   = {.valueAddress = &reflectedValueAddress<TBlock, TMember>, .write = &writeStoredValue<Stored>};
            }
        }
        if constexpr (AnnotatedType<RawType>) {
            using Limit = typename RawType::LimitType;
            if constexpr (!std::is_same_v<Limit, EmptyLimit>) {
                member.checkFieldLimit = &checkLimit<Limit, Type>;
                if constexpr (std::is_enum_v<Type>) {
                    member.typeDetails = &addEnumChoicesAndLimits<Type, Limit>;
                } else if constexpr (std::is_arithmetic_v<Type>) {
                    member.typeDetails = &addLimits<Type, Limit>;
                }
            }
        }
        if constexpr (std::is_enum_v<Type>) {
            if (member.typeDetails == nullptr) {
                member.typeDetails = &addEnumChoices<Type>;
            }
        }
    }
    return member;
}

// the entry of each flat member of the block, in the order of `kFlatIndices`: the one pass over its members that
// the static checks and the table read
template<typename TBlock>
inline constexpr std::array<ReflectedMember, kFlatCount<TBlock>> kReflectedMembers = []<std::size_t... kFlat>(std::index_sequence<kFlat...>) { //
    return std::array<ReflectedMember, sizeof...(kFlat)>{reflectedMemberOf<TBlock, FlatMemberAt<TBlock, kFlat>>()...};
}(std::make_index_sequence<kFlatCount<TBlock>>());

// the names of the block's reflected members, by index
template<typename TBlock>
inline constexpr std::array<std::string_view, refl::data_member_count<TBlock>> kOuterNames = []<std::size_t... kIdx>(std::index_sequence<kIdx...>) { //
    return std::array<std::string_view, sizeof...(kIdx)>{refl::data_member_name<TBlock, kIdx>.view()...};
}(std::make_index_sequence<refl::data_member_count<TBlock>>());

template<typename TBlock>
[[nodiscard]] consteval std::size_t baseMemberCountOf() {
    if constexpr (refl::reflectable<TBlock>) {
        return refl::data_member_count<refl::base_type<TBlock>>;
    } else {
        return 0UZ;
    }
}

enum class PortCountForFault : std::uint8_t { none, notInteger, noDynamicCollection, sizedTwice };

// the first fault of the members' `PortCountFor` annotations, or none
[[nodiscard]] consteval PortCountForFault firstPortCountForFault(std::span<const ReflectedMember> members) {
    const auto isDynamicCollection = [members](std::string_view name) { return std::ranges::any_of(members, [name](const ReflectedMember& member) { return member.role == ReflectedMember::Role::portCollection && !member.inStruct && member.name == name; }); };
    for (const ReflectedMember& member : members) {
        if (member.portCountFor.empty()) {
            continue;
        }
        if (!member.integer) {
            return PortCountForFault::notInteger;
        }
        if (!std::ranges::all_of(member.portCountFor, isDynamicCollection)) {
            return PortCountForFault::noDynamicCollection;
        }
    }
    for (std::size_t member = 0UZ; member < members.size(); ++member) {
        for (std::size_t other = member + 1UZ; other < members.size(); ++other) {
            for (std::string_view collection : members[member].portCountFor) {
                if (std::ranges::find(members[other].portCountFor, collection) != members[other].portCountFor.end()) {
                    return PortCountForFault::sizedTwice;
                }
            }
        }
    }
    return PortCountForFault::none;
}

// the first fault of the block's `PortCountFor` annotations, or none
template<typename TBlock>
[[nodiscard]] consteval PortCountForFault portCountForFault() {
    return firstPortCountForFault(kReflectedMembers<TBlock>);
}

// the place of the first member named like a member of a base block, whose names are reserved, writable or not, or
// kDirectMember; a direct member compares with the reflected members before it, a struct's member with the base's
[[nodiscard]] consteval std::size_t firstBaseNameTaken(std::span<const ReflectedMember> members, std::span<const std::string_view> outerNames, std::size_t baseMemberCount) {
    for (std::size_t flat = 0UZ; flat < members.size(); ++flat) {
        const ReflectedMember& member = members[flat];
        if (!member.inStruct && member.repeatedBase) {
            continue;
        }
        const std::span<const std::string_view> earlier = outerNames.first(member.inStruct ? baseMemberCount : member.outer);
        if (std::ranges::find(earlier, member.name) != earlier.end()) {
            return flat;
        }
    }
    return kDirectMember;
}

// the place of the first member of the block's `Status` named like one of the block's own other members, a setting
// among them, or kDirectMember
[[nodiscard]] consteval std::size_t firstStatusNameTaken(std::span<const ReflectedMember> members) {
    for (std::size_t flat = 0UZ; flat < members.size(); ++flat) {
        const std::string_view name = members[flat].name;
        if (members[flat].inStatus && std::ranges::any_of(members, [name](const ReflectedMember& other) { return !other.inStatus && !other.fromBaseType && other.name == name; })) {
            return flat;
        }
    }
    return kDirectMember;
}

// the place of the first member of a struct Settings or Status named like an earlier member of the same struct, which
// only a struct derived from another can hold, or kDirectMember
[[nodiscard]] consteval std::size_t firstRepeatedStructName(std::span<const ReflectedMember> members) {
    for (std::size_t flat = 0UZ; flat < members.size(); ++flat) {
        const ReflectedMember& member  = members[flat];
        const auto             earlier = members.first(flat);
        if (member.inStruct && std::ranges::any_of(earlier, [&member](const ReflectedMember& other) { return other.inStruct && other.outer == member.outer && other.name == member.name; })) {
            return flat;
        }
    }
    return kDirectMember;
}

// a setting the block declares as a member of its own outside its struct Settings; a base block's settings are its own
// direct members
[[nodiscard]] consteval bool holdsOwnDirectSetting(std::span<const ReflectedMember> members) {
    return std::ranges::any_of(members, [](const ReflectedMember& member) { return member.role == ReflectedMember::Role::setting && !member.inStruct && !member.fromBaseType && !member.repeatedBase; });
}

// a scheduler's block category
template<typename TBlock>
consteval bool isScheduledBlockGroup() {
    if constexpr (requires { TBlock::blockCategory; }) {
        using Category = std::remove_cvref_t<decltype(TBlock::blockCategory)>;
        return TBlock::blockCategory == Category::ScheduledBlockGroup;
    } else {
        return false;
    }
}

// the failed instantiation names the member
template<gr::meta::fixed_string kMemberName>
consteval void refuseBaseMemberName() {
    static_assert(kMemberName.size() == 0UZ, "a block's member takes the name of a setting of its base block, which is reserved: rename it");
}

// the failed instantiation names the member
template<gr::meta::fixed_string kMemberName>
consteval void refuseRepeatedStructName() {
    static_assert(kMemberName.size() == 0UZ, "a member of a struct Settings or Status takes the name of a member of the struct it derives from: rename it");
}

// the failed instantiation names the member
template<gr::meta::fixed_string kMemberName>
consteval void refuseStatusSettingName() {
    static_assert(kMemberName.size() == 0UZ, "a member of struct Status takes the name of a setting of the block: a name is a setting or a status value, never both");
}

} // namespace detail

// a block type that declares settings by a description, and holds their values in one map
template<typename TBlock>
concept DescribedBlock = requires(TBlock& block) {
    { TBlock::settingsDescription() } -> std::same_as<property_map>;
    { block.describedSettings } -> std::convertible_to<property_map&>;
};

namespace detail {

template<typename TBlock>
[[nodiscard]] DeclarationTable buildDeclarationTable() {
    static_assert(!(isScheduledBlockGroup<TBlock>() && holdsStatusStruct<TBlock>()), "a scheduler declares no struct Status");
    constexpr std::span<const ReflectedMember> kMembers   = kReflectedMembers<TBlock>;
    constexpr PortCountForFault                kPortFault = firstPortCountForFault(kMembers);
    static_assert(kPortFault != PortCountForFault::notInteger, "PortCountFor annotates an integer setting");
    static_assert(kPortFault != PortCountForFault::noDynamicCollection, "PortCountFor names a dynamic port collection of the block");
    static_assert(kPortFault != PortCountForFault::sizedTwice, "PortCountFor: two settings size one port collection");
    static_assert(!holdsOwnDirectSetting(kMembers), "a block declares each setting of its own in its struct Settings");
    if constexpr (constexpr std::size_t kTaken = firstBaseNameTaken(kMembers, kOuterNames<TBlock>, baseMemberCountOf<TBlock>()); kTaken != kDirectMember) {
        refuseBaseMemberName<FlatMemberAt<TBlock, kTaken>::kName>();
    }
    if constexpr (constexpr std::size_t kTaken = firstStatusNameTaken(kMembers); kTaken != kDirectMember) {
        refuseStatusSettingName<FlatMemberAt<TBlock, kTaken>::kName>();
    }
    if constexpr (constexpr std::size_t kRepeated = firstRepeatedStructName(kMembers); kRepeated != kDirectMember) {
        refuseRepeatedStructName<FlatMemberAt<TBlock, kRepeated>::kName>();
    }
    if constexpr (DescribedBlock<TBlock>) {
        const property_map description = TBlock::settingsDescription();
        return makeDeclarationTable(std::string(gr::meta::type_name<TBlock>()), kMembers, &description, isScheduledBlockGroup<TBlock>());
    } else {
        return makeDeclarationTable(std::string(gr::meta::type_name<TBlock>()), kMembers, nullptr, isScheduledBlockGroup<TBlock>());
    }
}

template<typename TBlock>
[[nodiscard]] property_map& metaInformationOf(TBlock& block) {
    if constexpr (AnnotatedType<std::remove_cvref_t<decltype(block.meta_information)>>) {
        return block.meta_information.value;
    } else {
        return block.meta_information;
    }
}

// the declarations of TBlock, built at the first call and kept to the program's end
template<typename TBlock>
[[nodiscard]] const DeclarationTable& declarationTable() {
    static const DeclarationTable table = detail::buildDeclarationTable<TBlock>();
    return table;
}

// writes the block's metadata keys from the type's table, and gives each described setting with a default that default
// in `describedSettings` where the map lacks it, the members below a group's count included; a faulty description gets
// no defaults; checks nothing and calls no block hook
template<typename TBlock>
void describe(TBlock& block) {
    const DeclarationTable& table = declarationTable<TBlock>();
    if constexpr (requires { block.meta_information; }) {
        std::string_view blockDescription;
        if constexpr (requires { std::string_view(detail::undecorated(block.description)); }) {
            blockDescription = detail::undecorated(block.description);
        }
        detail::writeMetadata(table, blockDescription, detail::metaInformationOf(block));
    }
    if constexpr (DescribedBlock<TBlock>) {
        detail::fillDescribedDefaults(table, std::addressof(block), block.describedSettings);
    }
}
} // namespace detail

} // namespace gr::settings

#endif // GNURADIO_SETTING_DECLARATION_HPP
