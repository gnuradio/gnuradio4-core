#ifndef GNURADIO_RECIPEPARAMETERS_HPP
#define GNURADIO_RECIPEPARAMETERS_HPP

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Tag.hpp>

/**
 * @brief The recipe parameter dialect: exported parameter declarations and the bounded
 * arithmetic expressions that derive interior settings from them.
 *
 * The YAML definitions loader compiles a recipe document into these types. The grammar is small: numbers, parameter
 * references, the named constants `pi` and `tau_circle` (2·pi), the four arithmetic operators, unary sign and
 * parentheses. It has no functions and no chaining: an expression reads exported parameters and constants only, so
 * evaluation is one pass by construction.
 */
namespace gr::recipe {

/// One exported parameter of a recipe. A declaration without a default is required at instantiation.
struct ParameterDeclaration {
    std::string               name;
    std::string               type; // dialect type word: bool, int8..int64, uint8..uint64, float32, float64, string
    std::optional<pmt::Value> defaultValue;
    std::string               doc;

    [[nodiscard]] bool required() const noexcept { return !defaultValue.has_value(); }
};

namespace detail {

[[nodiscard]] constexpr bool integralTypeWord(std::string_view type) noexcept {
    return type == "int8" || type == "int16" || type == "int32" || type == "int64" //
           || type == "uint8" || type == "uint16" || type == "uint32" || type == "uint64";
}

[[nodiscard]] constexpr bool numericTypeWord(std::string_view type) noexcept { return integralTypeWord(type) || type == "float32" || type == "float64"; }

/// A vector type word is a scalar one with `[]` after it: `float64[]`, `int32[]`, `string[]`.
[[nodiscard]] constexpr bool vectorTypeWord(std::string_view type) noexcept { return type.ends_with("[]") && (numericTypeWord(type.substr(0UZ, type.size() - 2UZ)) || type.substr(0UZ, type.size() - 2UZ) == "bool" || type.substr(0UZ, type.size() - 2UZ) == "string"); }

/// True for a declared type that is handed through rather than computed. Strings, booleans and vectors are handed
/// through: a recipe exports one for a caller to set, and then places the value unchanged.
/// A boolean belongs here and not with the numbers: adding two toggles or scaling one has no meaning a recipe should be
/// able to write.
[[nodiscard]] constexpr bool substitutedTypeWord(std::string_view type) noexcept { return type == "string" || type == "bool" || vectorTypeWord(type); }

[[nodiscard]] constexpr bool knownTypeWord(std::string_view type) noexcept { return numericTypeWord(type) || type == "bool" || type == "string" || vectorTypeWord(type); }

/// the numeric content of a parameter value, whichever numeric alternative it holds
[[nodiscard]] inline std::optional<double> doubleOf(const pmt::Value& value) noexcept {
    if (const auto* v = value.get_if<double>()) {
        return *v;
    }
    if (const auto* v = value.get_if<float>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int64_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint64_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int32_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint32_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int16_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint16_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::int8_t>()) {
        return static_cast<double>(*v);
    }
    if (const auto* v = value.get_if<std::uint8_t>()) {
        return static_cast<double>(*v);
    }
    return std::nullopt;
}

/// the integral content of a parameter value; the integer evaluation path uses this so an
/// index-grade value never passes through a double
[[nodiscard]] inline std::optional<std::int64_t> integerOf(const pmt::Value& value) noexcept {
    if (const auto* v = value.get_if<std::int64_t>()) {
        return *v;
    }
    if (const auto* v = value.get_if<std::uint64_t>()) {
        return *v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? std::optional<std::int64_t>(static_cast<std::int64_t>(*v)) : std::nullopt;
    }
    if (const auto* v = value.get_if<std::int32_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint32_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::int16_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint16_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::int8_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    if (const auto* v = value.get_if<std::uint8_t>()) {
        return static_cast<std::int64_t>(*v);
    }
    return std::nullopt;
}

} // namespace detail

/// A parsed expression as a postfix program. Parameters are referenced by index into the
/// declaration list the expression was parsed against, so evaluation needs only the value
/// vector aligned with those declarations.
struct Expression {
    enum class OpKind : std::uint8_t { pushNumber, pushInteger, pushParameter, add, subtract, multiply, divide, negate };
    struct Op {
        OpKind       kind;
        double       number{};
        std::int64_t integer{};
        std::size_t  parameterIndex{};
    };

    std::vector<Op> ops;
    std::string     source;
    // every operand integral and no division: evaluated in int64 so integer-grade values
    // never round-trip through a double
    bool integerMode = false;
};

namespace detail {

/// user text echoed into an error is display data: control characters are replaced and the echo is bounded, so a
/// definition cannot carry terminal escape sequences or unbounded text through a refusal message
[[nodiscard]] inline std::string printableEcho(std::string_view text) {
    constexpr std::size_t kMaxEcho = 120;
    std::string           echo;
    echo.reserve(std::min(text.size(), kMaxEcho));
    for (const char c : text.substr(0, kMaxEcho)) {
        echo.push_back(static_cast<unsigned char>(c) < 0x20U || c == 0x7F ? '?' : c);
    }
    if (text.size() > kMaxEcho) {
        echo += "...";
    }
    return echo;
}

inline constexpr std::size_t kMaxExpressionDepth = 64;

struct Parser {
    std::string_view                      text;
    std::size_t                           position = 0;
    std::span<const ParameterDeclaration> declarations;
    Expression*                           out;
    bool                                  sawDivide           = false;
    bool                                  allOperandsIntegral = true;
    std::size_t                           depth               = 0;

    void skipSpace() noexcept {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
            ++position;
        }
    }

    [[nodiscard]] bool atEnd() noexcept {
        skipSpace();
        return position >= text.size();
    }

    [[nodiscard]] std::optional<char> peek() noexcept {
        skipSpace();
        return position < text.size() ? std::optional<char>(text[position]) : std::nullopt;
    }

    [[nodiscard]] std::unexpected<gr::Error> fail(std::string_view what) const { return std::unexpected(gr::Error(std::format("recipe_expression_parse: {} at position {} in '{}'", what, position, printableEcho(text)))); }

    [[nodiscard]] std::expected<void, gr::Error> parseExpr() {
        if (auto term = parseTerm(); !term.has_value()) {
            return term;
        }
        while (auto c = peek()) {
            if (*c != '+' && *c != '-') {
                break;
            }
            ++position;
            if (auto term = parseTerm(); !term.has_value()) {
                return term;
            }
            out->ops.push_back({.kind = *c == '+' ? Expression::OpKind::add : Expression::OpKind::subtract});
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseTerm() {
        if (auto factor = parseFactor(); !factor.has_value()) {
            return factor;
        }
        while (auto c = peek()) {
            if (*c != '*' && *c != '/') {
                break;
            }
            sawDivide = sawDivide || *c == '/';
            ++position;
            if (auto factor = parseFactor(); !factor.has_value()) {
                return factor;
            }
            out->ops.push_back({.kind = *c == '*' ? Expression::OpKind::multiply : Expression::OpKind::divide});
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseFactor() {
        // the depth guard bounds recursion, so deep nesting is a refusal and not a stack overflow
        if (depth >= kMaxExpressionDepth) {
            return fail("expression nested too deeply");
        }
        ++depth;
        auto parsed = parseFactorAtDepth();
        --depth;
        return parsed;
    }

    [[nodiscard]] std::expected<void, gr::Error> parseFactorAtDepth() {
        const auto c = peek();
        if (!c.has_value()) {
            return fail("expected a value");
        }
        if (*c == '-' || *c == '+') {
            ++position;
            if (auto factor = parseFactor(); !factor.has_value()) {
                return factor;
            }
            if (*c == '-') {
                out->ops.push_back({.kind = Expression::OpKind::negate});
            }
            return {};
        }
        return parsePrimary();
    }

    [[nodiscard]] std::expected<void, gr::Error> parsePrimary() {
        const auto c = peek();
        if (!c.has_value()) {
            return fail("expected a value");
        }
        if (*c == '(') {
            ++position;
            if (auto expr = parseExpr(); !expr.has_value()) {
                return expr;
            }
            if (peek() != ')') {
                return fail("expected ')'");
            }
            ++position;
            return {};
        }
        if ((*c >= '0' && *c <= '9') || *c == '.') {
            return parseNumber();
        }
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_') {
            return parseIdentifier();
        }
        return fail(std::format("unexpected character '{}'", printableEcho(std::string_view(&*c, 1))));
    }

    [[nodiscard]] std::expected<void, gr::Error> parseNumber() {
        const std::size_t start = position;
        while (position < text.size() && ((text[position] >= '0' && text[position] <= '9') || text[position] == '.' || text[position] == 'e' || text[position] == 'E' //
                                             || ((text[position] == '+' || text[position] == '-') && (text[position - 1] == 'e' || text[position - 1] == 'E')))) {
            ++position;
        }
        const std::string_view token      = text.substr(start, position - start);
        const bool             isIntegral = token.find('.') == std::string_view::npos && token.find('e') == std::string_view::npos && token.find('E') == std::string_view::npos;
        if (isIntegral) {
            std::int64_t value{};
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (ec != std::errc{} || ptr != token.data() + token.size()) {
                return fail(std::format("unparsable integer '{}'", token));
            }
            out->ops.push_back({.kind = Expression::OpKind::pushInteger, .integer = value});
            return {};
        }
        double value{};
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || ptr != token.data() + token.size()) {
            return fail(std::format("unparsable number '{}'", token));
        }
        allOperandsIntegral = false;
        out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = value});
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> parseIdentifier() {
        const std::size_t start = position;
        while (position < text.size() && ((text[position] >= 'a' && text[position] <= 'z') || (text[position] >= 'A' && text[position] <= 'Z') || (text[position] >= '0' && text[position] <= '9') || text[position] == '_')) {
            ++position;
        }
        const std::string_view name = text.substr(start, position - start);
        if (name == "pi") {
            allOperandsIntegral = false;
            out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = std::numbers::pi});
            return {};
        }
        if (name == "tau_circle") {
            allOperandsIntegral = false;
            out->ops.push_back({.kind = Expression::OpKind::pushNumber, .number = 2.0 * std::numbers::pi});
            return {};
        }
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            if (declarations[index].name != name) {
                continue;
            }
            if (!numericTypeWord(declarations[index].type)) {
                return std::unexpected(gr::Error(std::format("recipe_expression_parse: parameter '{}' has type {} and cannot appear in an expression ('{}')", printableEcho(name), declarations[index].type, printableEcho(text))));
            }
            allOperandsIntegral = allOperandsIntegral && integralTypeWord(declarations[index].type);
            out->ops.push_back({.kind = Expression::OpKind::pushParameter, .parameterIndex = index});
            return {};
        }
        std::string declared;
        for (const auto& declaration : declarations) {
            declared += declared.empty() ? declaration.name : (", " + declaration.name);
        }
        return std::unexpected(gr::Error(std::format("recipe_unknown_identifier: '{}' in '{}'; declared parameters: [{}]", printableEcho(name), printableEcho(text), printableEcho(declared))));
    }
};

} // namespace detail

/// Parses the text after the '=' sentinel against the recipe's declarations.
[[nodiscard]] inline std::expected<Expression, gr::Error> parseExpression(std::string_view source, std::span<const ParameterDeclaration> declarations) {
    Expression     expression{.ops = {}, .source = std::string(source)};
    detail::Parser parser{.text = source, .declarations = declarations, .out = &expression};
    if (auto parsed = parser.parseExpr(); !parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!parser.atEnd()) {
        return std::unexpected(parser.fail("trailing input").error());
    }
    expression.integerMode = parser.allOperandsIntegral && !parser.sawDivide;
    return expression;
}

/// Evaluates against values aligned with the declarations the expression was parsed with.
/// Integer-mode expressions evaluate in int64 throughout; everything else in float64. A
/// non-finite value at any step is refused, so a division by zero is refused even where a
/// later operation would make the result finite.
[[nodiscard]] inline std::expected<pmt::Value, gr::Error> evaluate(const Expression& expression, std::span<const pmt::Value> parameterValues) {
    auto operandError = [&](std::size_t index) { return std::unexpected(gr::Error(std::format("recipe_expression_conversion: parameter #{} holds no numeric value for '{}'", index, detail::printableEcho(expression.source)))); };

    if (expression.integerMode) {
        std::vector<std::int64_t> stack;
        for (const auto& op : expression.ops) {
            switch (op.kind) {
            case Expression::OpKind::pushInteger: stack.push_back(op.integer); break;
            case Expression::OpKind::pushParameter: {
                if (op.parameterIndex >= parameterValues.size()) {
                    return operandError(op.parameterIndex);
                }
                const auto value = detail::integerOf(parameterValues[op.parameterIndex]);
                if (!value.has_value()) {
                    return operandError(op.parameterIndex);
                }
                stack.push_back(*value);
                break;
            }
            case Expression::OpKind::negate:
                if (stack.back() == std::numeric_limits<std::int64_t>::min()) {
                    return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' overflows", detail::printableEcho(expression.source))));
                }
                stack.back() = -stack.back();
                break;
            default: {
                const std::int64_t right = stack.back();
                stack.pop_back();
                std::int64_t& left = stack.back();
                // checked arithmetic: an overflowing expression is refused and never wraps
                std::int64_t result{};
                const bool   overflowed = op.kind == Expression::OpKind::add ? __builtin_add_overflow(left, right, &result) : op.kind == Expression::OpKind::subtract ? __builtin_sub_overflow(left, right, &result) : __builtin_mul_overflow(left, right, &result);
                if (overflowed) {
                    return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' overflows", detail::printableEcho(expression.source))));
                }
                left = result;
                break;
            }
            }
        }
        if (stack.empty()) {
            return std::unexpected(gr::Error(std::format("recipe_expression_parse: '{}' is empty", detail::printableEcho(expression.source))));
        }
        return pmt::Value(stack.back());
    }

    std::vector<double> stack;
    for (const auto& op : expression.ops) {
        switch (op.kind) {
        case Expression::OpKind::pushNumber: stack.push_back(op.number); break;
        case Expression::OpKind::pushInteger: stack.push_back(static_cast<double>(op.integer)); break;
        case Expression::OpKind::pushParameter: {
            if (op.parameterIndex >= parameterValues.size()) {
                return operandError(op.parameterIndex);
            }
            const auto value = detail::doubleOf(parameterValues[op.parameterIndex]);
            if (!value.has_value()) {
                return operandError(op.parameterIndex);
            }
            stack.push_back(*value);
            break;
        }
        case Expression::OpKind::negate: stack.back() = -stack.back(); break;
        default: {
            const double right = stack.back();
            stack.pop_back();
            double& left = stack.back();
            left         = op.kind == Expression::OpKind::add ? left + right : op.kind == Expression::OpKind::subtract ? left - right : op.kind == Expression::OpKind::multiply ? left * right : left / right;
            break;
        }
        }
        if (!stack.empty() && !std::isfinite(stack.back())) {
            return std::unexpected(gr::Error(std::format("recipe_expression_conversion: '{}' evaluated to a non-finite value", detail::printableEcho(expression.source))));
        }
    }
    if (stack.empty()) {
        return std::unexpected(gr::Error(std::format("recipe_expression_parse: '{}' is empty", detail::printableEcho(expression.source))));
    }
    return pmt::Value(stack.back());
}

namespace detail {

/// Whether a vector-typed declaration's default holds the vector its type word names. The element word is the
/// type word without its `[]`; a sequence reaches a recipe as a Tensor of that element type, and a vector of
/// strings as a Tensor of Values.
[[nodiscard]] inline bool vectorDefaultAgrees(std::string_view type, const pmt::Value& value) noexcept {
    const std::string_view element = type.substr(0UZ, type.size() - 2UZ);
    if (element == "float64") {
        return value.get_if<Tensor<double>>() != nullptr;
    }
    if (element == "float32") {
        return value.get_if<Tensor<float>>() != nullptr;
    }
    if (element == "int64") {
        return value.get_if<Tensor<std::int64_t>>() != nullptr;
    }
    if (element == "int32") {
        return value.get_if<Tensor<std::int32_t>>() != nullptr;
    }
    if (element == "int16") {
        return value.get_if<Tensor<std::int16_t>>() != nullptr;
    }
    if (element == "int8") {
        return value.get_if<Tensor<std::int8_t>>() != nullptr;
    }
    if (element == "uint64") {
        return value.get_if<Tensor<std::uint64_t>>() != nullptr;
    }
    if (element == "uint32") {
        return value.get_if<Tensor<std::uint32_t>>() != nullptr;
    }
    if (element == "uint16") {
        return value.get_if<Tensor<std::uint16_t>>() != nullptr;
    }
    if (element == "uint8") {
        return value.get_if<Tensor<std::uint8_t>>() != nullptr;
    }
    if (element == "bool") {
        return value.get_if<Tensor<bool>>() != nullptr;
    }
    return element == "string" && value.get_if<Tensor<pmt::Value>>() != nullptr;
}

/// True when `value` is a scalar the type word `type` can hold: an integral type takes an integer within its range, a
/// floating type a finite number, `bool` a boolean and `string` a string. A `float32` takes a finite number no larger in
/// magnitude than a float can hold, so a default, a supplied value, a live change and a list element all refuse 1e300.
[[nodiscard]] inline bool scalarValueAgrees(std::string_view type, const pmt::Value& value) noexcept {
    if (type == "bool") {
        return value.get_if<bool>() != nullptr;
    }
    if (type == "string") {
        return value.get_if<std::pmr::string>() != nullptr;
    }
    if (type == "float32" || type == "float64") {
        const std::optional<double> number = doubleOf(value);
        if (!number.has_value() || !std::isfinite(*number)) {
            return false;
        }
        return type == "float64" || std::abs(*number) <= static_cast<double>(std::numeric_limits<float>::max());
    }
    if (type == "uint64" && value.get_if<std::uint64_t>() != nullptr) {
        return true;
    }
    const std::optional<std::int64_t> integer = integerOf(value);
    if (!integer.has_value()) {
        return false;
    }
    if (type == "int8") {
        return std::in_range<std::int8_t>(*integer);
    }
    if (type == "int16") {
        return std::in_range<std::int16_t>(*integer);
    }
    if (type == "int32") {
        return std::in_range<std::int32_t>(*integer);
    }
    if (type == "int64") {
        return true;
    }
    if (type == "uint8") {
        return std::in_range<std::uint8_t>(*integer);
    }
    if (type == "uint16") {
        return std::in_range<std::uint16_t>(*integer);
    }
    if (type == "uint32") {
        return std::in_range<std::uint32_t>(*integer);
    }
    return type == "uint64" && *integer >= 0;
}

/// True when `value` is a value of the type word `type`. A vector type takes a tensor of its element type, or a list
/// of values each of which the element type can hold.
[[nodiscard]] inline bool valueAgrees(std::string_view type, const pmt::Value& value) noexcept {
    if (!vectorTypeWord(type)) {
        return scalarValueAgrees(type, value);
    }
    if (vectorDefaultAgrees(type, value)) {
        return true;
    }
    const auto* elements = value.get_if<Tensor<pmt::Value>>();
    return elements != nullptr && std::ranges::all_of(*elements, [element = type.substr(0UZ, type.size() - 2UZ)](const pmt::Value& item) { return scalarValueAgrees(element, item); });
}

} // namespace detail

/// Refuses a value the declared type of `declaration` cannot hold, naming the parameter and the type.
[[nodiscard]] inline std::expected<void, gr::Error> checkParameterValue(const ParameterDeclaration& declaration, const pmt::Value& value) {
    if (detail::valueAgrees(declaration.type, value)) {
        return {};
    }
    return std::unexpected(gr::Error(std::format("recipe_parameter_type: parameter '{}' of type {} cannot hold {}", detail::printableEcho(declaration.name), declaration.type, detail::printableEcho(std::format("{}", value)))));
}

/// Validates a recipe's declaration list at definition load: duplicates, reserved spellings
/// (framework settings and the expression constants), unknown type words, mistyped defaults.
[[nodiscard]] inline std::expected<void, gr::Error> validateDeclarations(std::span<const ParameterDeclaration> declarations) {
    static constexpr std::array<std::string_view, 8> kReservedNames{"name", "compute_domain", "disconnect_on_done", "enabled", "ui_constraints", "meta_information", "pi", "tau_circle"};
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        const auto& declaration = declarations[index];
        if (!detail::knownTypeWord(declaration.type)) {
            return std::unexpected(gr::Error(std::format("recipe_default_type: parameter '{}' declares unknown type '{}'", declaration.name, declaration.type)));
        }
        if (std::ranges::find(kReservedNames, declaration.name) != kReservedNames.end()) {
            return std::unexpected(gr::Error(std::format("recipe_reserved_parameter: '{}'", declaration.name)));
        }
        for (std::size_t other = index + 1; other < declarations.size(); ++other) {
            if (declarations[other].name == declaration.name) {
                return std::unexpected(gr::Error(std::format("recipe_duplicate_parameter: '{}'", declaration.name)));
            }
        }
        if (declaration.defaultValue.has_value()) {
            if (!detail::valueAgrees(declaration.type, *declaration.defaultValue)) {
                return std::unexpected(gr::Error(std::format("recipe_default_type: parameter '{}' declares type {} but its default holds another", declaration.name, declaration.type)));
            }
        }
    }
    return {};
}

/// Overlays supplied values on declared defaults. Unknown names are refused by name, a supplied value its declared type
/// cannot hold is refused naming the type, and missing required parameters are refused together, all named in one
/// error.
[[nodiscard]] inline std::expected<std::vector<pmt::Value>, gr::Error> resolveParameters(std::span<const ParameterDeclaration> declarations, const property_map& supplied) {
    for (const auto& [name, value] : supplied) {
        const bool declared = std::ranges::any_of(declarations, [&](const ParameterDeclaration& declaration) { return std::string_view(declaration.name) == std::string_view(name); });
        if (!declared) {
            return std::unexpected(gr::Error(std::format("recipe_unknown_parameter: '{}'", detail::printableEcho(name))));
        }
    }
    std::vector<pmt::Value> values;
    values.reserve(declarations.size());
    std::string missing;
    for (const auto& declaration : declarations) {
        const pmt::Value* suppliedValue = nullptr;
        for (const auto& [name, value] : supplied) {
            if (std::string_view(name) == std::string_view(declaration.name)) {
                suppliedValue = &value;
                break;
            }
        }
        if (suppliedValue != nullptr) {
            if (auto agrees = checkParameterValue(declaration, *suppliedValue); !agrees.has_value()) {
                return std::unexpected(agrees.error());
            }
            values.push_back(*suppliedValue);
        } else if (declaration.defaultValue.has_value()) {
            values.push_back(*declaration.defaultValue);
        } else {
            missing += missing.empty() ? declaration.name : (", " + declaration.name);
            values.emplace_back();
        }
    }
    if (!missing.empty()) {
        return std::unexpected(gr::Error(std::format("recipe_parameter_required: [{}]", missing)));
    }
    return values;
}

/// One derived interior setting: which block (as a name path from the composite's interior
/// downward, through nested subgraphs), which setting, and the expression that derives it.
struct Binding {
    std::vector<std::string> namePath;
    std::string              settingKey;
    Expression               expression;
    /// Set instead of `expression` when the setting takes a parameter's value unchanged: a string or a vector
    /// carries no arithmetic, so what a recipe can do with one is hand it through.
    std::optional<std::size_t> substituted;
};

/// The value a binding produces for one set of parameter values.
[[nodiscard]] inline std::expected<pmt::Value, gr::Error> bindingValue(const Binding& binding, std::span<const pmt::Value> values) {
    if (binding.substituted.has_value()) {
        if (*binding.substituted >= values.size()) {
            return std::unexpected(gr::Error("recipe_parameter_index: a substituted parameter is out of range"));
        }
        return values[*binding.substituted];
    }
    return evaluate(binding.expression, values);
}

} // namespace gr::recipe

#endif // GNURADIO_RECIPEPARAMETERS_HPP
