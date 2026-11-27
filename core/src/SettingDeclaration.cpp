#include <gnuradio-4.0/SettingDeclaration.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <variant>
#include <version>

#if !defined(__cpp_lib_to_chars) && !(defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 200000)
#include <locale>
#include <sstream>
#endif

namespace gr::settings::detail {

namespace {

using ValueType     = pmt::Value::ValueType;
using ContainerType = pmt::Value::ContainerType;

struct ElementName {
    ValueType        element;
    std::string_view name;
};

constexpr std::array<ElementName, 14UZ> kElementNames{{
    {ValueType::Bool, "bool"},
    {ValueType::Int8, "int8"},
    {ValueType::Int16, "int16"},
    {ValueType::Int32, "int32"},
    {ValueType::Int64, "int64"},
    {ValueType::UInt8, "uint8"},
    {ValueType::UInt16, "uint16"},
    {ValueType::UInt32, "uint32"},
    {ValueType::UInt64, "uint64"},
    {ValueType::Float32, "float32"},
    {ValueType::Float64, "float64"},
    {ValueType::ComplexFloat32, "complex32"},
    {ValueType::ComplexFloat64, "complex64"},
    {ValueType::String, "str"},
}};

constexpr std::string_view kVectorSuffix = "_vector";

[[nodiscard]] std::string_view elementName(ValueType element) {
    const auto it = std::ranges::find(kElementNames, element, &ElementName::element);
    return it != kElementNames.end() ? it->name : std::string_view{"any"};
}

[[nodiscard]] ContainerType scalarContainer(ValueType element) {
    if (element == ValueType::ComplexFloat32 || element == ValueType::ComplexFloat64) {
        return ContainerType::Complex;
    }
    return element == ValueType::String ? ContainerType::String : ContainerType::Scalar;
}

// calls `visitor` with `std::type_identity<T>` for the C++ type of a number, bool or complex element, else with
// `std::type_identity<std::monostate>`
template<typename TVisitor>
decltype(auto) withElementType(ValueType element, TVisitor&& visitor) {
    switch (element) {
    case ValueType::Bool: return visitor(std::type_identity<bool>{});
    case ValueType::Int8: return visitor(std::type_identity<std::int8_t>{});
    case ValueType::Int16: return visitor(std::type_identity<std::int16_t>{});
    case ValueType::Int32: return visitor(std::type_identity<std::int32_t>{});
    case ValueType::Int64: return visitor(std::type_identity<std::int64_t>{});
    case ValueType::UInt8: return visitor(std::type_identity<std::uint8_t>{});
    case ValueType::UInt16: return visitor(std::type_identity<std::uint16_t>{});
    case ValueType::UInt32: return visitor(std::type_identity<std::uint32_t>{});
    case ValueType::UInt64: return visitor(std::type_identity<std::uint64_t>{});
    case ValueType::Float32: return visitor(std::type_identity<float>{});
    case ValueType::Float64: return visitor(std::type_identity<double>{});
    case ValueType::ComplexFloat32: return visitor(std::type_identity<std::complex<float>>{});
    case ValueType::ComplexFloat64: return visitor(std::type_identity<std::complex<double>>{});
    default: return visitor(std::type_identity<std::monostate>{});
    }
}

using ParsedNumber = std::variant<std::int64_t, std::uint64_t, double>;

template<typename T>
[[nodiscard]] bool parsesWhole(std::string_view text, T& out) {
    const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// a decimal number: a sign, digits with at most one point, and an exponent; no space, no hexadecimal, no infinity
[[nodiscard]] bool isDecimalNumber(std::string_view text) {
    const auto  isDigit  = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t position = 0UZ;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        ++position;
    }
    std::size_t digits = 0UZ;
    for (; position < text.size() && isDigit(text[position]); ++position) {
        ++digits;
    }
    if (position < text.size() && text[position] == '.') {
        for (++position; position < text.size() && isDigit(text[position]); ++position) {
            ++digits;
        }
    }
    if (digits == 0UZ) {
        return false;
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const std::size_t exponentStart = position;
        while (position < text.size() && isDigit(text[position])) {
            ++position;
        }
        if (position == exponentStart) {
            return false;
        }
    }
    return position == text.size();
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\n\v\f\r";
    const std::size_t          first       = text.find_first_not_of(kWhitespace);
    return first == std::string_view::npos ? std::string_view{} : text.substr(first, text.find_last_not_of(kWhitespace) - first + 1UZ);
}

// whether a decimal number's digits ahead of its exponent hold one other than 0
[[nodiscard]] bool isNonzero(std::string_view number) {
    return std::ranges::any_of(number.substr(0UZ, number.find_first_of("eE")), [](char c) { return c >= '1' && c <= '9'; });
}

// a decimal number as a double, read alike under every locale; nothing when it lies outside the double's range
[[nodiscard]] std::optional<double> parseDouble(std::string_view number) {
#if defined(__cpp_lib_to_chars) || (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 200000)
    double value{};
    return parsesWhole(number, value) ? std::optional<double>(value) : std::nullopt;
#else
    // the classic locale reads a point as the decimal point whatever locale the application sets
    std::istringstream stream{std::string(number)};
    stream.imbue(std::locale::classic());
    double value{};
    stream >> value;
    return !stream.fail() && stream.eof() ? std::optional<double>(value) : std::nullopt;
#endif
}

// a text read whole as a number, with the whitespace around it dropped
[[nodiscard]] std::expected<ParsedNumber, std::string_view> parseNumber(std::string_view text) {
    std::string_view number = trimmed(text);
    if (!isDecimalNumber(number)) {
        return std::unexpected(std::string_view("not a decimal number"));
    }
    if (number.front() == '+') { // std::from_chars takes no plus sign
        number.remove_prefix(1UZ);
    }
    if (std::int64_t signedValue{}; parsesWhole(number, signedValue)) {
        return signedValue;
    }
    if (std::uint64_t unsignedValue{}; parsesWhole(number, unsignedValue)) {
        return unsignedValue;
    }
    if (const std::optional<double> floating = parseDouble(number); floating) {
        return *floating;
    }
    return std::unexpected(detail::kOutsideType);
}

template<typename T>
[[nodiscard]] std::expected<T, std::string> textAsNumber(std::string_view text) {
    const std::expected<ParsedNumber, std::string_view> parsed = parseNumber(text);
    if (!parsed) {
        return std::unexpected(std::string(parsed.error()));
    }
    const std::expected<T, std::string_view> converted = std::visit([](const auto& number) { return detail::nearestNumber<T>(number); }, *parsed);
    if (!converted) {
        return std::unexpected(std::string(converted.error()));
    }
    bool outsideRange = *converted == T{} && isNonzero(trimmed(text));
    if constexpr (std::floating_point<T>) {
        outsideRange = outsideRange || std::isinf(*converted);
    }
    if (outsideRange) {
        return std::unexpected(std::string(detail::kOutsideType));
    }
    return *converted;
}

// `true` and `false` match in any letter case, as `convert_safely` in `PmtTypeHelpers.hpp` matches them
[[nodiscard]] std::expected<bool, std::string> textAsBool(std::string_view text) {
    const auto matches = [text](std::string_view word) { return std::ranges::equal(text, word, [](char sent, char letter) { return std::tolower(static_cast<unsigned char>(sent)) == letter; }); };
    if (matches("true") || text == "1") {
        return true;
    }
    if (matches("false") || text == "0") {
        return false;
    }
    return std::unexpected(std::string("not one of: true, false, 1, 0"));
}

// a name matches the enumerator spelled exactly as it is, so every host sends one spelling
[[nodiscard]] std::optional<std::string_view> enumeratorNamed(const std::vector<std::string>& enumerators, std::string_view name) {
    const auto found = std::ranges::find(enumerators, name);
    return found != enumerators.end() ? std::optional<std::string_view>(*found) : std::nullopt;
}

// the choices as core stores and returns them; an empty choice, such as the notification level's, reads "(or empty)"
[[nodiscard]] std::string noneOfChoices(const Declaration& declaration) {
    std::string choices;
    bool        takesEmpty = false;
    for (const std::string& choice : declaration.choices) {
        if (choice.empty()) {
            takesEmpty = true;
            continue;
        }
        choices += std::format("{}{}", choices.empty() ? "" : ", ", choice);
    }
    return std::format("not one of: {}{}", choices, takesEmpty ? " (or empty)" : "");
}

[[nodiscard]] std::string withReason(std::string_view reason) { return std::string(reason); }

// a conversion's reason as a host reads it: a value of the wrong kind or outside the type named by the type
[[nodiscard]] std::string typedReason(std::string_view reason, std::string_view type) {
    if (reason.empty()) {
        return std::format("not of type {}", type);
    }
    return reason == detail::kOutsideType ? std::format("out of range for {}", type) : std::string(reason);
}

// one sent value as a number, bool or complex T
template<typename T>
[[nodiscard]] std::expected<T, std::string> scalarAs(const pmt::Value& sent) {
    if constexpr (std::same_as<T, bool>) {
        if (const bool* flag = sent.get_if<bool>(); flag != nullptr) {
            return *flag;
        }
        return sent.is_string() ? textAsBool(sent.value_or(std::string_view{})) : std::unexpected(std::string{});
    } else if constexpr (detail::kIsComplex<T>) {
        using TPart = typename T::value_type;
        return detail::visitStoredScalar(sent, []<typename S>(const S& source) -> std::expected<T, std::string> {
            if constexpr (detail::kIsComplex<S>) {
                const std::expected<TPart, std::string_view> real = detail::nearestNumber<TPart>(source.real());
                const std::expected<TPart, std::string_view> imag = detail::nearestNumber<TPart>(source.imag());
                if (!real || !imag) {
                    return std::unexpected(withReason(real ? imag.error() : real.error()));
                }
                return T{*real, *imag};
            } else if constexpr (detail::SettingScalar<S> && !std::same_as<S, bool>) {
                return detail::nearestNumber<TPart>(source).transform([](TPart real) { return T{real, TPart{}}; }).transform_error(withReason);
            } else {
                return std::unexpected(std::string{});
            }
        });
    } else if (sent.is_string()) {
        return textAsNumber<T>(sent.value_or(std::string_view{}));
    } else {
        return detail::visitStoredScalar(sent, [](const auto& source) { return detail::nearestNumber<T>(source).transform_error(withReason); });
    }
}

[[nodiscard]] bool isTextList(const pmt::Value& value) {
    if (value.get_if<Tensor<std::pmr::string>>() != nullptr) {
        return true;
    }
    const Tensor<pmt::Value>* elements = value.get_if<Tensor<pmt::Value>>();
    return elements != nullptr && std::ranges::all_of(*elements, [](const pmt::Value& element) { return element.is_string(); });
}

[[nodiscard]] std::size_t elementCount(const pmt::Value& value) {
    return detail::visitStoredTensor(value, []<typename S>(const S& tensor) -> std::size_t {
        if constexpr (std::same_as<S, std::monostate>) {
            return 0UZ;
        } else {
            return tensor.size();
        }
    });
}

// a list converts element by element
[[nodiscard]] std::expected<pmt::Value, std::string> convertList(const SettingType& type, const pmt::Value& sent) {
    if (!sent.is_tensor()) {
        return std::unexpected(std::string{});
    }
    if (type.extent != 0UZ && elementCount(sent) != type.extent) {
        return std::unexpected(std::format("not of length {}", type.extent));
    }
    if (type.element == ValueType::Monostate) {
        return sent;
    }
    if (type.element == ValueType::String) {
        return isTextList(sent) ? std::expected<pmt::Value, std::string>(sent) : std::unexpected(std::string{});
    }
    const std::string_view elementType = elementName(type.element);
    return withElementType(type.element, [&sent, elementType]<typename T>(std::type_identity<T>) -> std::expected<pmt::Value, std::string> {
        if constexpr (std::same_as<T, std::monostate>) {
            return std::unexpected(std::string{});
        } else {
            return detail::visitStoredTensor(sent, [elementType]<typename S>(const S& source) -> std::expected<pmt::Value, std::string> {
                if constexpr (std::same_as<S, std::monostate>) {
                    return std::unexpected(std::string{});
                } else {
                    Tensor<T>   converted(extents_from, {source.size()});
                    std::size_t index = 0UZ;
                    for (const auto& element : source) {
                        std::expected<T, std::string> elementValue;
                        if constexpr (std::same_as<typename S::value_type, pmt::Value>) {
                            elementValue = scalarAs<T>(element);
                        } else {
                            elementValue = scalarAs<T>(pmt::Value(static_cast<typename S::value_type>(element)));
                        }
                        if (!elementValue) {
                            return std::unexpected(std::format("element {}: {}", index, typedReason(elementValue.error(), elementType)));
                        }
                        converted[index++] = *elementValue;
                    }
                    return pmt::Value(std::move(converted));
                }
            });
        }
    });
}

// whether `value` lies within [minimum, maximum], each bound compared only where it holds a value; a NaN lies outside any bound given
[[nodiscard]] bool withinRange(const pmt::Value& value, const pmt::Value& minimum, const pmt::Value& maximum) {
    return detail::visitStoredScalar(value, [&minimum, &maximum]<typename T>(const T& number) {
        if constexpr (std::same_as<T, std::monostate> || std::same_as<T, bool> || detail::kIsComplex<T>) {
            return true;
        } else {
            const T* lower = minimum.get_if<T>();
            const T* upper = maximum.get_if<T>();
            return (lower == nullptr || number >= *lower) && (upper == nullptr || number <= *upper);
        }
    });
}

// whether the value is a floating-point NaN, which lies outside any range
[[nodiscard]] bool isNotANumber(const pmt::Value& value) {
    return detail::visitStoredScalar(value, []<typename T>([[maybe_unused]] const T& number) {
        if constexpr (std::floating_point<T>) {
            return std::isnan(number);
        } else {
            return false;
        }
    });
}

// a number as a limit's bound reads; any other value as it formats
[[nodiscard]] std::string scalarText(const pmt::Value& value) {
    return detail::visitStoredScalar(value, [&value]<typename T>([[maybe_unused]] const T& number) -> std::string {
        if constexpr (std::is_arithmetic_v<T> && !std::same_as<T, bool>) {
            return gr::detail::limitValueText(number);
        } else {
            return std::format("{}", value);
        }
    });
}

// the end of a refusal that names the value sent: the text or the number, after a colon; none for a list or a map
[[nodiscard]] std::string sentValueSuffix(const pmt::Value& sent) {
    if (sent.is_string()) {
        return std::format(": {}", sent.value_or(std::string_view{}));
    }
    return sent.is_arithmetic() ? std::format(": {}", scalarText(sent)) : std::string{};
}

// a declared bound as a host reads it; empty for the type's own extreme, which no value breaks
[[nodiscard]] std::string declaredBound(const pmt::Value& bound, bool lower) {
    return detail::visitStoredScalar(bound, [lower]<typename T>(const T& number) -> std::string {
        if constexpr (std::same_as<T, std::monostate> || std::same_as<T, bool> || detail::kIsComplex<T>) {
            return {};
        } else {
            const T extreme = lower ? std::numeric_limits<T>::lowest() : std::numeric_limits<T>::max();
            return number == extreme ? std::string{} : gr::detail::limitValueText(number);
        }
    });
}

[[nodiscard]] bool isIdentifier(std::string_view name) {
    const auto isWordCharacter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    return !name.empty() && !(name.front() >= '0' && name.front() <= '9') && std::ranges::all_of(name, isWordCharacter);
}

// a member's name: its index as decimal digits with no leading zero
[[nodiscard]] std::optional<std::size_t> parseMemberIndex(std::string_view text) noexcept {
    if (text.empty() || (text.size() > 1UZ && text.front() == '0')) {
        return std::nullopt;
    }
    std::size_t index{};
    if (!parsesWhole(text, index)) {
        return std::nullopt;
    }
    return index;
}

constexpr std::array<std::string_view, 5UZ>  kTopLevelFields{"version", "block_type", "documentation", "settings", "groups"};
constexpr std::array<std::string_view, 3UZ>  kGroupFields{"count", "bare", "settings"};
constexpr std::array<std::string_view, 15UZ> kSettingFields{"type", "default", "minimum", "maximum", "choices", "step", "unit", "update_policy", "port_count_for", "access", "description", "documentation", "visible", "enabled_when_parameter", "enabled_when_value"};

constexpr std::array<std::string_view, 8UZ> kNotificationLevels{"", "trace", "debug", "info", "warning", "error", "critical", "off"};

// reads one description and records each fault it finds in the table
struct DescriptionReader {
    DeclarationTable& table;

    void fault(std::string_view what) { table.faults.push_back(Rejection{.key = {}, .reason = std::format("faulty settings description of {}: {}", table.blockType, what)}); }

    void checkFields(const property_map& entry, std::span<const std::string_view> defined, std::string_view where) {
        for (const auto& [field, _] : entry) {
            if (std::ranges::find(defined, std::string_view(field)) == defined.end()) {
                fault(std::format("{}: unknown field '{}'", where, std::string_view(field)));
            }
        }
    }

    [[nodiscard]] const pmt::Value* field(const property_map& entry, std::string_view name) {
        const auto it = entry.find(name);
        return it != entry.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::optional<std::string> text(const property_map& entry, std::string_view name, std::string_view where) {
        const pmt::Value* value = field(entry, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            fault(std::format("{}: {} not a text", where, name));
            return std::nullopt;
        }
        return std::string(value->value_or(std::string_view{}));
    }

    [[nodiscard]] std::optional<bool> flag(const property_map& entry, std::string_view name, std::string_view where) {
        const pmt::Value* value = field(entry, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const bool* asBool = value->get_if<bool>();
        if (asBool == nullptr) {
            fault(std::format("{}: {} not a bool", where, name));
            return std::nullopt;
        }
        return *asBool;
    }

    [[nodiscard]] const property_map* map(const property_map& entry, std::string_view name, std::string_view where) {
        const pmt::Value* value = field(entry, name);
        if (value == nullptr) {
            return nullptr;
        }
        const property_map* asMap = value->get_if<property_map>();
        if (asMap == nullptr) {
            fault(std::format("{}: {} not a map", where, name));
        }
        return asMap;
    }

    [[nodiscard]] const property_map* entryMap(const pmt::Value& entry, std::string_view where) {
        const property_map* asMap = entry.get_if<property_map>();
        if (asMap == nullptr) {
            fault(std::format("{}: not a map", where));
        }
        return asMap;
    }

    void checkName(std::string_view name, std::string_view kind) {
        if (!isIdentifier(name)) {
            fault(std::format("{} name not an identifier: '{}'", kind, name));
        }
    }

    [[nodiscard]] std::optional<SettingType> type(const property_map& entry, std::string_view where) {
        const std::optional<std::string> typeText = text(entry, "type", where);
        if (!typeText) {
            if (field(entry, "type") == nullptr) {
                fault(std::format("{}: no type", where));
            }
            return std::nullopt;
        }
        std::optional<SettingType> parsed = parseTypeName(*typeText);
        if (!parsed) {
            fault(std::format("{}: unknown type '{}'", where, *typeText));
        }
        return parsed;
    }

    void presentation(const property_map& entry, std::string_view where, Declaration& declaration) {
        declaration.unit          = text(entry, "unit", where).value_or("");
        declaration.description   = text(entry, "description", where).value_or("");
        declaration.documentation = text(entry, "documentation", where).value_or("");
        declaration.visible       = flag(entry, "visible", where);
    }

    // a value of the entry converted to the setting's type: the default, a limit or the step
    void convertedField(const property_map& entry, std::string_view name, const Declaration& declaration, std::string_view where, pmt::Value& out) {
        const pmt::Value* value = field(entry, name);
        if (value == nullptr) {
            return;
        }
        std::expected<pmt::Value, std::string> converted = convert(declaration, *value);
        if (!converted) {
            fault(std::format("{}: {} {}", where, name, converted.error()));
            return;
        }
        out = std::move(*converted);
    }

    [[nodiscard]] std::optional<Declaration> setting(std::string_view name, const pmt::Value& entryValue, bool inGroup, std::string_view where) {
        const property_map* entry = entryMap(entryValue, where);
        if (entry == nullptr) {
            return std::nullopt;
        }
        checkFields(*entry, kSettingFields, where);
        const std::optional<SettingType> settingType = type(*entry, where);
        if (!settingType) {
            return std::nullopt;
        }
        Declaration declaration{.name = std::string(name), .type = *settingType, .source = DeclarationSource::described};
        presentation(*entry, where, declaration);
        declaration.enabledWhenParameter = text(*entry, "enabled_when_parameter", where).value_or("");
        if (!declaration.enabledWhenParameter.empty()) {
            if (const pmt::Value* condition = field(*entry, "enabled_when_value"); condition != nullptr && condition->get_if<bool>() != nullptr) {
                declaration.enabledWhenValue = *condition;
            } else {
                fault(std::format("{}: enabled_when_value missing or not a bool", where));
            }
        }
        if (const std::optional<std::string> policy = text(*entry, "update_policy", where); policy) {
            if (*policy == "stopped") {
                declaration.stoppedOnly = true;
            } else {
                fault(std::format("{}: update_policy '{}' not one of: stopped", where, *policy));
            }
        }
        if (const std::optional<std::string> access = text(*entry, "access", where); access) {
            if (*access == "read_only") {
                declaration.access    = AccessMode::readOnly;
                declaration.published = true;
            } else {
                fault(std::format("{}: access '{}' not one of: read_only", where, *access));
            }
        }
        if (const pmt::Value* collections = field(*entry, "port_count_for"); collections != nullptr) {
            if (inGroup) {
                fault(std::format("{}: port_count_for not allowed in a member group", where));
            } else if (collections->is_string()) {
                declaration.portCollections.emplace_back(collections->value_or(std::string_view{}));
            } else if (isTextList(*collections) && !collections->get_if<Tensor<pmt::Value>>()->empty()) {
                for (const pmt::Value& collection : *collections->get_if<Tensor<pmt::Value>>()) {
                    declaration.portCollections.emplace_back(collection.value_or(std::string_view{}));
                }
            } else {
                fault(std::format("{}: port_count_for not a text or a list of texts", where));
            }
            if (declaration.isPortKey() && declaration.access != AccessMode::readWrite) {
                fault(std::format("{}: read-only port key", where));
            }
        }
        const bool hasRange = field(*entry, "minimum") != nullptr || field(*entry, "maximum") != nullptr || field(*entry, "step") != nullptr;
        if (hasRange && !declaration.type.isNumber()) {
            fault(std::format("{}: range on a {} setting", where, typeName(declaration.type)));
        } else {
            convertedField(*entry, "minimum", declaration, where, declaration.minimum);
            convertedField(*entry, "maximum", declaration, where, declaration.maximum);
            convertedField(*entry, "step", declaration, where, declaration.step);
            if (declaration.minimum.has_value() && declaration.maximum.has_value() && !withinRange(declaration.minimum, pmt::Value{}, declaration.maximum)) {
                fault(std::format("{}: minimum above maximum", where));
            }
        }
        if (const pmt::Value* choices = field(*entry, "choices"); choices != nullptr) {
            if (!declaration.type.isText()) {
                fault(std::format("{}: choices on a {} setting", where, typeName(declaration.type)));
            } else if (!isTextList(*choices)) {
                fault(std::format("{}: choices not a list of texts", where));
            } else {
                for (const pmt::Value& choice : *choices->get_if<Tensor<pmt::Value>>()) {
                    declaration.choices.emplace_back(choice.value_or(std::string_view{}));
                }
            }
        }
        convertedField(*entry, "default", declaration, where, declaration.defaultValue);
        if (declaration.defaultValue.has_value()) {
            if (const std::optional<std::string> refusal = refuseByLimits(declaration, declaration.defaultValue); refusal) {
                fault(std::format("{}: default {}", where, *refusal));
            }
        }
        return declaration;
    }

    void blockWideSetting(std::string_view name, const pmt::Value& entry) {
        checkName(name, "setting");
        std::optional<Declaration> declaration = setting(name, entry, false, std::format("setting '{}'", name));
        if (!declaration) {
            return;
        }
        if (table.keys.contains(name)) {
            fault(std::format("setting '{}': declared by a reflected field and by the description", name));
            return;
        }
        table.keys.emplace(std::string(name), std::move(*declaration));
    }

    void group(std::string_view name, const pmt::Value& entryValue) {
        checkName(name, "group");
        const std::string   where = std::format("group '{}'", name);
        const property_map* entry = entryMap(entryValue, where);
        if (entry == nullptr) {
            return;
        }
        checkFields(*entry, kGroupFields, where);
        if (const auto existing = table.keys.find(name); existing != table.keys.end()) {
            fault(std::format("group '{}': name of a block-wide setting", name));
        }
        MemberGroup memberGroup{.name = std::string(name), .countKey = text(*entry, "count", where).value_or(""), .bare = flag(*entry, "bare", where).value_or(false)};
        if (const property_map* settings = map(*entry, "settings", where); settings != nullptr) {
            for (const auto& [settingName, settingEntry] : *settings) {
                checkName(settingName, "setting");
                if (std::optional<Declaration> declaration = setting(settingName, settingEntry, true, std::format("setting '{}.{}'", name, std::string_view(settingName))); declaration) {
                    memberGroup.settings.emplace(std::string(settingName), std::move(*declaration));
                }
            }
        }
        if (memberGroup.bare) {
            if (!table.bareGroup.empty()) {
                fault(std::format("groups '{}' and '{}': two bare groups", table.bareGroup, name));
            } else {
                table.bareGroup = std::string(name);
            }
        }
        table.groups.emplace(std::string(name), std::move(memberGroup));
    }

    void read(const property_map& description) {
        checkFields(description, kTopLevelFields, "description");
        if (const pmt::Value* version = field(description, "version"); version == nullptr) {
            fault("description: no version");
        } else if (detail::losslessAs<std::int64_t>(*version) != std::optional<std::int64_t>(1)) {
            fault(std::format("description: version not 1: {}", *version));
        }
        if (field(description, "block_type") == nullptr) {
            fault("description: no block_type");
        } else {
            std::ignore = text(description, "block_type", "description");
        }
        table.documentation = text(description, "documentation", "description").value_or("");
        if (const property_map* settings = map(description, "settings", "description"); settings != nullptr) {
            for (const auto& [name, entry] : *settings) {
                blockWideSetting(name, entry);
            }
        }
        if (const property_map* groups = map(description, "groups", "description"); groups != nullptr) {
            for (const auto& [name, entry] : *groups) {
                group(name, entry);
            }
        }
    }
};

void tableFault(DeclarationTable& table, std::string_view what) { table.faults.push_back(Rejection{.key = {}, .reason = std::format("faulty settings declaration of {}: {}", table.blockType, what)}); }

[[nodiscard]] const Declaration* conditionTarget(const DeclarationTable& table, const MemberGroup* group, std::string_view name) {
    if (group != nullptr) {
        if (const auto it = group->settings.find(name); it != group->settings.end()) {
            return &it->second;
        }
    }
    const auto it = table.keys.find(name);
    return it != table.keys.end() && !it->second.isReadOnly() ? &it->second : nullptr;
}

// the condition's setting exists and is bool, and no conditions form a cycle
void checkConditions(DeclarationTable& table) {
    struct Conditioned {
        const Declaration* setting;
        const MemberGroup* group;
    };
    std::vector<Conditioned> conditioned;
    for (const auto& [_, declaration] : table.keys) {
        if (!declaration.enabledWhenParameter.empty()) {
            conditioned.push_back({&declaration, nullptr});
        }
    }
    for (const auto& [_, memberGroup] : table.groups) {
        for (const auto& [__, declaration] : memberGroup.settings) {
            if (!declaration.enabledWhenParameter.empty()) {
                conditioned.push_back({&declaration, &memberGroup});
            }
        }
    }
    const auto describeSetting = [](const Conditioned& entry) { return entry.group == nullptr ? std::format("setting '{}'", entry.setting->name) : std::format("setting '{}.{}'", entry.group->name, entry.setting->name); };
    const auto next            = [&table](const Conditioned& entry) -> std::optional<Conditioned> {
        const Declaration* target = conditionTarget(table, entry.group, entry.setting->enabledWhenParameter);
        if (target == nullptr || target->enabledWhenParameter.empty()) {
            return std::nullopt;
        }
        const bool inGroup = entry.group != nullptr && entry.group->settings.contains(entry.setting->enabledWhenParameter);
        return Conditioned{target, inGroup ? entry.group : nullptr};
    };

    std::set<const Declaration*> inReportedCycle;
    for (const Conditioned& entry : conditioned) {
        const Declaration* target = conditionTarget(table, entry.group, entry.setting->enabledWhenParameter);
        if (target == nullptr) {
            tableFault(table, std::format("{}: condition '{}' not a setting of the block", describeSetting(entry), entry.setting->enabledWhenParameter));
            continue;
        }
        if (!target->type.isBool()) {
            tableFault(table, std::format("{}: condition '{}' not a bool setting", describeSetting(entry), entry.setting->enabledWhenParameter));
            continue;
        }
        if (inReportedCycle.contains(entry.setting)) {
            continue;
        }
        std::optional<Conditioned> walk = next(entry);
        for (std::size_t step = 0UZ; walk && step < conditioned.size(); ++step) {
            if (walk->setting == entry.setting) {
                tableFault(table, std::format("{}: cycle of conditions", describeSetting(entry)));
                for (std::optional<Conditioned> member = next(entry); member && !inReportedCycle.contains(member->setting); member = next(*member)) {
                    inReportedCycle.insert(member->setting);
                }
                break;
            }
            walk = next(*walk);
        }
    }
}

[[nodiscard]] std::optional<std::string_view> updatePolicyExport(const Declaration& declaration) {
    if (declaration.isPortKey()) {
        return "topology";
    }
    return declaration.stoppedOnly ? std::optional<std::string_view>("stopped") : std::nullopt;
}

// the limits as a host reads them: a reflected field's from its predicates, a described setting's from its minimum and
// maximum, without a bound that is the type's own extreme; empty for a setting without limits
[[nodiscard]] std::string limitsTextOf(const Declaration& declaration) {
    if (!declaration.limits.empty() || (!declaration.minimum.has_value() && !declaration.maximum.has_value())) {
        return declaration.limits;
    }
    const std::string lower = declaredBound(declaration.minimum, true);
    const std::string upper = declaredBound(declaration.maximum, false);
    if (lower.empty() || upper.empty()) {
        return lower.empty() ? (upper.empty() ? std::string("a number") : std::format("<= {}", upper)) : std::format(">= {}", lower);
    }
    return std::format(">= {} and <= {}", lower, upper);
}

void writeKeyMetadata(property_map& metaInformation, std::string_view key, const Declaration& declaration) {
    const auto put = [&metaInformation, key](std::string_view field, pmt::Value value) { metaInformation.insert_or_assign(std::pmr::string(std::format("{}::{}", key, field)), std::move(value)); };
    put("type", typeName(declaration.type));
    if (const std::optional<std::string_view> policy = updatePolicyExport(declaration); policy) {
        put("update_policy", *policy);
    }
    if (declaration.portCollections.size() == 1UZ) {
        put("port_count_for", declaration.portCollections.front());
    } else if (declaration.isPortKey()) {
        put("port_count_for", declaration.portCollections);
    }
    if (declaration.isReadOnly()) {
        put("access", std::string_view("read_only"));
    }
    if (declaration.minimum.has_value()) {
        put("minimum", declaration.minimum);
        put("minimum_excluded", declaration.minimumExcluded);
    }
    if (declaration.maximum.has_value()) {
        put("maximum", declaration.maximum);
        put("maximum_excluded", declaration.maximumExcluded);
    }
    if (declaration.oneOf.has_value()) {
        put("one_of", declaration.oneOf);
    }
    if (const std::string limits = limitsTextOf(declaration); !limits.empty()) {
        put("limits", limits);
    }
    if (declaration.annotated) {
        put("documentation", declaration.documentation);
        put("unit", declaration.unit);
        put("visible", declaration.visible.value_or(false));
    } else {
        if (!declaration.description.empty()) {
            put("description", declaration.description);
        }
        if (!declaration.documentation.empty()) {
            put("documentation", declaration.documentation);
        }
        if (!declaration.unit.empty()) {
            put("unit", declaration.unit);
        }
        if (declaration.visible) {
            put("visible", *declaration.visible);
        }
    }
    if (!declaration.choices.empty()) {
        put("enum_values", declaration.choices);
    }
    if (!declaration.enumType.empty()) {
        put("enum_type", declaration.enumType);
    }
    if (!declaration.enabledWhenParameter.empty()) {
        put("enabled_when_parameter", declaration.enabledWhenParameter);
        put("enabled_when_value", declaration.enabledWhenValue);
    }
}

} // namespace

std::string typeName(const SettingType& type) {
    if (type.container == ContainerType::Map) {
        return "map";
    }
    if (type.container != ContainerType::Tensor) {
        return std::string(elementName(type.element));
    }
    return type.extent == 0UZ ? std::format("{}{}", elementName(type.element), kVectorSuffix) : std::format("{}{}[{}]", elementName(type.element), kVectorSuffix, type.extent);
}

std::optional<SettingType> parseTypeName(std::string_view name) {
    if (name == "map") {
        return SettingType{.element = ValueType::Value, .container = ContainerType::Map};
    }
    const bool       isVector = name.ends_with(kVectorSuffix);
    std::string_view element  = isVector ? name.substr(0UZ, name.size() - kVectorSuffix.size()) : name;
    const auto       it       = std::ranges::find(kElementNames, element, &ElementName::name);
    if (it == kElementNames.end()) {
        return std::nullopt;
    }
    return SettingType{.element = it->element, .container = isVector ? ContainerType::Tensor : scalarContainer(it->element)};
}

std::expected<pmt::Value, std::string> convert(const Declaration& declaration, const pmt::Value& sent) {
    const SettingType& type   = declaration.type;
    const auto         refuse = [&type, &sent](const std::string& why) -> std::expected<pmt::Value, std::string> { return std::unexpected(typedReason(why, typeName(type)) + sentValueSuffix(sent)); };
    switch (type.container) {
    case ContainerType::Map: return sent.is_map() ? std::expected<pmt::Value, std::string>(sent) : refuse({});
    case ContainerType::Tensor: {
        std::expected<pmt::Value, std::string> converted = convertList(type, sent);
        return converted ? converted : refuse(converted.error());
    }
    case ContainerType::String:
        if (!sent.is_string()) {
            return refuse({});
        }
        if (!declaration.enumType.empty()) {
            const std::optional<std::string_view> enumerator = enumeratorNamed(declaration.choices, sent.value_or(std::string_view{}));
            if (!enumerator) {
                return std::unexpected(noneOfChoices(declaration) + sentValueSuffix(sent));
            }
            return pmt::Value(std::string(*enumerator));
        }
        return sent;
    default: break;
    }
    if (type.element == ValueType::Monostate) {
        return sent;
    }
    std::expected<pmt::Value, std::string> converted = withElementType(type.element, [&sent]<typename T>(std::type_identity<T>) -> std::expected<pmt::Value, std::string> {
        if constexpr (std::same_as<T, std::monostate>) {
            return std::unexpected(std::string{});
        } else {
            return scalarAs<T>(sent).transform([](const T& value) { return pmt::Value(value); });
        }
    });
    return converted ? converted : refuse(converted.error());
}

std::string rangeRefusal(std::string_view lower, std::string_view upper, std::string_view value) { return std::format("out of range {}{}, {}{}: {}", lower.empty() ? "(" : "[", lower.empty() ? "-inf" : lower, upper.empty() ? "inf" : upper, upper.empty() ? ")" : "]", value); }

std::optional<std::string> refuseByLimits(const Declaration& declaration, const pmt::Value& converted) {
    if (declaration.checkFieldLimit != nullptr) {
        return declaration.checkFieldLimit(converted);
    }
    if (!declaration.choices.empty() && converted.is_string() && std::ranges::find(declaration.choices, converted.value_or(std::string_view{})) == declaration.choices.end()) {
        return noneOfChoices(declaration) + sentValueSuffix(converted);
    }
    if ((declaration.minimum.has_value() || declaration.maximum.has_value()) && !withinRange(converted, declaration.minimum, declaration.maximum)) {
        if (isNotANumber(converted)) {
            return std::string("not a number");
        }
        return rangeRefusal(declaredBound(declaration.minimum, true), declaredBound(declaration.maximum, false), scalarText(converted));
    }
    return std::nullopt;
}

std::optional<KeyRef> DeclarationTable::find(std::string_view key) const noexcept {
    if (const auto it = keys.find(key); it != keys.end()) {
        return KeyRef{.declaration = &it->second};
    }
    if (!bareGroup.empty()) {
        const MemberGroup& group = groups.find(bareGroup)->second;
        if (const auto it = group.settings.find(key); it != group.settings.end()) {
            return KeyRef{.declaration = &it->second, .group = &group, .member = 0UZ, .bareName = true};
        }
    }
    const std::size_t groupEnd = key.find('.');
    if (groupEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t memberEnd = key.find('.', groupEnd + 1UZ);
    if (memberEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const auto groupIt = groups.find(key.substr(0UZ, groupEnd));
    if (groupIt == groups.end()) {
        return std::nullopt;
    }
    const std::optional<std::size_t> member = parseMemberIndex(key.substr(groupEnd + 1UZ, memberEnd - groupEnd - 1UZ));
    if (!member) {
        return std::nullopt;
    }
    const MemberGroup&     group   = groupIt->second;
    const std::string_view setting = key.substr(memberEnd + 1UZ);
    if (const auto it = group.settings.find(setting); it != group.settings.end()) {
        return KeyRef{.declaration = &it->second, .group = &group, .member = *member};
    }
    return std::nullopt;
}

void addDescription(DeclarationTable& table, const property_map& description) {
    table.described = true;
    DescriptionReader{table}.read(description);
}

void finishDeclarationTable(DeclarationTable& table, bool isScheduler) {
    std::map<std::string_view, std::string_view> sizedBy;
    for (const auto& [name, declaration] : table.keys) {
        if (!declaration.isPortKey()) {
            continue;
        }
        if (!declaration.type.isInteger()) {
            tableFault(table, std::format("port key '{}': not an integer setting", name));
        }
        for (const std::string& collection : declaration.portCollections) {
            if (std::ranges::find(table.portCollections, collection) == table.portCollections.end()) {
                tableFault(table, std::format("port key '{}': '{}' not a dynamic port collection of the block", name, collection));
            } else if (const auto [it, inserted] = sizedBy.emplace(collection, name); !inserted) {
                tableFault(table, std::format("port keys '{}' and '{}': two counts of '{}'", it->second, name, collection));
            }
        }
    }
    for (const auto& [name, memberGroup] : table.groups) {
        if (memberGroup.countKey.empty()) {
            continue;
        }
        const auto count = table.keys.find(memberGroup.countKey);
        if (count == table.keys.end() || count->second.isReadOnly()) {
            tableFault(table, std::format("group '{}': count '{}' not a setting", name, memberGroup.countKey));
        } else if (!count->second.isPortKey()) {
            tableFault(table, std::format("group '{}': count '{}' not a port key", name, memberGroup.countKey));
        }
    }
    if (!table.bareGroup.empty()) {
        for (const auto& [name, _] : table.groups.find(table.bareGroup)->second.settings) {
            if (table.keys.contains(name)) {
                tableFault(table, std::format("setting '{}.{}': bare name of a block-wide key", table.bareGroup, name));
            }
        }
    }
    checkConditions(table);
    if (table.keys.contains(kNotificationLevelKey)) {
        tableFault(table, std::format("{}: reserved for a scheduler", kNotificationLevelKey));
    } else if (isScheduler) {
        Declaration level{.name = std::string(kNotificationLevelKey), .type = {.element = ValueType::String, .container = ContainerType::String}, .source = DeclarationSource::core};
        level.defaultValue = pmt::Value(std::string_view{});
        level.choices.assign(kNotificationLevels.begin(), kNotificationLevels.end());
        table.keys.emplace(level.name, std::move(level));
    }
    for (const auto& [name, declaration] : table.keys) {
        if (declaration.source != DeclarationSource::core) {
            writeKeyMetadata(table.keyMetadata, name, declaration);
        }
    }
    for (const auto& [groupName, memberGroup] : table.groups) {
        for (const auto& [name, declaration] : memberGroup.settings) {
            writeKeyMetadata(table.keyMetadata, std::format("{}.*.{}", groupName, name), declaration);
        }
    }
}

void writeMetadata(const DeclarationTable& table, std::string_view blockDescription, property_map& metaInformation) {
    if (table.described && !table.documentation.empty()) {
        metaInformation.insert_or_assign(std::pmr::string("description"), pmt::Value(table.documentation));
    } else if (!blockDescription.empty()) {
        metaInformation.insert_or_assign(std::pmr::string("description"), pmt::Value(blockDescription));
    }
    for (const auto& [key, value] : table.keyMetadata) {
        metaInformation.insert_or_assign(key, value);
    }
}

void fillDescribedDefaults(const DeclarationTable& table, const void* block, property_map& describedSettings) {
    if (!table.described || !table.faults.empty()) {
        return;
    }
    for (const auto& [name, declaration] : table.keys) {
        if (declaration.source == DeclarationSource::described && declaration.defaultValue.has_value()) {
            describedSettings.try_emplace(std::pmr::string(name), declaration.defaultValue);
        }
    }
    const auto countOf = [&table, block, &describedSettings](const std::string& countKey) -> std::uint64_t {
        const Declaration& count = table.keys.find(countKey)->second;
        pmt::Value         value = count.defaultValue;
        if (count.readField != nullptr) {
            value = count.readField(block);
        } else if (const auto it = describedSettings.find(countKey); it != describedSettings.end()) {
            value = it->second;
        }
        return losslessAs<std::uint64_t>(value).value_or(0U);
    };
    for (const auto& [groupName, memberGroup] : table.groups) {
        if (memberGroup.countKey.empty()) {
            continue;
        }
        const std::uint64_t count = countOf(memberGroup.countKey);
        for (std::uint64_t member = 0U; member < count; ++member) {
            for (const auto& [name, declaration] : memberGroup.settings) {
                if (declaration.defaultValue.has_value()) {
                    describedSettings.try_emplace(std::pmr::string(std::format("{}.{}.{}", groupName, member, name)), declaration.defaultValue);
                }
            }
        }
    }
}

} // namespace gr::settings::detail
