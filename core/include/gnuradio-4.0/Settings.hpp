#ifndef GNURADIO_SETTINGS_HPP
#define GNURADIO_SETTINGS_HPP

#include <array>
#include <chrono>
#include <concepts>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

#include <gnuradio-4.0/AtomicRef.hpp>
#include <gnuradio-4.0/BlockTraits.hpp>
#include <gnuradio-4.0/PmtTypeHelpers.hpp>
#include <gnuradio-4.0/SettingsCtx.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/ValueHelper.hpp>
#include <gnuradio-4.0/annotated.hpp>
#include <gnuradio-4.0/meta/formatter.hpp>
#include <gnuradio-4.0/meta/immutable.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>

namespace gr {

namespace settings {

template<typename T>
constexpr bool isSupportedVectorOrTensorType() {
    if constexpr (gr::meta::vector_type<T> || gr::meta::array_type<T> || is_tensor<T>) {
        using ValueType = typename T::value_type;
        // TODO(follow-up PR): remove pmt::Value as collection element — it bypasses C++ type safety and breaks settings introspection
        return std::is_arithmetic_v<ValueType> || std::is_same_v<ValueType, std::string> || std::is_same_v<ValueType, std::pmr::string> || std::is_same_v<ValueType, std::complex<double>> || std::is_same_v<ValueType, std::complex<float>> || std::is_enum_v<ValueType> || std::is_same_v<ValueType, pmt::Value>;
    } else {
        return false;
    }
}

template<typename T>
constexpr bool isReadableMember() {
    auto isReadableImmutable = [] {
        if constexpr (gr::meta::is_immutable<T>{}) {
            return isReadableMember<typename T::value_type>();
        } else if constexpr (is_annotated<T>{}) {
            return isReadableMember<typename T::value_type>();

        } else {
            return false;
        }
    };
    // TODO(follow-up PR): remove pmt::Value as settings type — it erases type information, prevents validation, and complicates GRC YAML serialisation
    return std::is_arithmetic_v<T> || std::is_same_v<T, std::string> || std::is_same_v<T, std::pmr::string> || isSupportedVectorOrTensorType<T>() || std::is_same_v<T, property_map> //
           || std::is_same_v<T, std::complex<double>> || std::is_same_v<T, std::complex<float>> || std::is_enum_v<T> || std::is_same_v<T, pmt::Value> || isReadableImmutable();
}

template<typename T, typename TMember>
constexpr bool isWritableMember() {
    return isReadableMember<T>() && !std::is_const_v<T> && !std::is_const_v<TMember> && !gr::meta::is_immutable<TMember>{};
}

inline constexpr uint64_t convertTimePointToUint64Ns(const std::chrono::time_point<std::chrono::system_clock>& tp) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return static_cast<uint64_t>(ns);
}

inline auto nullMatchPred = [](auto, auto, auto) { return std::nullopt; };

} // namespace settings

struct ApplyStagedParametersResult {
    property_map forwardParameters; // parameters that should be forwarded to dependent child blocks
    property_map appliedParameters;
    property_map failedParameters; // staged values the block's setter rejected -- neither applied nor forwarded
};

namespace detail {

#if defined(__EMSCRIPTEN__) || (defined(__APPLE__) && defined(__aarch64__))
template<typename TValue>
auto castToGrSizeIfNeeded(const TValue& value) {
    if constexpr (std::is_same_v<TValue, std::size_t>) {
        return static_cast<gr::Size_t>(value);
    } else {
        return value;
    }
};
#else
template<typename TValue>
auto castToGrSizeIfNeeded(const TValue& value) {
    return value;
};
#endif

template<typename T>
auto unwrap_decorated_value(const T& value) {
    if constexpr (AnnotatedType<T>) {
        return castToGrSizeIfNeeded(value.value);
    } else if constexpr (meta::ImmutableType<T>) {
        return castToGrSizeIfNeeded(value.value());
    } else {
        return castToGrSizeIfNeeded(value);
    }
}

template<typename T>
const auto& unwrap_decorated_reference(const T& value) {
    if constexpr (AnnotatedType<T>) {
        return value.value;
    } else if constexpr (meta::ImmutableType<T>) {
        return value.value();
    } else {
        return value;
    }
};

template<typename TCollection>
auto collectionToTensor(const TCollection& collection) {
    using TValue       = typename TCollection::value_type;
    using TTensorValue = std::conditional_t<std::is_same_v<std::string, TValue> || std::is_same_v<std::pmr::string, TValue>, pmt::Value, TValue>;
    Tensor<TTensorValue> result(extents_from, {collection.size()});
    std::ranges::copy(collection, result.begin());
    return result;
}

template<typename T, typename U = unwrap_if_wrapped_t<std::remove_cvref_t<T>>>
constexpr bool isEnumOrAnnotatedEnum = std::is_enum_v<U>;

template<typename T>
requires isEnumOrAnnotatedEnum<T>
std::expected<T, std::string> tryExtractEnumValue(const pmt::Value& pmt, std::string_view key) {

    auto str = pmt.value_or(std::string_view{});
    if (str.data() == nullptr) {
        return std::unexpected(std::format("Field '{}' expects enum string, got different type", key));
    }

    if (auto opt = gr::meta::parseEnum<T>(str); opt.has_value()) {
        return *opt;
    }

    return std::unexpected(std::format("Invalid enum value '{}' for key '{}'", str, key));
}

template<typename T, typename U = std::remove_cvref_t<T>>
requires isEnumOrAnnotatedEnum<U>
std::string enumToString(T&& enum_value) {
    if constexpr (is_annotated<U>()) {
        return std::string(gr::meta::enumName(enum_value.value).value_or(""));
    } else {
        return std::string(gr::meta::enumName(enum_value).value_or(""));
    }
}

} // namespace detail

/**
 * @brief a concept verifying whether a processing block optionally provides a `settingsChanged` callback to react to
 * block configuration changes and/or to influence forwarded downstream parameters.
 *
 * Implementers may have:
 * 1. `settingsChanged(oldSettings, newSettings)`
 * 2. `settingsChanged(oldSettings, newSettings, forwardSettings)`
 *    - where `forwardSettings` is for influencing subsequent blocks. E.g., a decimating block might adjust the `sample_rate` for downstream blocks.
 */
template<typename BlockType>
concept HasSettingsChangedCallback = requires(BlockType* block, const property_map& oldSettings, property_map& newSettings) {
    { block->settingsChanged(oldSettings, newSettings) };
} or requires(BlockType* block, const property_map& oldSettings, property_map& newSettings, property_map& forwardSettings) {
    { block->settingsChanged(oldSettings, newSettings, forwardSettings) };
};

/**
 * @brief a concept verifying whether a processing block optionally provides a `reset` callback to react to
 * block reset requests (being called after the settings have been reverted(.
 */
template<typename TBlock>
concept HasSettingsResetCallback = requires(TBlock* block) {
    { block->reset() };
};

namespace settings {
/**
 * @brief Convert the given `value` to type `T`. If conversion fails or return diagnostic text.
 */
template<typename T>
[[nodiscard]] std::expected<T, std::string> convertParameter(std::string_view key, const pmt::Value& value) {
    if constexpr (std::is_enum_v<T>) {
        return detail::tryExtractEnumValue<T>(value, key);
    } else {
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::pmr::string>) {
            auto sv = value.value_or(std::string_view{});
            if (sv.data()) {
                return T(sv);
            } else {
                return std::unexpected(std::format("value {} for key '{}' has wrong type {} {}, needs {}", value, key, value.value_type(), value.container_type(), std::string(meta::type_name<T>())));
            }

        } else if constexpr (meta::array_or_vector_type<T>) {
            using TValue            = typename T::value_type;
            using TTensorElem       = std::conditional_t<std::is_same_v<TValue, std::string> || std::is_same_v<TValue, std::pmr::string>, pmt::Value, TValue>;
            const auto* tensorValue = value.get_if<Tensor<TTensorElem>>();
            if (!tensorValue) {
                return std::unexpected(std::format("Value {} is not a tensor of {}", value, meta::type_name<TTensorElem>()));
            }
            T converted;
            if constexpr (meta::array_type<T>) {
                if (tensorValue->size() != std::tuple_size_v<T>) {
                    return std::unexpected(std::format("tensor size {} does not match array size {}", tensorValue->size(), std::tuple_size_v<T>));
                }
            } else {
                converted.resize(tensorValue->size());
            }
            if constexpr (std::is_same_v<TValue, std::string> || std::is_same_v<TValue, std::pmr::string>) {
                std::ranges::transform(*tensorValue, converted.begin(), [](const pmt::Value& in) { return TValue(in.value_or(std::string_view{})); });
            } else {
                std::ranges::copy(*tensorValue, converted.begin());
            }
            return converted;

        } else {
            constexpr bool strictChecks = false;
            auto           converted    = pmt::convert_safely<T, strictChecks>(value);
            if (!converted) {
                return std::unexpected(std::format("value {} for key '{}' has wrong type {} {}, needs {}", value, key, value.value_type(), value.container_type(), std::string(meta::type_name<T>())));
            }
            return *converted;
        }
    }
}

} // namespace settings

namespace detail {
// Free templates (not CtxSettings<TBlock>::) so the bodies instantiate once per Type
// instead of once per (TBlock, Type). Deliberately not `inline`: an explicit instantiation
// declaration does not suppress the implicit instantiation of an inline function, and the
// list at the end of this header exists to keep these bodies out of every block's TU.
template<typename Type>
std::optional<std::string> setParameterImpl(std::string_view key, const pmt::Value& value, property_map& newParameters) {
    if (auto convertedValue = settings::convertParameter<Type>(key, value); convertedValue) [[likely]] {
        const auto keyStr = std::pmr::string(key);
        if constexpr (detail::isEnumOrAnnotatedEnum<Type>) {
            newParameters.insert_or_assign(keyStr, detail::enumToString(convertedValue.value()));
        } else if constexpr (meta::array_or_vector_type<Type>) {
            newParameters.insert_or_assign(keyStr, pmt::Value(detail::collectionToTensor(*convertedValue)));
        } else {
            newParameters.insert_or_assign(keyStr, detail::castToGrSizeIfNeeded(convertedValue.value()));
        }
        return std::nullopt;
    } else {
        return convertedValue.error();
    }
}

template<typename Type>
bool autoUpdateImpl(std::string_view key, const pmt::Value& value, const std::set<std::string>& autoUpdateParams, property_map& stagedParameters) {
    const auto keyStr = std::string(key);
    if (!autoUpdateParams.contains(keyStr)) {
        return false;
    }
    const auto keyPmr = std::pmr::string(key);
    if constexpr (std::is_enum_v<Type>) {
        if (value.holds<std::string>()) {
            stagedParameters.insert_or_assign(keyPmr, value);
            return true;
        }
#ifdef __EMSCRIPTEN__
    } else if constexpr (std::is_same_v<Type, std::size_t> && !std::is_same_v<std::size_t, gr::Size_t>) {
        if (value.holds<gr::Size_t>()) {
            stagedParameters.insert_or_assign(keyPmr, value);
            return true;
        }
#endif
    } else if constexpr (std::is_same_v<Type, std::string> || std::is_same_v<Type, std::pmr::string>) {
        if (value.holds<std::pmr::string>()) {
            stagedParameters.insert_or_assign(keyPmr, value);
            return true;
        }
    } else if constexpr (meta::array_or_vector_type<Type>) {
        using TValue      = typename Type::value_type;
        using TTensorElem = std::conditional_t<std::is_same_v<TValue, std::string> || std::is_same_v<TValue, std::pmr::string>, std::pmr::string, TValue>;
        if (value.holds<Tensor<TTensorElem>>()) {
            auto converted = pmt::convertTo<Tensor<TTensorElem>>(value);
            if (converted.has_value()) {
                stagedParameters.insert_or_assign(keyPmr, std::move(*converted));
                return true;
            }
            return false;
        }
    } else {
        if (value.holds<Type>()) {
            stagedParameters.insert_or_assign(keyPmr, value);
            return true;
        }
        if constexpr (std::is_arithmetic_v<Type> && !std::is_same_v<Type, bool>) {
            if (const auto converted = pmt::convert_numerically<Type>(value); converted) {
                stagedParameters.insert_or_assign(keyPmr, detail::castToGrSizeIfNeeded(*converted));
                return true;
            }
        }
    }
    return false;
}
} // namespace detail

namespace settings {

/**
 * @brief Convert a staged `pmt::Value` to the member type `Type` on the apply path.
 *
 * One instantiation per member type, shared by every block declaring a member of that type,
 * rather than one per (block, member).
 */
template<typename Type>
[[nodiscard]] std::expected<Type, std::string> extractStagedValue(const pmt::Value& stagedValue, std::string_view key) {
    std::expected<Type, std::string> maybeValue;
    if constexpr (std::is_enum_v<Type>) {
        maybeValue = detail::tryExtractEnumValue<Type>(stagedValue, key);
    } else if constexpr (std::is_same_v<Type, std::string> || std::is_same_v<Type, std::pmr::string>) {
        auto str = stagedValue.value_or(std::string_view{});
        if (str.data() != nullptr) {
            maybeValue = Type(str);
        } else {
            maybeValue = std::unexpected("Unexpected type in stagedValue");
        }
    } else if constexpr (meta::array_or_vector_type<Type>) {
        using TValue       = typename Type::value_type;
        using TTensorValue = std::conditional_t<std::is_same_v<std::string, TValue> || std::is_same_v<std::pmr::string, TValue>, pmt::Value, TValue>;
        auto tensor        = checked_access_ptr{stagedValue.get_if<Tensor<TTensorValue>>()};
        if (tensor != nullptr) {
            maybeValue = settings::convertParameter<Type>(key, stagedValue);
        } else {
            maybeValue = std::unexpected("Unexpected type in stagedValue");
        }
#ifdef __EMSCRIPTEN__
    } else if constexpr (std::is_same_v<Type, std::size_t> && !std::is_same_v<std::size_t, gr::Size_t>) {
        auto ptr = checked_access_ptr{stagedValue.get_if<gr::Size_t>()};
        if (ptr != nullptr) {
            maybeValue = static_cast<std::size_t>(*ptr);
        } else {
            maybeValue = std::unexpected("Unexpected type in stagedValue");
        }
#endif
    } else {
        auto ptr = checked_access_ptr{stagedValue.get_if<Type>()};
        if (ptr != nullptr) {
            maybeValue = *ptr;
        } else {
            maybeValue = std::unexpected("Unexpected type in stagedValue");
        }
    }
    return maybeValue;
}

// the type-independent tail of the apply path, compiled once in Settings.cpp
[[nodiscard]] bool recordAppliedValue(std::string_view key, const pmt::Value& stagedValue, property_map& appliedParameters, property_map& stagedForCallback, bool hasSettingsChangedCallback);
void               reportValidationFailure(std::string_view key, const pmt::Value& value);
void               reportConversionFailure(std::string_view key, std::string_view error);

/**
 * @brief One reflected member of a block, as the compiled settings machinery sees it.
 *
 * The accessors reach the block through an erased pointer, so the only code a block type
 * contributes per member is the accessor pair; everything that reads this table is compiled
 * once in Settings.cpp. A null accessor means the member does not take part in that path:
 * `setParameter`/`autoUpdate`/`applyStaged` are set for writable members, `readParameter`
 * for readable ones.
 */
struct MemberDescriptor {
    using ParameterSetter   = std::optional<std::string> (*)(std::string_view key, const pmt::Value& value, property_map& newParameters);
    using AutoUpdateHandler = bool (*)(std::string_view key, const pmt::Value& value, const std::set<std::string>& autoUpdateParams, property_map& stagedParameters);
    using StagedApplier     = bool (*)(void* block, std::string_view key, const pmt::Value& stagedValue, property_map& appliedParameters, property_map& stagedForCallback, bool hasSettingsChangedCallback);
    using ParameterReader   = void (*)(const void* block, std::string_view key, property_map& parameters);
    using TypeNameReader    = std::string (*)();

    std::string_view name{};

    ParameterSetter   setParameter{nullptr};
    AutoUpdateHandler autoUpdate{nullptr};
    StagedApplier     applyStaged{nullptr};
    ParameterReader   readParameter{nullptr};

    bool             isAnnotated{false};
    std::string_view description{};
    std::string_view documentation{};
    std::string_view unit{};
    bool             visible{false};

    bool                              isEnum{false};
    TypeNameReader                    enumTypeName{nullptr};
    std::span<const std::string_view> enumValueNames{};
};

/**
 * @brief What a block type contributes to the settings machinery beyond its member list:
 * the optional user callbacks and the framework members reached through them.
 */
struct BlockHooks {
    using MetaInformationAccessor = property_map& (*)(void* block);
    using DescriptionReader       = std::string_view (*)(const void* block);
    using SettingsChangedInvoker  = void (*)(void* block, property_map& oldSettings, property_map& newSettings, property_map& forwardSettings);
    using ResetInvoker            = void (*)(void* block);
    using ChunkRatioReader        = bool (*)(const void* block, float& ratio);

    std::span<const MemberDescriptor> members{};
    bool                              reflectable{false};

    MetaInformationAccessor metaInformation{nullptr};
    DescriptionReader       blockDescription{nullptr};
    SettingsChangedInvoker  settingsChanged{nullptr};
    ResetInvoker            reset{nullptr};
    ChunkRatioReader        chunkRatio{nullptr};
};

/**
 * @brief The per-block-type settings table: the reflected member list, the block's hooks and
 * the lookups derived from them once. One instance per block type, reached through
 * `settings::blockDescriptor<TBlock>()`.
 */
struct BlockDescriptor {
    explicit BlockDescriptor(const BlockHooks& blockHooks);

    BlockHooks                                                    hooks;
    std::set<std::string>                                         writableMembers{};
    std::unordered_map<std::string_view, const MemberDescriptor*> writableByName{};
    std::vector<const MemberDescriptor*>                          readableMembers{};
};

} // namespace settings

namespace detail {

template<typename TBlock, std::size_t kIdx, typename RawType, typename Type>
bool applyStagedMember(void* block, std::string_view key, const pmt::Value& stagedValue, property_map& appliedParameters, property_map& stagedForCallback, bool hasSettingsChangedCallback) {
    auto& member = refl::data_member<kIdx>(*static_cast<TBlock*>(block));

    std::expected<Type, std::string> maybeValue = settings::extractStagedValue<Type>(stagedValue, key);

    if constexpr (is_annotated<RawType>()) {
        if (maybeValue && member.validate_and_set(*maybeValue)) {
            return settings::recordAppliedValue(key, stagedValue, appliedParameters, stagedForCallback, hasSettingsChangedCallback);
        }
        settings::reportValidationFailure(key, stagedValue);
        return false;
    } else {
        if (!maybeValue) {
            settings::reportConversionFailure(key, maybeValue.error());
            return false;
        }
        member = *maybeValue;
        return settings::recordAppliedValue(key, stagedValue, appliedParameters, stagedForCallback, hasSettingsChangedCallback);
    }
}

template<typename TBlock, std::size_t kIdx, typename RawType, typename Type>
void readMember(const void* block, std::string_view key, property_map& parameters) {
    const auto  keyPmr = std::pmr::string(key);
    const auto& member = refl::data_member<kIdx>(*static_cast<const TBlock*>(block));
    if constexpr (detail::isEnumOrAnnotatedEnum<RawType>) {
        parameters.insert_or_assign(keyPmr, detail::enumToString(member));
    } else if constexpr (meta::array_or_vector_type<Type>) {
        parameters.insert_or_assign(keyPmr, pmt::Value(detail::collectionToTensor(detail::unwrap_decorated_reference(member))));
    } else {
        parameters.insert_or_assign(keyPmr, detail::unwrap_decorated_value(member));
    }
}

template<typename TBlock>
property_map& blockMetaInformation(void* block) {
    auto& metaInformation = static_cast<TBlock*>(block)->meta_information;
    if constexpr (AnnotatedType<std::remove_cvref_t<decltype(metaInformation)>>) {
        return metaInformation.value;
    } else {
        return metaInformation;
    }
}

template<typename TBlock>
std::string_view readBlockDescription(const void* block) {
    return detail::unwrap_decorated_reference(static_cast<const TBlock*>(block)->description);
}

template<typename TBlock>
void invokeSettingsChanged(void* block, property_map& oldSettings, property_map& newSettings, [[maybe_unused]] property_map& forwardSettings) {
    TBlock* self = static_cast<TBlock*>(block);
    if constexpr (requires { self->settingsChanged(oldSettings, newSettings); }) {
        self->settingsChanged(oldSettings, newSettings);
    } else if constexpr (requires { self->settingsChanged(oldSettings, newSettings, forwardSettings); }) {
        self->settingsChanged(oldSettings, newSettings, forwardSettings);
    }
}

template<typename TBlock>
void invokeReset(void* block) {
    static_cast<TBlock*>(block)->reset();
}

template<typename TBlock>
bool readChunkRatio(const void* block, float& ratio) {
    const TBlock* self = static_cast<const TBlock*>(block);
    if (self->input_chunk_size == 1ULL && self->output_chunk_size == 1ULL) {
        return false;
    }
    ratio = static_cast<float>(self->output_chunk_size) / static_cast<float>(self->input_chunk_size);
    return true;
}

} // namespace detail

namespace settings {

template<typename TEnum>
inline constexpr auto kEnumValueNames = [] {
    constexpr auto                              values = gr::meta::enumValues<TEnum>();
    std::array<std::string_view, values.size()> names{};
    for (std::size_t i = 0UZ; i < values.size(); ++i) {
        names[i] = gr::meta::enumName(values[i]).value_or(std::string_view{});
    }
    return names;
}();

template<typename TBlock>
inline constexpr auto kMemberDescriptors = [] {
    std::array<MemberDescriptor, refl::data_member_count<TBlock>> table{};
    if constexpr (refl::reflectable<TBlock>) {
        refl::for_each_data_member_index<TBlock>([&table](auto kIdx) {
            using MemberType = refl::data_member_type<TBlock, kIdx>;
            using RawType    = std::remove_cvref_t<MemberType>;
            using Type       = unwrap_if_wrapped_t<RawType>;

            MemberDescriptor& entry = table[kIdx];
            entry.name              = refl::data_member_name<TBlock, kIdx>.view();

            if constexpr (settings::isReadableMember<Type>()) {
                entry.readParameter = &detail::readMember<TBlock, kIdx, RawType, Type>;
            }
            if constexpr (settings::isWritableMember<Type, MemberType>()) {
                entry.setParameter = &detail::setParameterImpl<Type>;
                entry.autoUpdate   = &detail::autoUpdateImpl<Type>;
                entry.applyStaged  = &detail::applyStagedMember<TBlock, kIdx, RawType, Type>;
            }
            if constexpr (AnnotatedType<RawType>) {
                entry.isAnnotated   = true;
                entry.description   = RawType::description();
                entry.documentation = RawType::documentation();
                entry.unit          = RawType::unit();
                entry.visible       = RawType::visible();
            }
            if constexpr (std::is_enum_v<Type>) {
                entry.isEnum         = true;
                entry.enumTypeName   = &gr::meta::type_name<Type>;
                entry.enumValueNames = std::span<const std::string_view>(kEnumValueNames<Type>);
            }
        });
    }
    return table;
}();

template<typename TBlock>
[[nodiscard]] constexpr BlockHooks makeBlockHooks() {
    BlockHooks hooks;
    hooks.members     = std::span<const MemberDescriptor>(kMemberDescriptors<TBlock>);
    hooks.reflectable = refl::reflectable<TBlock>;

    constexpr bool hasMetaInformation = requires(TBlock block) {
        {
            unwrap_if_wrapped_t<decltype(block.meta_information)> {}
        } -> std::same_as<property_map>;
    };
    if constexpr (hasMetaInformation) {
        hooks.metaInformation = &detail::blockMetaInformation<TBlock>;
        if constexpr (requires(TBlock block) { block.description; }) {
            hooks.blockDescription = &detail::readBlockDescription<TBlock>;
        }
    }
    if constexpr (HasSettingsChangedCallback<TBlock>) {
        hooks.settingsChanged = &detail::invokeSettingsChanged<TBlock>;
    }
    if constexpr (HasSettingsResetCallback<TBlock>) {
        hooks.reset = &detail::invokeReset<TBlock>;
    }
    if constexpr (requires { TBlock::ResamplingControl::kEnabled; }) {
        if constexpr (TBlock::ResamplingControl::kEnabled) {
            hooks.chunkRatio = &detail::readChunkRatio<TBlock>;
        }
    }
    return hooks;
}

template<typename TBlock>
[[nodiscard]] const BlockDescriptor& blockDescriptor() {
    static const BlockDescriptor descriptor{makeBlockHooks<TBlock>()};
    return descriptor;
}

} // namespace settings

struct SettingsBase {
    struct CtxSettingsPair {
        SettingsCtx  context;
        property_map settings;
    };
    virtual ~SettingsBase() = default;

    /**
     * @brief returns if there are stored settings that haven't been applied yet.
     */
    [[nodiscard]] virtual bool changed() const noexcept    = 0;
    virtual void               setChanged(bool b) noexcept = 0;

    /**
     * @brief Set initial parameters provided in the Block constructor to ensure they are available during Settingd::init()
     */
    virtual void setInitBlockParameters(const property_map& parameters) = 0;

    /**
     * @brief initialize settings, set init parameters provided in the Block constructor
     */
    virtual void init() = 0;

    /**
     * @brief Add new key-value pairs to stored parameters.
     * N.B. settings become staged after calling activateContext(), and after executing 'applyStagedParameters()' settings are applied (usually done early on in the 'Block::work()' function)
     * @return key-value pairs that could not be set
     */
    [[nodiscard]] virtual property_map set(const property_map& parameters, SettingsCtx ctx = {}) = 0;

    /**
     * @brief Add new key-value pairs to stagedParameters. The changes do not affect storedParameters.
     * @return key-value pairs that could not be set
     */
    [[nodiscard]] virtual property_map setStaged(const property_map& parameters) = 0;

    virtual void storeDefaults() = 0;
    virtual void resetDefaults() = 0;

    /**
     * @brief return the name of the active context
     */
    [[nodiscard]] virtual const SettingsCtx& activeContext() const noexcept = 0;

    /**
     * @brief removes the given context
     * @return true on success
     */
    [[nodiscard]] virtual bool removeContext(SettingsCtx ctx) = 0;

    /**
     * @brief Set new activate context and set staged parameters
     * @return best match context or std::nullopt if best match context is not found in storage
     */
    [[nodiscard]] virtual std::optional<SettingsCtx> activateContext(SettingsCtx ctx = {}) = 0;

    /**
     * @brief updates parameters based on block input tags for those with keys stored in `autoUpdateParameters()`
     * Parameter changes to down-stream blocks is controlled via `autoForwardParameters()`
     */
    virtual void autoUpdate(const Tag& tag) = 0;

    /**
     * @brief return all (or for selected multiple keys) available active block settings as key-value pairs
     */
    [[nodiscard]] virtual property_map get(std::span<const std::string> parameter_keys = {}) const noexcept = 0;

    /**
     * @brief return available active block setting as key-value pair for a single key
     */
    [[nodiscard]] virtual std::optional<pmt::Value> get(const std::string& parameter_key) const noexcept = 0;

    /**
     * @brief return all (or for selected multiple keys) stored block settings for provided context as key-value pairs
     */
    [[nodiscard]] virtual std::optional<property_map> getStored(std::span<const std::string> parameterKeys = {}, SettingsCtx ctx = {}) const noexcept = 0;

    /**
     * @brief return available stored block setting for provided context as key-value pair for a single key
     */
    [[nodiscard]] virtual std::optional<pmt::Value> getStored(const std::string& parameter_key, SettingsCtx ctx = {}) const noexcept = 0;

    /**
     * @brief return number of all sets of stored parameters
     */
    [[nodiscard]] virtual gr::Size_t getNStoredParameters() const noexcept = 0;

    /**
     * @brief return number of sets of auto update parameters
     */
    [[nodiscard]] virtual gr::Size_t getNAutoUpdateParameters() const noexcept = 0;

    /**
     * @brief return _storedParameters
     */
    [[nodiscard]] virtual std::map<pmt::Value, std::vector<CtxSettingsPair>, settings::PMTCompare> getStoredAll() const noexcept = 0;

    /**
     * @brief returns the staged/not-yet-applied new parameters
     */
    [[nodiscard]] virtual property_map stagedParameters() const = 0;

    [[nodiscard]] virtual std::set<std::string> autoUpdateParameters(SettingsCtx ctx = {}) noexcept = 0;

    // N.B. by reference: fixed once the block runs, and read on the per-work() tag-forwarding
    // path where a set copy costs more than the whole work() call
    [[nodiscard]] virtual const std::set<std::string>& autoForwardParameters() const noexcept = 0;

    /**
     * @brief add keys a block forwards to its children beyond the default tag set.
     *
     * The set is read unlocked on the per-work() path, so it is fixed for the duration of a
     * run: add to it while the block is not running -- at construction, or while the graph is
     * being built.
     */
    virtual void addAutoForwardParameters(std::set<std::string> parameterKeys) = 0;

    [[nodiscard]] virtual property_map defaultParameters() const noexcept = 0;

    [[nodiscard]] virtual property_map activeParameters() const noexcept = 0;

    /**
     * @brief synchronise map-based with actual block field-based settings
     * returns map with key-value tags that should be forwarded
     * to dependent/child blocks.
     */
    [[nodiscard]] virtual ApplyStagedParametersResult applyStagedParameters() = 0;

    /**
     * @brief synchronises the map-based with the block's field-based parameters
     * (N.B. usually called after the staged parameters have been synchronised)
     */
    virtual void updateActiveParameters() noexcept = 0;

    /**
     * @brief Loads parameters from a property_map by matching pmt keys to TBlock's writable data members.
     * Handles type conversion and special cases, such as std::vector<bool>.
     */
    virtual void loadParametersFromPropertyMap(const property_map& parameters, SettingsCtx ctx = {}) = 0;

}; // struct SettingsBase

/**
 * @brief Non-templated implementation of the settings machinery.
 *
 * The owning block is reached through an erased pointer and its reflected members through
 * `settings::BlockDescriptor`, so every body below is compiled once in `Settings.cpp` rather
 * than instantiated per block type.
 */
class CtxSettingsBase : public SettingsBase {
public:
    using MatchPredicate = std::function<std::optional<bool>(const pmt::Value&, const pmt::Value&, std::size_t)>;

protected:
    void*                            _block      = nullptr;
    const settings::BlockDescriptor* _descriptor = nullptr;

    mutable bool       _changed{false};
    mutable std::mutex _mutex{};

    // key: SettingsCtx.context, value: queue of parameters with the same SettingsCtx.context but for different time
    mutable std::map<pmt::Value, std::vector<CtxSettingsPair>, settings::PMTCompare> _storedParameters{};
    property_map                                                                     _defaultParameters{};
    property_map                                                                     _initBlockParameters{};
    std::map<SettingsCtx, std::set<std::string>>                                     _autoUpdateParameters{};
    std::set<std::string>                                                            _autoForwardParameters{};
    MatchPredicate                                                                   _matchPred = settings::nullMatchPred;
    SettingsCtx                                                                      _activeCtx{};
    property_map                                                                     _stagedParameters{};
    property_map                                                                     _activeParameters{};

    const std::size_t _timePrecisionTolerance = 100; // ns, now used for emscripten

    CtxSettingsBase(void* block, const settings::BlockDescriptor& descriptor) noexcept;

public:
    // Settings configuration
    std::uint64_t expiry_time{std::numeric_limits<std::uint64_t>::max()};

    [[nodiscard]] const std::set<std::string>& writableMembers() const override;

    [[nodiscard]] bool changed() const noexcept override;
    void               setChanged(bool b) noexcept override;
    void               setInitBlockParameters(const property_map& parameters) override;

    void init() override;

    [[nodiscard]] const SettingsCtx& activeContext() const noexcept override;

    [[nodiscard]] const std::set<std::string>& autoForwardParameters() const noexcept override;
    void                                       addAutoForwardParameters(std::set<std::string> parameterKeys) override;
    [[nodiscard]] property_map                 defaultParameters() const noexcept override;
    [[nodiscard]] property_map                 activeParameters() const noexcept override;

    [[nodiscard]] property_map              get(std::span<const std::string> parameterKeys = {}) const noexcept override;
    [[nodiscard]] std::optional<pmt::Value> get(const std::string& parameterKey) const noexcept override;

    [[nodiscard]] std::optional<property_map> getStored(std::span<const std::string> parameterKeys = {}, SettingsCtx ctx = {}) const noexcept override;
    [[nodiscard]] std::optional<pmt::Value>   getStored(const std::string& parameterKey, SettingsCtx ctx = {}) const noexcept override;

    [[nodiscard]] gr::Size_t getNStoredParameters() const noexcept override;
    [[nodiscard]] gr::Size_t getNAutoUpdateParameters() const noexcept override;

    [[nodiscard]] std::map<pmt::Value, std::vector<CtxSettingsPair>, settings::PMTCompare> getStoredAll() const noexcept override;

    [[nodiscard]] property_map stagedParameters() const override;

    [[nodiscard]] std::set<std::string> autoUpdateParameters(SettingsCtx ctx = {}) noexcept override;

    [[nodiscard]] property_map set(const property_map& parameters, SettingsCtx ctx = {}) override;
    [[nodiscard]] property_map setStaged(const property_map& parameters) override;

    void storeDefaults() override;
    void resetDefaults() override;

    [[nodiscard]] std::optional<SettingsCtx> activateContext(SettingsCtx ctx = {}) override;
    [[nodiscard]] bool                       removeContext(SettingsCtx ctx) override;

    void autoUpdate(const Tag& tag) override;

    [[nodiscard]] ApplyStagedParametersResult applyStagedParameters() override;

    void updateActiveParameters() noexcept override;

    void loadParametersFromPropertyMap(const property_map& parameters, SettingsCtx ctx = {}) override;

    void assignFrom(const CtxSettingsBase& other);
    void assignFrom(CtxSettingsBase&& other) noexcept;

protected:
    // *Impl bodies run without taking _mutex, for callers that already hold it
    [[nodiscard]] property_map setImpl(const property_map& parameters, SettingsCtx ctx);
    [[nodiscard]] property_map setStagedImpl(const property_map& parameters);
    void                       resetDefaultsImpl();
    // reentrantLock is null when a caller already holds _mutex and cannot have it released underneath it
    [[nodiscard]] ApplyStagedParametersResult          applyStagedParametersImpl(std::unique_lock<std::mutex>* reentrantLock = nullptr);
    void                                               updateActiveParametersImpl() noexcept;
    void                                               storeCurrentParameters(property_map& parameters);
    [[nodiscard]] bool                                 isActiveValueImpl(const std::pmr::string& key, const pmt::Value& value) const;
    [[nodiscard]] std::optional<SettingsCtx>           activateContextImpl(SettingsCtx ctx);
    [[nodiscard]] bool                                 removeContextImpl(SettingsCtx ctx);
    [[nodiscard]] std::optional<pmt::Value>            findBestMatchCtx(const pmt::Value& contextToSearch) const;
    [[nodiscard]] std::optional<SettingsCtx>           findBestMatchSettingsCtx(const SettingsCtx& ctx) const;
    [[nodiscard]] std::optional<property_map>          getBestMatchStoredParameters(const SettingsCtx& ctx) const;
    [[nodiscard]] std::optional<std::set<std::string>> getBestMatchAutoUpdateParameters(const SettingsCtx& ctx) const;
    void                                               resolveDuplicateTimestamp(SettingsCtx& ctx);
    void                                               addStoredParameters(const property_map& newParameters, const SettingsCtx& ctx);
    void                                               removeExpiredStoredParameters();
    [[nodiscard]] std::optional<std::string>           contextInTag(const Tag& tag) const;
    [[nodiscard]] std::optional<std::uint64_t>         triggeredTimeInTag(const Tag& tag) const;
    [[nodiscard]] std::optional<SettingsCtx>           createSettingsCtxFromTag(const Tag& tag) const;
}; // class CtxSettingsBase

/**
 * @brief The typed face of the settings machinery: it binds a block to its
 * `settings::BlockDescriptor` and checks the block's optional callback signatures. All behaviour
 * lives in `CtxSettingsBase`.
 */
template<typename TBlock>
class CtxSettings : public CtxSettingsBase {
public:
    [[nodiscard]] static const std::set<std::string>& allWritableMembers() { return settings::blockDescriptor<TBlock>().writableMembers; }

    explicit CtxSettings(TBlock& block, MatchPredicate matchPred = settings::nullMatchPred) noexcept : CtxSettingsBase(std::addressof(block), settings::blockDescriptor<TBlock>()) {
        _matchPred = std::move(matchPred);
        if constexpr (requires { &TBlock::settingsChanged; }) { // if settingsChanged is defined
            static_assert(HasSettingsChangedCallback<TBlock>, "if provided, settingsChanged must have either a `(const property_map& old, property_map& new, property_map& fwd)`"
                                                              "or `(const property_map& old, property_map& new)` paremeter signatures.");
        }

        if constexpr (requires { &TBlock::reset; }) { // if reset is defined
            static_assert(HasSettingsResetCallback<TBlock>, "if provided, reset() may have no function parameters");
        }
    }

    // Not safe as CtxSettings has a pointer back to the block
    // that owns it
    CtxSettings(const CtxSettings& other)            = delete;
    CtxSettings(CtxSettings&& other)                 = delete;
    CtxSettings& operator=(const CtxSettings& other) = delete;
    CtxSettings& operator=(CtxSettings&& other)      = delete;

    CtxSettings(TBlock& block, const CtxSettings& other) : CtxSettingsBase(std::addressof(block), settings::blockDescriptor<TBlock>()) { assignFrom(other); }

    CtxSettings(TBlock& block, CtxSettings&& other) noexcept : CtxSettingsBase(std::addressof(block), settings::blockDescriptor<TBlock>()) { assignFrom(std::move(other)); }
}; // class CtxSettings

} // namespace gr

/**
 * The settings machinery a member contributes depends on the member's TYPE, never on the block
 * declaring it, so the four leaves below are compiled once in Settings.cpp for the types the
 * framework and the block library actually use. Every block reflects the eight members
 * `Block<>` declares on its behalf, so without this list every block's translation unit paid
 * for the bool/string/property_map/Size_t conversion tier again.
 *
 * The list is an optimisation, not a contract: a member of an unlisted type instantiates its
 * leaves in the using translation unit exactly as before.
 */
// clang-format off
#define GR_SETTINGS_MEMBER_TYPES \
    X(bool)                      \
    X(std::int8_t)               \
    X(std::int16_t)              \
    X(std::int32_t)              \
    X(std::int64_t)              \
    X(std::uint8_t)              \
    X(std::uint16_t)             \
    X(std::uint32_t)             \
    X(std::uint64_t)             \
    X(float)                     \
    X(double)                    \
    X(std::complex<float>)       \
    X(std::complex<double>)      \
    X(std::string)               \
    X(std::pmr::string)          \
    X(gr::property_map)

namespace gr {

#define X(T)                                                                                                                       \
    extern template std::optional<std::string> detail::setParameterImpl<T>(std::string_view, const pmt::Value&, property_map&);     \
    extern template bool detail::autoUpdateImpl<T>(std::string_view, const pmt::Value&, const std::set<std::string>&, property_map&); \
    extern template std::expected<T, std::string> settings::convertParameter<T>(std::string_view, const pmt::Value&);               \
    extern template std::expected<T, std::string> settings::extractStagedValue<T>(const pmt::Value&, std::string_view);

GR_SETTINGS_MEMBER_TYPES
#undef X

} // namespace gr
// clang-format on

namespace std {
template<>
struct hash<gr::SettingsCtx> {
    [[nodiscard]] size_t operator()(const gr::SettingsCtx& ctx) const noexcept { return ctx.hash(); }
};
} // namespace std

#endif // GNURADIO_SETTINGS_HPP
