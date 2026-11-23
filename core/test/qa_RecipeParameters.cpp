#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/RecipeParameters.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

#include "settings_utils.hpp"

using namespace boost::ut;
using gr::recipe::evaluate;
using gr::recipe::ParameterDeclaration;
using gr::recipe::parseExpression;
using gr::recipe::resolveParameters;
using gr::recipe::validateDeclarations;

namespace {

// boost.ut runs a suite from its runner's destructor, after the objects of this translation unit have been
// destroyed, so anything a test body reads is held by a function-local static and outlives the run.
[[nodiscard]] const std::vector<ParameterDeclaration>& nbfmDeclarations() {
    static const std::vector<ParameterDeclaration> declarations{
        {.name = "sample_rate", .type = "float32", .defaultValue = std::nullopt, .doc = ""},
        {.name = "deviation", .type = "float32", .defaultValue = std::nullopt, .doc = ""},
        {.name = "tau", .type = "float64", .defaultValue = gr::pmt::Value(7.5e-05), .doc = ""},
    };
    return declarations;
}

[[nodiscard]] double evaluated(std::string_view source, const std::vector<gr::pmt::Value>& values) {
    const auto expression = parseExpression(source, nbfmDeclarations());
    expect(expression.has_value()) << source;
    // expect() does not abort, so a refusal must be answered with a value that fails every
    // comparison after it, never with a dereference of the error
    if (!expression.has_value()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto result = evaluate(*expression, values);
    expect(result.has_value()) << source;
    if (!result.has_value()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // integer-mode expressions return an int64 value; read whichever numeric arrived
    return gr::recipe::detail::doubleOf(*result).value_or(0.0);
}

} // namespace

const boost::ut::suite<"RecipeParameters"> recipeParameterTests = [] {
    "the canonical derivation evaluates in float64 exactly"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(96000.0f), gr::pmt::Value(5000.0f), gr::pmt::Value(7.5e-05)};
        const double                      gain = evaluated("sample_rate / (2 * pi * deviation)", values);
        expect(eq(gain, 96000.0 / (2.0 * std::numbers::pi * 5000.0)));
    };

    "precedence, unary sign, parentheses and constants"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(4.0f), gr::pmt::Value(2.0f), gr::pmt::Value(1.0)};
        expect(eq(evaluated("1 + 2 * 3", values), 7.0));
        expect(eq(evaluated("(1 + 2) * 3", values), 9.0));
        expect(eq(evaluated("-sample_rate + 1", values), -3.0));
        expect(eq(evaluated("tau_circle / 2", values), std::numbers::pi));
        expect(eq(evaluated("sample_rate / deviation / 2", values), 1.0)) << "division is left-associative";
    };

    "clamp holds a derived value between its bounds"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(2000000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(7.5e-05)};
        expect(eq(evaluated("clamp(sample_rate * 0.05, 32768, 4194304)", values), 100000.0)) << "between the bounds the value stands";
        expect(eq(evaluated("clamp(sample_rate * 0.05, 200000, 4194304)", values), 200000.0)) << "below the low bound the low bound stands";
        expect(eq(evaluated("clamp(sample_rate * 0.05, 32768, 50000)", values), 50000.0)) << "above the high bound the high bound stands";
        expect(eq(evaluated("clamp(-sample_rate, -1, 1) + clamp(1, 0, 2)", values), 0.0)) << "a call is a factor like any other";
        expect(eq(evaluated("clamp(clamp(sample_rate, 0, 100), 0, 10)", values), 10.0)) << "an argument is a whole expression, calls included";

        const std::vector<ParameterDeclaration> integral{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              expression = parseExpression("clamp(n, 32768, 4194304)", integral);
        expect(expression.has_value());
        if (!expression.has_value()) {
            return;
        }
        expect(expression->integerMode) << "an all-integral clamp stays on the int64 path";
        const std::vector<gr::pmt::Value> big{gr::pmt::Value(std::int64_t{9007199254740993})};
        const auto                        bounded = evaluate(*expression, big);
        expect(bounded.has_value());
        if (bounded.has_value()) {
            expect(eq(bounded->value_or(std::int64_t{}), std::int64_t{4194304})) << "the high bound holds without a trip through a double";
        }
    };

    "clamp refuses a wrong arity, a non-numeric argument and unordered bounds"_test = [] {
        const auto arity = parseExpression("clamp(sample_rate, 1)", nbfmDeclarations());
        expect(!arity.has_value());
        expect(arity.error().message.contains("recipe_expression_parse")) << arity.error().message;
        expect(arity.error().message.contains("clamp takes 3 arguments")) << arity.error().message;
        expect(!parseExpression("clamp(sample_rate, 1, 2, 3)", nbfmDeclarations()).has_value()) << "a fourth argument is refused too";
        expect(!parseExpression("clamp()", nbfmDeclarations()).has_value());
        expect(!parseExpression("clamp(sample_rate, 1, 2", nbfmDeclarations()).has_value()) << "an unclosed call is refused";

        const auto unknownFunction = parseExpression("ceil(sample_rate)", nbfmDeclarations());
        expect(!unknownFunction.has_value());
        expect(unknownFunction.error().message.contains("unknown function")) << unknownFunction.error().message;
        expect(unknownFunction.error().message.contains("clamp")) << "the refusal names the vocabulary it has";

        const std::vector<ParameterDeclaration> withText{
            {.name = "label", .type = "string", .defaultValue = std::nullopt, .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = std::nullopt, .doc = ""},
        };
        const auto text = parseExpression("clamp(label, 0, 1)", withText);
        expect(!text.has_value()) << "a call argument is an expression, so a non-numeric parameter is refused there too";
        if (!text.has_value()) {
            expect(text.error().message.contains("cannot appear in an expression")) << text.error().message;
        }

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(2000000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(7.5e-05)};
        const auto                        unordered = parseExpression("clamp(sample_rate, 4194304, 32768)", nbfmDeclarations());
        expect(unordered.has_value()) << "the bounds' order is a value question, not a grammar one";
        if (unordered.has_value()) {
            const auto refused = evaluate(*unordered, values);
            expect(!refused.has_value());
            if (!refused.has_value()) {
                expect(refused.error().message.contains("recipe_expression_domain")) << refused.error().message;
            }
        }

        const auto unboundedArgument = parseExpression("clamp(1 / 0, 0, 1)", nbfmDeclarations());
        expect(unboundedArgument.has_value());
        if (unboundedArgument.has_value()) {
            expect(!evaluate(*unboundedArgument, values).has_value()) << "a division by zero is refused even where clamp would bound it";
        }

        const std::vector<ParameterDeclaration> integral{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              unorderedIntegers = parseExpression("clamp(n, 10, 1)", integral);
        expect(unorderedIntegers.has_value() && unorderedIntegers->integerMode);
        if (unorderedIntegers.has_value()) {
            const std::vector<gr::pmt::Value> one{gr::pmt::Value(std::int64_t{5})};
            const auto                        refusedIntegers = evaluate(*unorderedIntegers, one);
            expect(!refusedIntegers.has_value()) << "the int64 path refuses the same bounds";
            if (!refusedIntegers.has_value()) {
                expect(refusedIntegers.error().message.contains("recipe_expression_domain")) << refusedIntegers.error().message;
            }
        }

        const std::vector<ParameterDeclaration> shadow{{.name = "clamp", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              shadowed = validateDeclarations(shadow);
        expect(!shadowed.has_value()) << "a parameter may not shadow the dialect's own function name";
        if (!shadowed.has_value()) {
            expect(shadowed.error().message.contains("recipe_reserved_parameter")) << shadowed.error().message;
        }
    };

    "a bare parameter reference is plain forwarding"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(48000.0f), gr::pmt::Value(2500.0f), gr::pmt::Value(5e-05)};
        expect(eq(evaluated("tau", values), 5e-05));
    };

    "the integer rule preserves 2^53 + 1"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "start", .type = "uint64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              expression = parseExpression("start + 1", declarations);
        expect(expression.has_value());
        expect(expression->integerMode) << "all-integral with no division evaluates in int64";
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(std::int64_t{9007199254740992})};
        const auto                        result = evaluate(*expression, values);
        expect(result.has_value());
        expect(eq(result->value_or(std::int64_t{}), std::int64_t{9007199254740993})) << "the value a double would silently lose";
    };

    "division and float operands leave integer mode"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "n", .type = "int32", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              divided = parseExpression("n / 2", declarations);
        expect(divided.has_value() && !divided->integerMode);
        const auto scaled = parseExpression("n * 1.5", declarations);
        expect(scaled.has_value() && !scaled->integerMode);
        const auto shifted = parseExpression("n * 2 - 1", declarations);
        expect(shifted.has_value() && shifted->integerMode);
    };

    "parse refusals carry position and name"_test = [] {
        expect(!parseExpression("1 +", nbfmDeclarations()).has_value());
        expect(!parseExpression("(1 + 2", nbfmDeclarations()).has_value());
        expect(!parseExpression("1 ; 2", nbfmDeclarations()).has_value());
        expect(!parseExpression("", nbfmDeclarations()).has_value());
        const auto unknown = parseExpression("sample_rate * bandwidth", nbfmDeclarations());
        expect(!unknown.has_value());
        expect(unknown.error().message.contains("recipe_unknown_identifier")) << unknown.error().message;
        expect(unknown.error().message.contains("bandwidth")) << unknown.error().message;
        expect(unknown.error().message.contains("sample_rate")) << "the refusal lists the declared parameters";
    };

    "a string parameter cannot appear in an expression"_test = [] {
        const std::vector<ParameterDeclaration> declarations{{.name = "label", .type = "string", .defaultValue = std::nullopt, .doc = ""}};
        expect(!parseExpression("label + 1", declarations).has_value());
    };

    "a string or vector parameter is substituted, not evaluated"_test = [] {
        // these types carry no arithmetic, so a recipe hands one through unchanged. The engine names them as
        // substituted and produces the value; the grammar stays numeric.
        const std::vector<ParameterDeclaration> declarations{
            {.name = "detector", .type = "string", .defaultValue = gr::pmt::Value(std::pmr::string("zero_crossing")), .doc = ""},
            {.name = "carriers", .type = "int32[]", .defaultValue = std::nullopt, .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = gr::pmt::Value(48000.0), .doc = ""},
        };
        expect(gr::recipe::detail::substitutedTypeWord(declarations[0].type));
        expect(gr::recipe::detail::substitutedTypeWord(declarations[1].type));
        expect(!gr::recipe::detail::substitutedTypeWord(declarations[2].type)) << "a numeric parameter stays on the expression path";
        expect(gr::recipe::detail::vectorTypeWord("string[]"));
        expect(!gr::recipe::detail::vectorTypeWord("int32[")) << "a malformed type word is not a vector";

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(std::pmr::string("gardner")), gr::pmt::Value(std::vector<std::int32_t>{-2, 0, 3}), gr::pmt::Value(48000.0)};

        const gr::recipe::Binding substituted{.namePath = {"inner"}, .settingKey = "detector", .expression = {}, .substituted = 0UZ, .sequence = {}};
        const auto                bound = gr::recipe::bindingValue(substituted, std::span<const gr::pmt::Value>(values));
        expect(bound.has_value());
        const auto* text = bound->get_if<std::pmr::string>();
        expect(text != nullptr) << "the string arrives as a string, not converted";
        if (text != nullptr) {
            expect(eq(std::string_view(*text), std::string_view("gardner")));
        }

        const gr::recipe::Binding vectorBound{.namePath = {"inner"}, .settingKey = "carriers", .expression = {}, .substituted = 1UZ, .sequence = {}};
        const auto                carriers = gr::recipe::bindingValue(vectorBound, std::span<const gr::pmt::Value>(values));
        expect(carriers.has_value());
        expect(carriers->get_if<gr::Tensor<std::int32_t>>() != nullptr) << "the sequence arrives whole";

        // an index past the values is a refusal rather than a read off the end
        const gr::recipe::Binding outOfRange{.namePath = {"inner"}, .settingKey = "x", .expression = {}, .substituted = 9UZ, .sequence = {}};
        expect(!gr::recipe::bindingValue(outOfRange, std::span<const gr::pmt::Value>(values)).has_value());
    };

    "a vector declaration accepts only its own element type"_test = [] {
        const auto declare = [](std::string_view type, gr::pmt::Value value) {
            const std::vector<ParameterDeclaration> declarations{{.name = "seq", .type = std::string(type), .defaultValue = std::move(value), .doc = ""}};
            return validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value();
        };
        expect(declare("int32[]", gr::pmt::Value(std::vector<std::int32_t>{1, 2})));
        expect(declare("float64[]", gr::pmt::Value(std::vector<double>{1., 2.})));
        expect(!declare("int32[]", gr::pmt::Value(std::vector<double>{1., 2.}))) << "an element type that disagrees is refused";
        expect(!declare("int32[]", gr::pmt::Value(3))) << "a scalar default under a vector type is refused";
        expect(!declare("complex64[]", gr::pmt::Value(std::vector<double>{1.}))) << "an element word the dialect has no name for is refused";
    };

    "a boolean parameter is substituted, and refuses to be an operand"_test = [] {
        // A toggle is a value to hand through, not one to compute with: adding two of them or scaling one has no
        // meaning a recipe should be able to write, so it joins the strings and vectors on the substitution path.
        // The exported name cannot be `enabled`: that is one of the framework's own settings and is reserved. The
        // interior setting it binds to can be `enabled`, and `settingKey` below is that setting.
        const std::vector<ParameterDeclaration> declarations{
            {.name = "dc_block_enabled", .type = "bool", .defaultValue = gr::pmt::Value(true), .doc = ""},
            {.name = "rate", .type = "float64", .defaultValue = gr::pmt::Value(48000.0), .doc = ""},
        };
        expect(validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value());
        expect(gr::recipe::detail::substitutedTypeWord("bool"));
        expect(gr::recipe::detail::substitutedTypeWord("bool[]")) << "a table of toggles is substituted like any other sequence";

        const std::vector<ParameterDeclaration> reserved{{.name = "enabled", .type = "bool", .defaultValue = gr::pmt::Value(true), .doc = ""}};
        const auto                              refused = validateDeclarations(std::span<const ParameterDeclaration>(reserved));
        expect(!refused.has_value()) << "a block setting's own name is not available to export";
        if (!refused.has_value()) {
            expect(refused.error().message.contains("recipe_reserved_parameter")) << refused.error().message;
        }

        const std::vector<gr::pmt::Value> values{gr::pmt::Value(false), gr::pmt::Value(48000.0)};
        const gr::recipe::Binding         toggle{.namePath = {"inner"}, .settingKey = "enabled", .expression = {}, .substituted = 0UZ, .sequence = {}};
        const auto                        bound = gr::recipe::bindingValue(toggle, std::span<const gr::pmt::Value>(values));
        expect(bound.has_value());
        const auto* flag = bound->get_if<bool>();
        expect(flag != nullptr) << "the boolean arrives as a boolean, not as a number";
        if (flag != nullptr) {
            expect(!*flag) << "a supplied false is handed through, not defaulted back to the declaration";
        }

        const auto asOperand = parseExpression("dc_block_enabled * rate", std::span<const ParameterDeclaration>(declarations));
        expect(!asOperand.has_value()) << "there is no grammar over a boolean";
        if (!asOperand.has_value()) {
            expect(asOperand.error().message.contains("cannot appear in an expression")) << asOperand.error().message;
            expect(asOperand.error().message.contains("bool")) << "the refusal names the type it refused";
        }
    };

    "a boolean declaration accepts only a boolean default"_test = [] {
        const auto declare = [](std::string_view type, gr::pmt::Value value) {
            const std::vector<ParameterDeclaration> declarations{{.name = "flag", .type = std::string(type), .defaultValue = std::move(value), .doc = ""}};
            return validateDeclarations(std::span<const ParameterDeclaration>(declarations)).has_value();
        };
        expect(declare("bool", gr::pmt::Value(true)));
        expect(declare("bool", gr::pmt::Value(false)));
        expect(!declare("bool", gr::pmt::Value(1))) << "an integer standing in for a toggle is refused";
        expect(!declare("bool", gr::pmt::Value(std::pmr::string("true")))) << "the word true is a string, not a boolean";
        expect(!declare("float64", gr::pmt::Value(true))) << "a boolean default under a numeric type is refused";
        expect(!declare("bool[]", gr::pmt::Value(true))) << "a scalar default under a vector type is refused";
        expect(declare("bool[]", gr::pmt::Value(std::vector<bool>{true, false})));
    };

    "a non-finite result is refused"_test = [] {
        const std::vector<gr::pmt::Value> values{gr::pmt::Value(1.0f), gr::pmt::Value(0.0f), gr::pmt::Value(0.0)};
        const auto                        expression = parseExpression("sample_rate / deviation", nbfmDeclarations());
        expect(expression.has_value());
        expect(!evaluate(*expression, values).has_value());

        const auto absorbed = parseExpression("1 / (1 / 0)", nbfmDeclarations());
        expect(absorbed.has_value());
        if (absorbed.has_value()) {
            expect(!evaluate(*absorbed, values).has_value()) << "a division by zero is refused even where a later step makes the result finite";
        }
    };

    "required parameters are refused together, all named"_test = [] {
        const auto resolved = resolveParameters(nbfmDeclarations(), {});
        expect(!resolved.has_value());
        expect(resolved.error().message.contains("recipe_parameter_required")) << resolved.error().message;
        expect(resolved.error().message.contains("sample_rate") && resolved.error().message.contains("deviation")) << resolved.error().message;
        expect(!resolved.error().message.contains("tau")) << "a defaulted parameter is not demanded";
    };

    "supplied values overlay defaults; unknown names are refused"_test = [] {
        gr::property_map supplied;
        supplied["sample_rate"] = 48000.0f;
        supplied["deviation"]   = 2500.0f;
        const auto resolved     = resolveParameters(nbfmDeclarations(), supplied);
        expect(resolved.has_value());
        expect(eq((*resolved)[0].value_or(float{}), 48000.0f));
        expect(eq((*resolved)[2].value_or(double{}), 7.5e-05)) << "the default filled in";

        supplied["bandwidth"] = 1.0f;
        const auto unknown    = resolveParameters(nbfmDeclarations(), supplied);
        expect(!unknown.has_value());
        expect(unknown.error().message.contains("recipe_unknown_parameter")) << unknown.error().message;
    };

    "a supplied value its declared type cannot hold is refused naming the type"_test = [] {
        const std::vector<ParameterDeclaration> declarations{
            {.name = "width", .type = "uint8", .defaultValue = std::nullopt, .doc = ""},
            {.name = "count", .type = "int32", .defaultValue = std::nullopt, .doc = ""},
        };
        gr::property_map supplied;
        supplied["width"] = std::int64_t{255};
        supplied["count"] = std::int32_t{7};
        expect(resolveParameters(declarations, supplied).has_value()) << "values within their types are taken";

        supplied["width"]     = std::int64_t{300};
        const auto outOfRange = resolveParameters(declarations, supplied);
        expect(!outOfRange.has_value()) << "300 does not fit a uint8";
        if (!outOfRange.has_value()) {
            expect(outOfRange.error().message.contains("recipe_parameter_type") && outOfRange.error().message.contains("width") && outOfRange.error().message.contains("uint8")) << outOfRange.error().message;
        }

        supplied["width"]     = std::int64_t{255};
        supplied["count"]     = 2.5;
        const auto fractional = resolveParameters(declarations, supplied);
        expect(!fractional.has_value()) << "a float given to an integral parameter is refused";
        if (!fractional.has_value()) {
            expect(fractional.error().message.contains("count") && fractional.error().message.contains("int32")) << fractional.error().message;
        }
    };

    "a float32 parameter refuses a value a float cannot hold, and a float64 parameter takes it"_test = [] {
        constexpr double kBeyondFloat = 1e300;
        constexpr double kFloatMax    = static_cast<double>(std::numeric_limits<float>::max());
        const auto       declare      = [](std::string_view type, std::optional<gr::pmt::Value> defaultValue) { return std::vector<ParameterDeclaration>{{.name = "level", .type = std::string(type), .defaultValue = std::move(defaultValue), .doc = ""}}; };
        const auto       namesLevel   = [](const auto& result) { return !result.has_value() && result.error().message.contains("level") && result.error().message.contains("float32"); };

        const auto asDefault = declare("float32", gr::pmt::Value(kBeyondFloat));
        const auto loaded    = validateDeclarations(std::span<const ParameterDeclaration>(asDefault));
        expect(namesLevel(loaded)) << "a default beyond float32 is refused naming the parameter";

        const auto       required = declare("float32", std::nullopt);
        gr::property_map supplied;
        supplied["level"]     = kBeyondFloat;
        const auto asSupplied = resolveParameters(std::span<const ParameterDeclaration>(required), supplied);
        expect(namesLevel(asSupplied)) << "a supplied value beyond float32 is refused naming the parameter";

        const std::vector<gr::pmt::Value> elements{gr::pmt::Value(1.0), gr::pmt::Value(kBeyondFloat)};
        supplied["level"] = gr::pmt::Value(gr::Tensor<gr::pmt::Value>(elements.begin(), elements.end()));
        const auto listed = declare("float32[]", std::nullopt);
        const auto asList = resolveParameters(std::span<const ParameterDeclaration>(listed), supplied);
        expect(!asList.has_value() && asList.error().message.contains("level")) << "a list element beyond float32 is refused naming the parameter";

        for (const double edge : {kFloatMax, -kFloatMax}) {
            supplied["level"] = edge;
            expect(resolveParameters(std::span<const ParameterDeclaration>(required), supplied).has_value()) << "the largest float, either sign, is taken: " << edge;
            const auto atEdge = declare("float32", gr::pmt::Value(edge));
            expect(validateDeclarations(std::span<const ParameterDeclaration>(atEdge)).has_value()) << "as a default too: " << edge;
        }
        supplied["level"] = -kBeyondFloat;
        expect(namesLevel(resolveParameters(std::span<const ParameterDeclaration>(required), supplied))) << "the negative side is refused as well";

        const auto wide   = declare("float64", std::nullopt);
        supplied["level"] = kBeyondFloat;
        expect(resolveParameters(std::span<const ParameterDeclaration>(wide), supplied).has_value()) << "a float64 parameter still takes 1e300";
        const auto wideDefault = declare("float64", gr::pmt::Value(kBeyondFloat));
        expect(validateDeclarations(std::span<const ParameterDeclaration>(wideDefault)).has_value());
    };

    "input cannot escape: depth, overflow and echo are all bounded"_test = [] {
        const std::string deep   = std::string(300, '(') + "1" + std::string(300, ')');
        const auto        nested = parseExpression(deep, nbfmDeclarations());
        expect(!nested.has_value()) << "deep nesting is a refusal, not a stack overflow";
        expect(nested.error().message.contains("too deeply")) << nested.error().message;

        const std::vector<ParameterDeclaration> declarations{{.name = "n", .type = "int64", .defaultValue = std::nullopt, .doc = ""}};
        const auto                              product = parseExpression("n * n", declarations);
        expect(product.has_value() && product->integerMode);
        const std::vector<gr::pmt::Value> big{gr::pmt::Value(std::int64_t{4611686018427387904})};
        const auto                        overflowed = evaluate(*product, big);
        expect(!overflowed.has_value()) << "integer overflow refuses, never wraps or invokes undefined behavior";
        expect(overflowed.error().message.contains("overflows")) << overflowed.error().message;

        expect(!parseExpression("1e999", nbfmDeclarations()).has_value()) << "an out-of-range literal is refused at parse";

        const auto echoed = parseExpression("\x1b[31mred\x1b[0m", nbfmDeclarations());
        expect(!echoed.has_value());
        expect(echoed.error().message.find('\x1b') == std::string::npos) << "control characters never reach an error message";
    };

    "integer add, subtract and multiply refuse exactly the results outside int64"_test = [] {
        using gr::recipe::detail::checkedArithmetic;
        using enum gr::recipe::Expression::OpKind;
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        static_assert(checkedArithmetic(add, kMax - 1, 1) == kMax);
        static_assert(!checkedArithmetic(add, kMax, 1).has_value());
        static_assert(checkedArithmetic(add, kMin + 1, -1) == kMin);
        static_assert(!checkedArithmetic(add, kMin, -1).has_value());
        static_assert(checkedArithmetic(subtract, kMin + 1, 1) == kMin);
        static_assert(!checkedArithmetic(subtract, kMin, 1).has_value());
        static_assert(!checkedArithmetic(subtract, 0, kMin).has_value(), "-INT64_MIN is not an int64");
        static_assert(checkedArithmetic(subtract, -1, kMin) == kMax);
        static_assert(checkedArithmetic(multiply, kMin, 1) == kMin);
        static_assert(!checkedArithmetic(multiply, kMin, -1).has_value());
        static_assert(!checkedArithmetic(multiply, -1, kMin).has_value());
        static_assert(checkedArithmetic(multiply, std::int64_t{-4611686018427387904}, 2) == kMin);
        static_assert(!checkedArithmetic(multiply, std::int64_t{4611686018427387904}, 2).has_value());
        static_assert(!checkedArithmetic(multiply, std::int64_t{-4611686018427387905}, 2).has_value());
        static_assert(checkedArithmetic(multiply, 0, kMin) == std::int64_t{0});
        static_assert(!checkedArithmetic(divide, 6, 3).has_value(), "only add, subtract and multiply are evaluated");
        static_assert(!checkedArithmetic(negate, 6, 3).has_value(), "only add, subtract and multiply are evaluated");
    };

    "integer evaluation refuses an operation it does not support instead of invoking undefined behaviour"_test = [] {
        using enum gr::recipe::Expression::OpKind;
        const gr::recipe::Expression handBuilt{.ops = {{.kind = pushInteger, .integer = 6}, {.kind = pushInteger, .integer = 3}, {.kind = divide}}, .source = "6 / 3", .integerMode = true};
        const auto                   refused = evaluate(handBuilt, {});
        expect(!refused.has_value()) << "an integer-mode expression holding a division is refused";
        expect(refused.error().message.contains("does not support")) << refused.error().message;
    };

    "declaration validation refuses duplicates, reserved names, unknown types and mistyped defaults"_test = [] {
        expect(validateDeclarations(nbfmDeclarations()).has_value());
        const std::vector<ParameterDeclaration> duplicate{{.name = "a", .type = "float32", .defaultValue = std::nullopt, .doc = ""}, {.name = "a", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(duplicate).has_value());
        const std::vector<ParameterDeclaration> reserved{{.name = "pi", .type = "float32", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(reserved).has_value());
        const std::vector<ParameterDeclaration> unknownType{{.name = "a", .type = "float128", .defaultValue = std::nullopt, .doc = ""}};
        expect(!validateDeclarations(unknownType).has_value());
        const std::vector<ParameterDeclaration> mistyped{{.name = "a", .type = "float32", .defaultValue = gr::pmt::Value(std::pmr::string("x")), .doc = ""}};
        expect(!validateDeclarations(mistyped).has_value());
    };
};

namespace qa_recipe_definitions {

struct RecipeScale : gr::Block<RecipeScale> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float>              gain  = 1.0f;
        gr::Annotated<float>              rate  = 0.0f;
        gr::Annotated<std::string>        label = "";
        gr::Annotated<std::vector<float>> taps{};

        GR_MAKE_REFLECTABLE(Settings, gain, rate, label, taps);
    } settings;

    GR_MAKE_REFLECTABLE(RecipeScale, in, out, settings);

    explicit RecipeScale(gr::property_map init = {}) : gr::Block<RecipeScale>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return settings.gain * sample; }
};

// a stage whose gain lies within 0 to 10 and runs in whole steps, and whose device cannot read its rate back
struct RecipeWatched : gr::Block<RecipeWatched> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<10.f>>> gain = 1.0f;
        gr::Annotated<float>                                          rate = 0.0f;

        GR_MAKE_REFLECTABLE(Settings, gain, rate);
    } settings;

    GR_MAKE_REFLECTABLE(RecipeWatched, in, out, settings);

    std::size_t nGainSets = 0UZ; // the applies that changed the gain

    explicit RecipeWatched(gr::property_map init = {}) : gr::Block<RecipeWatched>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return settings.gain * sample; }

    void applySettings(Settings& s, const Settings& old) {
        if (s.gain == old.gain) {
            return;
        }
        ++nGainSets;
        s.gain = std::round(s.gain.value); // the stage runs whole gains
    }
};

// a stage whose check hook can hold its call until the test releases it
struct RecipeLatched : gr::Block<RecipeLatched> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(RecipeLatched, in, out, settings);

    std::atomic<float>        holdAtGain{0.0f}; // the first check of a call that would run this gain waits for `released`
    mutable std::atomic<bool> held{false};
    std::atomic<bool>         released{false};

    explicit RecipeLatched(gr::property_map init = {}) : gr::Block<RecipeLatched>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return settings.gain * sample; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        const float heldGain = holdAtGain.load();
        if (heldGain != 0.0f && s.gain == heldGain && !held.exchange(true)) {
            while (!released.load()) {
                std::this_thread::yield();
            }
        }
        return {};
    }
};

// a stage whose gain lies within 0 to 10, whose apply hook counts its calls and can hold one until the test releases it
struct RecipeHeld : gr::Block<RecipeHeld> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<10.f>>> gain = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings;

    GR_MAKE_REFLECTABLE(RecipeHeld, in, out, settings);

    std::atomic<std::size_t> nApplies{0UZ};
    std::atomic<float>       holdAtGain{0.0f}; // the first apply of this gain waits for `released`
    std::atomic<bool>        held{false};
    std::atomic<bool>        released{false};

    explicit RecipeHeld(gr::property_map init = {}) : gr::Block<RecipeHeld>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return settings.gain * sample; }

    void applySettings(const Settings& s) {
        ++nApplies;
        const float heldGain = holdAtGain.load();
        if (heldGain != 0.0f && s.gain == heldGain && !held.exchange(true)) {
            while (!released.load()) {
                std::this_thread::yield();
            }
        }
    }
};

void registerRecipeTestBlock() {
    static const bool registered = [] { return gr::globalBlockRegistry().insert<RecipeScale>("=qa::RecipeScale") && gr::globalBlockRegistry().insert<RecipeWatched>("=qa::RecipeWatched") && gr::globalBlockRegistry().insert<RecipeLatched>("=qa::RecipeLatched") && gr::globalBlockRegistry().insert<RecipeHeld>("=qa::RecipeHeld"); }();
    expect(registered) << "the test block must reach the global registry";
}

[[nodiscard]] gr::detail::YamlDefinitionsLoader::Definition definitionFrom(std::string_view yamlText) {
    const auto parsed = gr::pmt::yaml::deserialize(yamlText);
    expect(parsed.has_value()) << "the fixture yaml must parse";
    return {.definition = *parsed, .metadata = {}};
}

[[nodiscard]] gr::PluginLoader recipeTestLoader() {
    static gr::SchedulerRegistry          schedulerRegistry;
    static const std::vector<std::string> noPaths;
    return gr::PluginLoader(gr::globalBlockRegistry(), schedulerRegistry, noPaths);
}

constexpr std::string_view kParameterizedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: scaled
    exported_parameters:
      - name: sample_rate
        type: float32
      - name: deviation
        type: float32
      - name: tau
        type: float64
        default: !!float64 7.5e-05
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            gain: "=sample_rate / (2 * pi * deviation)"
            rate: "=sample_rate"
            label: "\\=verbatim"
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kLiteralRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: fixed
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            gain: !!float32 2.5
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kEdgeSizedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: sized
    exported_parameters:
      - name: sample_rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "=sample_rate * 0.05"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kUnsizedEdgeRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: unsized
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "one hundred thousand"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kDerivedTapsRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: shaped
    exported_parameters:
      - name: peak
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
            taps:
              - "=peak"
              - "=peak / 2"
              - !!float32 0.25
      exported_ports:
        - [inner, INPUT, in, in]
        - [inner, OUTPUT, out, out]
)yaml";

constexpr std::string_view kClampedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: clamped
    exported_parameters:
      - name: sample_rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
            gain: "=clamp(sample_rate * 0.05, 32768, 4194304)"
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        -
          - "first"
          - "out"
          - "second"
          - "in"
          - "=clamp(sample_rate * 0.05, 32768, 4194304)"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

constexpr std::string_view kPartlyRefusedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: partly
    exported_parameters:
      - name: level
        type: float32
      - name: count
        type: int64
    graph:
      blocks:
        - id: "qa::RecipeWatched"
          parameters:
            name: first
            gain: "=level"
        - id: "qa::RecipeWatched"
          parameters:
            name: second
            gain: "=count"
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";

[[nodiscard]] std::size_t interiorEdgeBufferSize(const std::shared_ptr<gr::BlockModel>& composite) {
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->edges().empty()) {
        expect(false) << "the definition must produce a composite with an interior edge";
        return 0UZ;
    }
    return composite->graph()->edges().front().minBufferSize();
}

[[nodiscard]] std::shared_ptr<gr::BlockModel> interiorBlockNamed(const std::shared_ptr<gr::BlockModel>& composite, std::string_view name) {
    if (composite == nullptr || composite->graph() == nullptr) {
        expect(false) << "the definition must produce a composite";
        return nullptr;
    }
    for (const auto& candidate : composite->graph()->blocks()) {
        if (candidate->name() == name) {
            return candidate;
        }
    }
    expect(false) << name << " is not an interior block of the composite";
    return nullptr;
}

[[nodiscard]] std::shared_ptr<gr::BlockModel> interiorBlock(const std::shared_ptr<gr::BlockModel>& composite) {
    // expect() does not abort, so a broken composite must be answered with null, never a dereference
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->blocks().empty()) {
        expect(false) << "the definition must produce a composite with an interior block";
        return nullptr;
    }
    return composite->graph()->blocks().front();
}

// a call on the composite's settings that waits for nothing, its refusal as one text
[[nodiscard]] std::expected<void, std::string> setParameters(const std::shared_ptr<gr::BlockModel>& composite, const gr::property_map& changes) {
    const std::expected<gr::settings::Values, gr::settings::Rejected> result = composite->set(changes, gr::settings::noWait);
    if (!result) {
        return std::unexpected(gr::settings::describeRejected(result.error()));
    }
    return {};
}

} // namespace qa_recipe_definitions

using namespace qa_recipe_definitions;

const boost::ut::suite<"RecipeDefinitions"> recipeDefinitionTests = [] {
    "a parameterized definition instantiates with required parameters and derives interior settings"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }

        const double expectedGain = 48000.0 / (2.0 * std::numbers::pi * 2500.0);
        const auto   gain         = inner->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), static_cast<float>(expectedGain))) << "the derivation reached the interior setting";
        const auto rate = inner->get<float>("rate");
        expect(rate.has_value());
        expect(eq(rate.value_or(float{}), 48000.0f)) << "a bare reference forwards";
        const auto label = inner->get<std::string>("label");
        expect(label.has_value());
        expect(eq(label.value_or(std::string{}), std::string("=verbatim"))) << "the escape yields a literal '='";
    };

    "missing required parameters are refused together"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kParameterizedRecipe);
        const auto refusal = gr::detail::instantiateBlockFromYamlDefinition(loader, def, {});
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_parameter_required")) << refusal.error().message;
        expect(refusal.error().message.contains("sample_rate") && refusal.error().message.contains("deviation")) << refusal.error().message;
    };

    "an unknown parameter is refused by name"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        parameters["bandwidth"]   = 1.0f;
        const auto refusal        = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_unknown_parameter")) << refusal.error().message;
    };

    "an unnamed nested subgraph holding a derived setting is refused at load"_test = [] {
        constexpr std::string_view kUnnamedNestedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: outer
    exported_parameters:
      - name: level
        type: float32
    graph:
      blocks:
        - id: SUBGRAPH
          graph:
            blocks:
              - id: "qa::RecipeScale"
                parameters:
                  name: inner
                  gain: "=level"
)yaml";
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        gr::property_map parameters;
        parameters["level"] = 2.0f;
        const auto refusal  = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kUnnamedNestedRecipe), parameters);
        expect(!refusal.has_value()) << "a binding path through an unnamed subgraph cannot be resolved";
        if (!refusal.has_value()) {
            expect(refusal.error().message.contains("nested subgraph") && refusal.error().message.contains("needs a name")) << refusal.error().message;
        }
    };

    "a live change beyond float32 refuses naming the parameter, and the one at the edge applies"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        auto* wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr);
        if (wrapper == nullptr) {
            return;
        }
        gr::property_map beyond;
        beyond["deviation"] = 1e300;
        const auto refused  = setParameters(*composite, beyond);
        expect(!refused.has_value()) << "1e300 does not fit the float32 deviation";
        if (!refused.has_value()) {
            expect(refused.error().contains("deviation") && refused.error().contains("float32")) << refused.error();
        }
        gr::property_map atEdge;
        atEdge["deviation"] = static_cast<double>(std::numeric_limits<float>::max());
        const auto applied  = setParameters(*composite, atEdge);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error());
    };

    "a live parameter change re-evaluates and stages; a refusal leaves the values standing"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kParameterizedRecipe);
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        parameters["deviation"]   = 2500.0f;
        const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        auto* wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        const auto inner = interiorBlock(*composite);
        if (wrapper == nullptr || inner == nullptr) {
            return;
        }

        gr::property_map change;
        change["deviation"] = 5000.0f;
        const auto applied  = setParameters(*composite, change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error());
        const auto staged = gr::settings::heldValues(inner->get());
        const auto gainIt = staged.find("gain");
        expect(gainIt != staged.end()) << "the derived setting reaches the interior block";
        if (gainIt != staged.end()) {
            // a set converts to the member's own float; read whichever numeric arrived
            const double stagedGain = gr::recipe::detail::doubleOf(gainIt->second).value_or(0.0);
            expect(eq(static_cast<float>(stagedGain), static_cast<float>(48000.0 / (2.0 * std::numbers::pi * 5000.0)))) << "re-evaluated with the changed parameter";
        }

        gr::property_map unknown;
        unknown["bandwidth"] = 1.0f;
        expect(!setParameters(*composite, unknown).has_value()) << "an undeclared parameter refuses";

        gr::property_map mistyped;
        mistyped["deviation"]  = std::pmr::string("wide");
        const auto refusedType = setParameters(*composite, mistyped);
        expect(!refusedType.has_value()) << "a live value its declared type cannot hold refuses";
        if (!refusedType.has_value()) {
            expect(eq(refusedType.error(), std::string("deviation: not a decimal number: wide")));
        }

        gr::property_map zero;
        zero["deviation"] = 0.0f;
        expect(!setParameters(*composite, zero).has_value()) << "a non-finite derivation refuses the change whole";

        gr::property_map next;
        next["sample_rate"]    = 96000.0f;
        const auto nextApplied = setParameters(*composite, next);
        expect(nextApplied.has_value()) << (nextApplied.has_value() ? "" : nextApplied.error());
        const auto stagedAfter = gr::settings::heldValues(inner->get());
        const auto gainAfter   = stagedAfter.find("gain");
        expect(gainAfter != stagedAfter.end());
        if (gainAfter != stagedAfter.end()) {
            const double stagedGain = gr::recipe::detail::doubleOf(gainAfter->second).value_or(0.0);
            expect(eq(static_cast<float>(stagedGain), static_cast<float>(96000.0 / (2.0 * std::numbers::pi * 5000.0)))) << "deviation stood at its last committed value through the refusal";
        }
    };

    "a connection's buffer size derives from an exported parameter"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kEdgeSizedRecipe);
        const auto sizedAt = [&](float rate) {
            gr::property_map parameters;
            parameters["sample_rate"] = rate;
            const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
            expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
            return composite.has_value() ? interiorEdgeBufferSize(*composite) : 0UZ;
        };
        expect(eq(sizedAt(2000000.0f), 100000UZ)) << "5 % of the rate in samples, not the loader's default size";
        expect(eq(sizedAt(2400000.0f), 120000UZ)) << "the edge follows the rate the recipe was instantiated at";
        expect(eq(sizedAt(1000010.0f), 50001UZ)) << "a fractional minimum rises to the next whole sample";
    };

    "a buffer size that is not a positive sample count leaves the size to the framework"_test = [] {
        constexpr std::string_view kFourElementRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: plain
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: first
        - id: "qa::RecipeScale"
          parameters:
            name: second
      connections:
        - [first, out, second, in]
      exported_ports:
        - [first, INPUT, in, in]
        - [second, OUTPUT, out, out]
)yaml";
        registerRecipeTestBlock();
        auto       loader = recipeTestLoader();
        const auto plain  = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kFourElementRecipe), {});
        expect(fatal(plain.has_value())) << (plain.has_value() ? "" : plain.error().message);
        const std::size_t frameworkSize = interiorEdgeBufferSize(*plain);

        gr::property_map zero;
        zero["sample_rate"] = 0.0f;
        const auto unsized  = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kEdgeSizedRecipe), zero);
        expect(unsized.has_value()) << (unsized.has_value() ? "" : unsized.error().message);
        if (unsized.has_value()) {
            expect(eq(interiorEdgeBufferSize(*unsized), frameworkSize)) << "a derived size of zero samples takes the size of an edge that names none";
        }

        const auto text = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kUnsizedEdgeRecipe), {});
        expect(text.has_value()) << (text.has_value() ? "" : text.error().message);
        if (text.has_value()) {
            expect(eq(interiorEdgeBufferSize(*text), frameworkSize)) << "a fifth element that is neither number nor expression takes the size of an edge that names none";
        }
    };

    "an expression in a tag coupling is refused by name"_test = [] {
        const auto definitionWith = [](std::string_view entry) {
            return definitionFrom(std::string(R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: literal
    exported_parameters:
      - name: sample_rate
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeScale"
          parameters:
            name: inner
)yaml") + std::string(entry));
        };
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;

        const auto coupled = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionWith("          tag_couplings:\n            rate: \"=sample_rate\"\n"), parameters);
        expect(!coupled.has_value()) << "an expression in tag_couplings is not evaluated, and is refused";
        if (!coupled.has_value()) {
            expect(coupled.error().message.contains("tag_couplings") && coupled.error().message.contains("=sample_rate")) << coupled.error().message;
        }
    };

    "a vector setting's elements derive from expressions and re-derive live"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        const auto       def    = definitionFrom(kDerivedTapsRecipe);
        gr::property_map parameters;
        parameters["peak"]   = 0.5f;
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }

        const auto taps = inner->get<std::vector<float>>("taps");
        expect(taps.has_value());
        const std::vector<float>* derived = taps.has_value() ? &*taps : nullptr;
        expect(derived != nullptr) << "the derived sequence reaches the setting in the member's own element type";
        if (derived != nullptr) {
            expect(eq(derived->size(), 3UZ));
            expect(eq((*derived)[0], 0.5f));
            expect(eq((*derived)[1], 0.25f));
            expect(eq((*derived)[2], 0.25f)) << "a literal element beside the expressions is carried unchanged";
        }

        auto* wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        if (wrapper == nullptr) {
            return;
        }
        gr::property_map change;
        change["peak"]     = 0.8f;
        const auto applied = setParameters(*composite, change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error());
        const auto staged = gr::settings::heldValues(inner->get());
        const auto tapsIt = staged.find("taps");
        expect(tapsIt != staged.end()) << "the whole sequence is staged, because a setting is staged whole";
        if (tapsIt != staged.end()) {
            const auto* stagedTaps = tapsIt->second.get_if<gr::Tensor<float>>();
            expect(stagedTaps != nullptr);
            if (stagedTaps != nullptr) {
                expect(eq(stagedTaps->size(), 3UZ));
                expect(eq((*stagedTaps)[0], 0.8f));
                expect(eq((*stagedTaps)[1], 0.4f)) << "every derived element re-evaluates against the changed parameter";
                expect(eq((*stagedTaps)[2], 0.25f));
            }
        }
    };

    "a clamped edge holds its bounds at every rate, and the derived setting re-evaluates live"_test = [] {
        registerRecipeTestBlock();
        auto       loader  = recipeTestLoader();
        const auto def     = definitionFrom(kClampedRecipe);
        const auto builtAt = [&](float rate) { //
            gr::property_map parameters;
            parameters["sample_rate"] = rate;
            const auto composite      = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
            expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
            return composite;
        };
        const auto sizedAt = [&](float rate) {
            const auto composite = builtAt(rate);
            return composite.has_value() ? interiorEdgeBufferSize(*composite) : 0UZ;
        };
        expect(eq(sizedAt(2000000.0f), 100000UZ)) << "between the bounds the rate rule stands";
        expect(eq(sizedAt(96000.0f), 32768UZ)) << "a low rate takes the floor, not 5 % of itself";
        expect(eq(sizedAt(200000000.0f), 4194304UZ)) << "a high rate takes the ceiling";

        const auto composite = builtAt(96000.0f);
        if (!composite.has_value()) {
            return;
        }
        const auto first   = interiorBlockNamed(*composite, "first");
        auto*      wrapper = dynamic_cast<gr::GraphWrapper<gr::Graph>*>(composite->get());
        expect(wrapper != nullptr) << "a parameterized composite carries the binding machinery";
        if (first == nullptr || wrapper == nullptr) {
            return;
        }
        const auto gain = first->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), 32768.0f)) << "the floor reached the interior setting";

        gr::property_map change;
        change["sample_rate"] = 2000000.0f;
        const auto applied    = setParameters(*composite, change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error());
        const auto staged = gr::settings::heldValues(first->get());
        const auto gainIt = staged.find("gain");
        expect(gainIt != staged.end()) << "the clamped derivation re-evaluates through the binding";
        if (gainIt != staged.end()) {
            expect(eq(static_cast<float>(gr::recipe::detail::doubleOf(gainIt->second).value_or(0.0)), 100000.0f)) << "the new rate falls between the bounds";
        }
    };

    "an inner block's refusal changes no inner block, and the next change reaches every block"_test = [] {
        registerRecipeTestBlock();
        auto             loader = recipeTestLoader();
        gr::property_map parameters;
        parameters["level"]  = 1.0f;
        parameters["count"]  = std::int64_t{1};
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(kPartlyRefusedRecipe), parameters);
        expect(fatal(composite.has_value())) << (composite.has_value() ? "" : composite.error().message);
        const auto first  = interiorBlockNamed(*composite, "first");
        const auto second = interiorBlockNamed(*composite, "second");
        expect(fatal(first != nullptr && second != nullptr));

        // the second block's limits refuse a gain above 10
        gr::property_map refusedChange;
        refusedChange["level"] = 2.0f;
        refusedChange["count"] = std::int64_t{100};
        const auto refused     = (*composite)->set(refusedChange); // a waiting set checks every inner block before the store
        expect(fatal(!refused.has_value())) << "the second block refuses the derived gain";
        expect(eq(refused.error().size(), 1UZ)) << gr::settings::describeRejected(refused.error());
        const gr::settings::Rejection& entry = refused.error().front();
        expect(eq(entry.key, std::string("count"))) << "the refusal names the parameter the refused gain derives from";
        expect(entry.reason.contains("second") && entry.reason.contains("gain")) << "the reason names the inner block and key: " << entry.reason;
        expect(eq(first->get<float>("gain").value_or(0.0f), 1.0f)) << "the first block keeps its gain";
        expect(eq(second->get<float>("gain").value_or(0.0f), 1.0f)) << "the second block keeps its gain";
        expect(eq((*composite)->get<float>("level").value_or(0.0f), 1.0f)) << "the refused call stores no parameter";

        gr::property_map next;
        next["level"] = 3.0f;
        expect(setParameters(*composite, next).has_value());
        expect(eq(first->get<float>("gain").value_or(0.0f), 3.0f)) << "the next change reaches the first block";
        expect(eq(second->get<float>("gain").value_or(0.0f), 1.0f)) << "a block no parameter of the call reaches keeps its gain";
        expect(eq((*composite)->get<std::int64_t>("count").value_or(0), std::int64_t{1})) << "the refused count was never stored";
    };

    "a literal definition is untouched, and parameters against it are refused"_test = [] {
        registerRecipeTestBlock();
        auto       loader    = recipeTestLoader();
        const auto def       = definitionFrom(kLiteralRecipe);
        const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, def, {});
        expect(composite.has_value()) << (composite.has_value() ? "" : composite.error().message);
        if (!composite.has_value()) {
            return;
        }
        const auto inner = interiorBlock(*composite);
        if (inner == nullptr) {
            return;
        }
        const auto gain = inner->get<float>("gain");
        expect(gain.has_value());
        expect(eq(gain.value_or(float{}), 2.5f));

        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        const auto refusal        = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_unknown_parameter")) << refusal.error().message;
    };
};

namespace qa_composite_calls {

using namespace qa_recipe_definitions;
using gr::property_map;
using gr::settings::Rejected;
using gr::settings::Values;
using gr::testing::textOf;
using gr::testing::waitUntil;
using Entry = gr::settings::Value;

constexpr std::string_view kLevelsRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: levels
    exported_parameters:
      - name: level
        type: float32
      - name: offset
        type: float64
        default: !!float64 0
      - name: taps
        type: float32[]
        default: [1.0, 2.0]
    graph:
      blocks:
        - id: "qa::RecipeWatched"
          parameters:
            name: stage
            gain: "=level"
            rate: "=offset"
        - id: "qa::RecipeScale"
          parameters:
            name: shaper
            gain: "=level * 2"
            taps: "=taps"
      connections:
        - [stage, out, shaper, in]
      exported_ports:
        - [stage, INPUT, in, in]
        - [shaper, OUTPUT, out, out]
)yaml";

// a latched stage that `level` drives, and a shaper that `other` drives
constexpr std::string_view kLatchedRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: latched
    exported_parameters:
      - name: level
        type: float32
      - name: other
        type: float64
        default: !!float64 1
    graph:
      blocks:
        - id: "qa::RecipeLatched"
          parameters:
            name: stage
            gain: "=level"
        - id: "qa::RecipeScale"
          parameters:
            name: shaper
            gain: "=other"
      connections:
        - [stage, out, shaper, in]
      exported_ports:
        - [stage, INPUT, in, in]
        - [shaper, OUTPUT, out, out]
)yaml";

// a held stage and a latched shaper, both driven by `level`
constexpr std::string_view kHeldRecipe = R"yaml(
blocks:
  - id: SUBGRAPH
    parameters:
      name: held
    exported_parameters:
      - name: level
        type: float32
    graph:
      blocks:
        - id: "qa::RecipeHeld"
          parameters:
            name: stage
            gain: "=level"
        - id: "qa::RecipeLatched"
          parameters:
            name: shaper
            gain: "=level * 2"
      connections:
        - [stage, out, shaper, in]
      exported_ports:
        - [stage, INPUT, in, in]
        - [shaper, OUTPUT, out, out]
)yaml";

[[nodiscard]] std::shared_ptr<gr::BlockModel> levelsComposite(float level, std::string_view recipe = kLevelsRecipe) {
    registerRecipeTestBlock();
    auto             loader = recipeTestLoader();
    gr::property_map parameters;
    parameters["level"]  = level;
    const auto composite = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionFrom(recipe), parameters);
    expect(fatal(composite.has_value())) << (composite.has_value() ? "" : composite.error().message);
    return *composite;
}

[[nodiscard]] float floatOf(const std::shared_ptr<gr::BlockModel>& block, std::string_view key) { return block->get<float>(key).value_or(-1.0f); }

// emits zeros until the run stops
struct RecipeSource : gr::Block<RecipeSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RecipeSource, out);

    [[nodiscard]] gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::ranges::fill(outSpan, 0.f);
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }
};

struct RecipeSink : gr::Block<RecipeSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecipeSink, in);

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// emits `nSamples` ones, each tag of `tags` at the first sample of a chunk at its index
struct TaggedOnes : gr::Block<TaggedOnes> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(TaggedOnes, out);

    std::size_t                                       nSamples = 4096UZ;
    std::vector<std::pair<std::size_t, property_map>> tags{};
    std::size_t                                       _sent = 0UZ;

    [[nodiscard]] gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::size_t n = std::min(outSpan.size(), nSamples - _sent);
        for (const auto& [index, map] : tags) {
            if (index > _sent && index < _sent + n) {
                n = index - _sent;
            }
        }
        for (const auto& [index, map] : tags) {
            if (index == _sent && n > 0UZ) {
                outSpan.publishTag(map, 0UZ);
            }
        }
        std::fill_n(outSpan.begin(), n, 1.f);
        outSpan.publish(n);
        _sent += n;
        return _sent >= nSamples ? gr::work::Status::DONE : gr::work::Status::OK;
    }
};

// records each sample and each tag key that reaches it
struct Recorder : gr::Block<Recorder> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Recorder, in);

    std::vector<float>    values{};
    std::set<std::string> tagKeys{};

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        for (const gr::Tag& rawTag : inSpan.rawTags) {
            for (const auto& [key, _] : rawTag.map) {
                tagKeys.emplace(std::string_view(key));
            }
        }
        values.insert(values.end(), inSpan.begin(), inSpan.end());
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

// a source, the composite and a sink, run by runAndWait() on a thread of its own, with a test's ends of the scheduler's
// message ports; a call that waits waits up to 30 s
struct RunningComposite {
    gr::scheduler::Simple<>         scheduler;
    std::shared_ptr<gr::BlockModel> composite;
    gr::MsgPortOut                  toScheduler;
    gr::MsgPortIn                   fromScheduler;
    std::thread                     runner{};

    explicit RunningComposite(std::shared_ptr<gr::BlockModel> built) {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RecipeSource>();
        auto&     sink   = flow.emplaceBlock<RecipeSink>();
        composite        = flow.addBlock(std::move(built));
        expect(fatal(flow.connect(*gr::graph::findBlock(flow, source), "out", composite, "in").has_value()));
        expect(fatal(flow.connect(composite, "out", *gr::graph::findBlock(flow, sink), "in").has_value()));
        expect(fatal(scheduler.set("settings_timeout_ms", gr::Size_t{30000U}).has_value()));
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(toScheduler.connect(scheduler.msgIn).has_value()));
        expect(fatal(scheduler.msgOut.connect(fromScheduler).has_value()));
        runner          = std::thread([this] { std::ignore = scheduler.runAndWait(); });
        const auto runs = [](const std::shared_ptr<gr::BlockModel>& block) { return gr::settings::detail::FrameworkAccess::isRunning(gr::settings::detail::settingsOf(*block)); };
        expect(fatal(waitUntil([this, &runs] { return std::ranges::all_of(composite->graph()->blocks(), runs); }))) << "an interior block of the composite does not run";
    }

    RunningComposite(const RunningComposite&)            = delete;
    RunningComposite& operator=(const RunningComposite&) = delete;

    ~RunningComposite() {
        std::ignore = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
        runner.join();
    }

    void send(property_map changes) { gr::sendMessage<gr::message::Command::Set>(toScheduler, composite->uniqueName(), gr::block::property::kSetting, std::move(changes)); }
};

const boost::ut::suite<"a call on a recipe composite"> compositeCallTests = [] {
    "a call holding a parameter and an own key applies both"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto stage     = interiorBlockNamed(composite, "stage");
        const auto shaper    = interiorBlockNamed(composite, "shaper");
        expect(fatal(stage != nullptr && shaper != nullptr));

        const std::expected<Values, Rejected> both = composite->set(property_map{{"level", 4.0}, {"disconnect_on_done", false}}, gr::settings::noWait);
        expect(both.has_value()) << textOf(both);
        expect(eq(floatOf(stage, "gain"), 4.0f)) << "the parameter reached the stage";
        expect(eq(floatOf(shaper, "gain"), 8.0f)) << "the parameter reached the shaper's expression";
        expect(!composite->get<bool>("disconnect_on_done").value_or(true)) << "the own key applied at the call";
        expect(eq(composite->get<float>("level").value_or(0.0f), 4.0f)) << "the parameter reads as sent";
    };

    "a refused parameter changes no own key, and a refused own key changes no inner block"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto stage     = interiorBlockNamed(composite, "stage");
        expect(fatal(stage != nullptr));

        const std::expected<Values, Rejected> refusedParameter = composite->set(property_map{{"level", 50.0}, {"disconnect_on_done", false}}); // a waiting set checks every inner block first
        expect(fatal(!refusedParameter.has_value()));
        expect(std::ranges::all_of(refusedParameter.error(), [](const auto& entry) { return entry.key == "level"; })) << textOf(refusedParameter);
        expect(refusedParameter.error().front().reason.contains("stage")) << "the reason names the inner block: " << textOf(refusedParameter);
        expect(composite->get<bool>("disconnect_on_done").value_or(false)) << "the own key stays";
        expect(eq(floatOf(stage, "gain"), 1.0f)) << "no inner block changes";

        const std::expected<Values, Rejected> refusedOwnKey = composite->set(property_map{{"level", 5.0}, {"levle", 1.0}}, gr::settings::noWait);
        expect(fatal(!refusedOwnKey.has_value()));
        expect(eq(refusedOwnKey.error().size(), 1UZ) && refusedOwnKey.error().front().key == "levle") << textOf(refusedOwnKey);
        expect(eq(floatOf(stage, "gain"), 1.0f)) << "the inner blocks stay";
        expect(eq(composite->get<float>("level").value_or(0.0f), 1.0f)) << "the parameter stays";
    };

    "a no-wait parameter a free inner block refuses fails naming that block, which notifies its key, and stays stored"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto stage     = interiorBlockNamed(composite, "stage");
        const auto shaper    = interiorBlockNamed(composite, "shaper");
        expect(fatal(stage != nullptr && shaper != nullptr));

        gr::testing::CerrCapture              captured;
        const std::expected<Values, Rejected> result = composite->set(property_map{{"level", 50.0}}, gr::settings::noWait);
        expect(fatal(result.has_value() && result->contains("level"))) << textOf(result);
        expect(result->at("level").isFailed() && result->at("level").reason.contains("stage")) << textOf(result);
        expect(ge(captured.lines("'gain': out of range"), 1UZ)) << "the stage did not notify its key: " << captured.str();
        expect(eq(composite->get<float>("level").value_or(0.0f), 50.0f)) << "the composite did not keep the stored parameter";
        expect(eq(floatOf(stage, "gain"), 1.0f)) << "the stage took the value it refused";
        expect(eq(floatOf(shaper, "gain"), 100.0f)) << "the shaper did not take the value";
    };

    "a check of a parameter refuses as the call does and stores nothing"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto stage     = interiorBlockNamed(composite, "stage");
        expect(fatal(stage != nullptr));
        const std::expected<void, Rejected> refused = composite->check(property_map{{"level", 50.0}});
        expect(fatal(!refused.has_value()));
        expect(eq(refused.error().front().key, std::string("level")));
        expect(composite->check(property_map{{"level", 6.0}}).has_value());
        expect(eq(floatOf(stage, "gain"), 1.0f)) << "a check reaches no inner block";
        expect(eq(composite->get<float>("level").value_or(0.0f), 1.0f)) << "a check stores no parameter";
    };

    "set(get()) on a composite changes nothing"_test = [] {
        const auto composite = levelsComposite(3.0f);
        const auto stage     = interiorBlockNamed(composite, "stage");
        const auto shaper    = interiorBlockNamed(composite, "shaper");
        expect(fatal(stage != nullptr && shaper != nullptr));
        const std::expected<Values, Rejected> read = composite->get();
        expect(fatal(read.has_value())) << textOf(read);
        expect(read->contains("level") && read->contains("offset") && read->contains("taps")) << "get() adds every parameter";
        const std::expected<Values, Rejected> again = composite->set(gr::testing::writableValues(gr::settings::detail::settingsOf(*composite), read), gr::settings::noWait);
        expect(again.has_value()) << textOf(again);
        expect(eq(floatOf(stage, "gain"), 3.0f));
        expect(eq(floatOf(shaper, "gain"), 6.0f));
        expect(eq(composite->get<float>("level").value_or(0.0f), 3.0f));
    };

    "a vector parameter converts each element to its declared type"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto shaper    = interiorBlockNamed(composite, "shaper");
        expect(fatal(shaper != nullptr));
        expect(setParameters(composite, property_map{{"taps", std::vector<double>{0.5, 0.25}}}).has_value());
        const std::expected<std::vector<float>, Rejected> stored = composite->get<std::vector<float>>("taps");
        expect(fatal(stored.has_value())) << "the parameter reads back in its declared element type";
        expect(*stored == std::vector<float>{0.5f, 0.25f});
        expect(shaper->get<std::vector<float>>("taps").value_or(std::vector<float>{}) == std::vector<float>{0.5f, 0.25f}) << "the inner key receives the floats";

        const std::expected<void, std::string> refused = setParameters(composite, property_map{{"taps", std::vector<std::string>{"a"}}});
        expect(!refused.has_value()) << "an element the type cannot hold refuses the parameter";
    };

    "on a running composite a parameter whose inner block runs another value names it"_test = [] {
        RunningComposite run(levelsComposite(1.0f));
        const auto       stage = interiorBlockNamed(run.composite, "stage");

        const std::expected<Values, Rejected> rounded = run.composite->set(property_map{{"level", 2.4}});
        expect(fatal(rounded.has_value())) << textOf(rounded);
        expect(fatal(rounded->contains("level"))) << textOf(rounded);
        const Entry& level = rounded->at("level");
        expect(level.isValue() && eq(level.as<float>().value_or(0.0f), 2.4f)) << "the parameter's entry carries its stored value";
        expect(level.reason.contains("stage") && level.reason.contains("gain") && level.reason.contains("runs")) << level.reason;
        expect(eq(run.composite->get<float>("level").value_or(0.0f), 2.4f)) << "the parameter reads as sent";
        expect(eq(floatOf(stage, "gain"), 2.0f)) << "the stage runs the whole gain";
    };

    "a tag on a parameter's key reaches each inner block at its sample, a repeated one stages nothing and a refused one changes none"_test = [] {
        const auto composite = levelsComposite(1.0f);
        expect(fatal(composite->setTagCouplings(property_map{{"level", std::string("qa:level")}, {"offset", std::string("qa:offset")}}).has_value()));
        auto* stage = static_cast<RecipeWatched*>(interiorBlockNamed(composite, "stage")->raw());

        gr::Graph   flow;
        TaggedOnes& source                           = flow.emplaceBlock<TaggedOnes>();
        Recorder&   sink                             = flow.emplaceBlock<Recorder>();
        source.tags                                  = {{1000UZ, property_map{{"qa:level", 3.0}, {"qa:offset", 2.0}}}, {2000UZ, property_map{{"qa:level", 3.0}}}, {3000UZ, property_map{{"qa:level", 50.0}}}};
        const std::shared_ptr<gr::BlockModel>& added = flow.addBlock(composite);
        expect(fatal(flow.connect(*gr::graph::findBlock(flow, source), "out", added, "in").has_value()));
        expect(fatal(flow.connect(added, "out", *gr::graph::findBlock(flow, sink), "in").has_value()));
        const std::size_t nGainSetsBefore = stage->nGainSets;

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));

        expect(fatal(eq(sink.values.size(), 4096UZ)));
        const auto firstAt = [&sink](float value) { return static_cast<std::size_t>(std::distance(sink.values.begin(), std::ranges::find(sink.values, value))); };
        expect(eq(firstAt(18.0f), 1000UZ)) << "the stage's 3 and the shaper's 6 are in force at the tag's sample";
        expect(std::ranges::all_of(sink.values | std::views::take(1000), [](float value) { return value == 2.0f; })) << "no sample before the tag sees the new values";
        expect(std::ranges::all_of(sink.values | std::views::drop(1000), [](float value) { return value == 18.0f; })) << "the refused tag changes no inner block";
        expect(eq(stage->nGainSets, nGainSetsBefore + 1UZ)) << "the repeated tag stages nothing";
        expect(eq(composite->get<float>("level").value_or(0.0f), 3.0f)) << "the tag stores the parameter";
        expect(eq(composite->get<double>("offset").value_or(0.0), 2.0)) << "the tag stores the other parameter";
        expect(!gr::settings::detail::FrameworkAccess::isGiven(gr::settings::detail::settingsOf(*composite), "offset")) << "a tag's value makes no parameter given";
        expect(sink.tagKeys.contains("qa:level")) << "the tag goes on down the stream";
        expect(std::ranges::none_of(sink.tagKeys, [](const std::string& key) { return key.starts_with(gr::settings::kInnerTagKeyPrefix); })) << "the composite's output drops the inner tag keys";
    };

    "a recipe nested in a recipe passes the outer composite's inner tag keys to the blocks after it"_test = [] {
        const std::shared_ptr<gr::BlockModel>  nested = levelsComposite(1.0f);
        auto                                   outer  = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        gr::Graph&                             inner  = *outer->graph();
        RecipeScale&                           head   = inner.emplaceBlock<RecipeScale>(property_map{{"name", std::string("head")}});
        const std::shared_ptr<gr::BlockModel>& held   = inner.addBlock(nested);
        RecipeScale&                           tail   = inner.emplaceBlock<RecipeScale>(property_map{{"name", std::string("tail")}});
        expect(fatal(inner.connect(*gr::graph::findBlock(inner, head), "out", held, "in").has_value()));
        expect(fatal(inner.connect(held, "out", *gr::graph::findBlock(inner, tail), "in").has_value()));
        expect(fatal(outer->exportPort(true, head.unique_name, gr::PortDirection::INPUT, "in", "in").has_value()));
        expect(fatal(outer->exportPort(true, tail.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value()));
        std::vector<ParameterDeclaration> declarations{{.name = "boost", .type = "float32", .defaultValue = gr::pmt::Value(1.0f), .doc = ""}};
        std::vector<gr::recipe::Binding>  bindings{{.namePath = {"tail"}, .settingKey = "gain", .expression = {}, .substituted = 0UZ, .sequence = {}}};
        expect(fatal(outer->attachRecipeBindings({.declarations = std::move(declarations), .bindings = std::move(bindings), .values = {gr::pmt::Value(1.0f)}}).has_value()));
        expect(fatal(outer->setTagCouplings(property_map{{"boost", std::string("qa:boost")}}).has_value()));

        gr::Graph   flow;
        TaggedOnes& source                           = flow.emplaceBlock<TaggedOnes>();
        Recorder&   sink                             = flow.emplaceBlock<Recorder>();
        source.tags                                  = {{1000UZ, property_map{{"qa:boost", 3.0}}}};
        const std::shared_ptr<gr::BlockModel>& added = flow.addBlock(outer);
        expect(fatal(flow.connect(*gr::graph::findBlock(flow, source), "out", added, "in").has_value()));
        expect(fatal(flow.connect(added, "out", *gr::graph::findBlock(flow, sink), "in").has_value()));

        gr::scheduler::Simple<> scheduler;
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));

        expect(fatal(eq(sink.values.size(), 4096UZ)));
        expect(eq(tail.settings.gain.value, 3.0f)) << "the tail after the nested recipe takes the tag";
        expect(std::ranges::all_of(sink.values | std::views::take(1000), [](float value) { return value == 2.0f; })) << "no sample before the tag sees the new gain";
        expect(std::ranges::all_of(sink.values | std::views::drop(1000), [](float value) { return value == 6.0f; })) << "the tail runs the new gain from the tag's sample";
        expect(std::ranges::none_of(sink.tagKeys, [](const std::string& key) { return key.starts_with(gr::settings::kInnerTagKeyPrefix); })) << "the outer composite's output drops its inner tag keys";
    };

    "a call reaches the inner block its binding names as the graph stands, not one the graph no longer holds"_test = [] {
        const auto composite = levelsComposite(1.0f);
        const auto removed   = composite->graph()->removeBlockByName(interiorBlockNamed(composite, "shaper")->uniqueName());
        expect(fatal(removed.has_value()));
        std::ignore = composite->graph()->emplaceBlock<RecipeScale>(property_map{{"name", std::string("shaper")}});

        expect(setParameters(composite, property_map{{"level", 4.0}}).has_value());
        expect(eq(floatOf(interiorBlockNamed(composite, "shaper"), "gain"), 8.0f)) << "the block now named shaper takes the derived gain";
        expect(eq(floatOf(*removed, "gain"), 2.0f)) << "the removed block keeps its gain";
    };

    "a plain subgraph's own key applies at the call on a running graph"_test = [] {
        auto  subgraph = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto& stage    = subgraph->graph()->emplaceBlock<RecipeWatched>(property_map{{"name", std::string("stage")}});
        expect(fatal(subgraph->exportPort(true, stage.unique_name, gr::PortDirection::INPUT, "in", "in").has_value()));
        expect(fatal(subgraph->exportPort(true, stage.unique_name, gr::PortDirection::OUTPUT, "out", "out").has_value()));
        RunningComposite run(subgraph);

        const std::expected<Values, Rejected> result = run.composite->set(property_map{{"disconnect_on_done", false}}, gr::settings::noWait);
        expect(result.has_value()) << textOf(result);
        expect(!run.composite->get<bool>("disconnect_on_done").value_or(true)) << "no work call reaches a subgraph, so the key applies at the call";
    };

    "a call while an inner block's check hook holds its settings is refused at its timeout, and stores nothing"_test = [] {
        const auto                            composite  = levelsComposite(1.0f, kLatchedRecipe);
        const std::shared_ptr<gr::BlockModel> stageModel = interiorBlockNamed(composite, "stage");
        RecipeLatched*                        stage      = static_cast<RecipeLatched*>(stageModel->raw());
        stage->holdAtGain                                = 3.0f; // the direct set's check, which holds the stage's block lock

        std::thread holder([&stageModel] { std::ignore = stageModel->set(property_map{{"gain", 3.0f}}, gr::settings::noWait); });
        expect(fatal(waitUntil([stage] { return stage->held.load(); }))) << "the direct set reached the stage's check";
        std::atomic<bool>               returned{false};
        std::expected<Values, Rejected> timedOut;
        std::thread                     caller([&] {
            timedOut = composite->set(property_map{{"level", 2.0}, {"other", 3.0}}, std::chrono::milliseconds(50));
            returned.store(true);
        });
        const bool                      returnedWhileHeld = waitUntil([&returned] { return returned.load(); });
        stage->released                                   = true;
        caller.join();
        holder.join();

        expect(returnedWhileHeld) << "the call waited for the stage's block lock past its timeout";
        expect(fatal(!timedOut.has_value())) << "the call was taken while the stage's settings were held";
        expect(fatal(eq(timedOut.error().size(), 1UZ))) << textOf(timedOut);
        expect(eq(timedOut.error().front().key, std::string("level")));
        const std::string& reason = timedOut.error().front().reason;
        expect(reason.contains("timed out after ") && reason.ends_with(" ms waiting for the block's settings")) << reason;
        expect(eq(composite->get<float>("level").value_or(0.0f), 1.0f)) << "the refused call stored its parameter";
        expect(eq(composite->get<double>("other").value_or(0.0), 1.0)) << "the refused call stored the parameter its shaper passed";
        expect(eq(floatOf(stageModel, "gain"), 3.0f)) << "the stage did not take the direct set";
        expect(eq(floatOf(interiorBlockNamed(composite, "shaper"), "gain"), 1.0f)) << "the refused call merged the shaper's change";
    };

    "six no-wait sets of a parameter during an inner block's held apply hook return pending, and the sixth reaches every inner block"_test = [] {
        RunningComposite run(levelsComposite(1.0f, kHeldRecipe));
        const auto       stageModel = interiorBlockNamed(run.composite, "stage");
        const auto       shaper     = interiorBlockNamed(run.composite, "shaper");
        auto*            stage      = static_cast<RecipeHeld*>(stageModel->raw());
        stage->holdAtGain           = 2.0f;

        expect(fatal(run.composite->set(property_map{{"level", 2.0}}, gr::settings::noWait).has_value()));
        expect(fatal(waitUntil([stage] { return stage->held.load(); }))) << "the stage's apply hook did not run the gain";
        const std::size_t appliesHeld = stage->nApplies.load();
        // a set that waited for the stage's lock would wait for the release below, which comes after the last of them
        std::vector<std::expected<Values, Rejected>> results;
        for (int level = 3; level <= 8; ++level) {
            results.push_back(run.composite->set(property_map{{"level", static_cast<double>(level)}}, gr::settings::noWait));
        }
        stage->released = true;

        for (const std::expected<Values, Rejected>& result : results) {
            expect(fatal(result.has_value())) << textOf(result);
            expect(result->at("level").isPending()) << textOf(result);
        }
        expect(fatal(waitUntil([&] { return floatOf(stageModel, "gain") == 8.0f && floatOf(shaper, "gain") == 16.0f; }))) << "the sixth value did not reach every inner block";
        expect(eq(run.composite->get<float>("level").value_or(0.0f), 8.0f)) << "the parameter does not hold the sixth value";
        expect(eq(stage->nApplies.load(), appliesHeld + 1UZ)) << "an earlier queued value reached the stage's hook";
    };

    "a tag whose value a parameter's inner block refuses notifies the refusal and changes nothing"_test = [] {
        const auto composite = levelsComposite(1.0f);
        expect(fatal(composite->setTagCouplings(property_map{{"level", std::string("qa:level")}}).has_value()));

        gr::Graph   flow;
        TaggedOnes& source                           = flow.emplaceBlock<TaggedOnes>();
        Recorder&   sink                             = flow.emplaceBlock<Recorder>();
        source.tags                                  = {{1000UZ, property_map{{"qa:level", 50.0}}}};
        const std::shared_ptr<gr::BlockModel>& added = flow.addBlock(composite);
        expect(fatal(flow.connect(*gr::graph::findBlock(flow, source), "out", added, "in").has_value()));
        expect(fatal(flow.connect(added, "out", *gr::graph::findBlock(flow, sink), "in").has_value()));
        gr::scheduler::Simple<>                 scheduler;
        std::mutex                              readLock;
        std::vector<gr::settings::Notification> read;
        scheduler.installReader([&readLock, &read](const gr::settings::Notification& notification) {
            std::lock_guard lock(readLock);
            read.push_back(notification);
        });
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(fatal(scheduler.runAndWait().has_value()));

        std::lock_guard lock(readLock);
        std::string     readText;
        for (const gr::settings::Notification& notification : read) {
            readText += std::format("[{} {} {}: {}] ", notification.block, notification.key, gr::settings::notificationLevelName(notification.level), notification.reason);
        }
        expect(std::ranges::any_of(read, [](const gr::settings::Notification& notification) { return notification.level == gr::settings::NotificationLevel::error && notification.reason.contains("level: tag value refused") && notification.reason.contains("stage"); })) << "the refusal reached the reader: " << readText;
        expect(eq(composite->get<float>("level").value_or(0.0f), 1.0f)) << "the parameter stays";
        expect(std::ranges::all_of(sink.values, [](float value) { return value == 2.0f; })) << "no inner block changes";
    };

    "a Set message holding a parameter and an own key applies both"_test = [] {
        RunningComposite run(levelsComposite(1.0f));
        const auto       stage = interiorBlockNamed(run.composite, "stage");

        run.send(property_map{{"level", 3.0}, {"disconnect_on_done", false}});
        expect(fatal(waitUntil([&] { return floatOf(stage, "gain") == 3.0f; }))) << "the parameter did not apply";
        expect(fatal(waitUntil([&] { return !run.composite->get<bool>("disconnect_on_done").value_or(true); }))) << "the own key did not apply";
    };

    "an export change on a running recipe composite is refused naming the run, and applies once the composite is stopped"_test = [] {
        {
            RunningComposite                     run(levelsComposite(1.0f));
            const std::string                    stageName = std::string(interiorBlockNamed(run.composite, "stage")->uniqueName());
            const gr::property_map               before    = run.composite->exportedInputPorts();
            const std::expected<void, gr::Error> refused   = run.composite->exportPort(false, stageName, gr::PortDirection::INPUT, "in", {});
            expect(fatal(!refused.has_value())) << "a running composite changed its exports";
            expect(refused.error().message.contains("not allowed while running")) << refused.error().message;
            expect(run.composite->exportedInputPorts() == before) << "the refused change left the exports changed";
        }
        const auto                           stopped   = levelsComposite(1.0f);
        const std::string                    stageName = std::string(interiorBlockNamed(stopped, "stage")->uniqueName());
        const std::expected<void, gr::Error> removed   = stopped->exportPort(false, stageName, gr::PortDirection::INPUT, "in", {});
        expect(removed.has_value()) << (removed.has_value() ? std::string() : removed.error().message);
    };
};

} // namespace qa_composite_calls
