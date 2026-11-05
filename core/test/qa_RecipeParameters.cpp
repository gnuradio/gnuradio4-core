#include <boost/ut.hpp>

#include <numbers>
#include <string>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/RecipeParameters.hpp>

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
    const auto result = evaluate(*expression, values);
    expect(result.has_value()) << source;
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

        const gr::recipe::Binding substituted{.namePath = {"inner"}, .settingKey = "detector", .expression = {}, .substituted = 0UZ};
        const auto                bound = gr::recipe::bindingValue(substituted, std::span<const gr::pmt::Value>(values));
        expect(bound.has_value());
        const auto* text = bound->get_if<std::pmr::string>();
        expect(text != nullptr) << "the string arrives as a string, not converted";
        if (text != nullptr) {
            expect(eq(std::string_view(*text), std::string_view("gardner")));
        }

        const gr::recipe::Binding vectorBound{.namePath = {"inner"}, .settingKey = "carriers", .expression = {}, .substituted = 1UZ};
        const auto                carriers = gr::recipe::bindingValue(vectorBound, std::span<const gr::pmt::Value>(values));
        expect(carriers.has_value());
        expect(carriers->get_if<gr::Tensor<std::int32_t>>() != nullptr) << "the sequence arrives whole";

        // an index past the values is a refusal rather than a read off the end
        const gr::recipe::Binding outOfRange{.namePath = {"inner"}, .settingKey = "x", .expression = {}, .substituted = 9UZ};
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
        const gr::recipe::Binding         toggle{.namePath = {"inner"}, .settingKey = "enabled", .expression = {}, .substituted = 0UZ};
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

    gr::Annotated<float, "gain">        gain  = 1.0f;
    gr::Annotated<float, "rate">        rate  = 0.0f;
    gr::Annotated<std::string, "label"> label = "";

    GR_MAKE_REFLECTABLE(RecipeScale, in, out, gain, rate, label);

    explicit RecipeScale(gr::property_map init = {}) : gr::Block<RecipeScale>(std::move(init)) {}

    [[nodiscard]] float processOne(float sample) const noexcept { return gain * sample; }
};

void registerRecipeTestBlock() {
    static const bool registered = [] { return gr::globalBlockRegistry().insert<RecipeScale>("=qa::RecipeScale"); }();
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

[[nodiscard]] std::shared_ptr<gr::BlockModel> interiorBlock(const std::shared_ptr<gr::BlockModel>& composite) {
    // expect() does not abort, so a broken composite must be answered with null, never a dereference
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->blocks().empty()) {
        expect(false) << "the definition must produce a composite with an interior block";
        return nullptr;
    }
    return composite->graph()->blocks().front();
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
        const auto   gain         = inner->settings().get("gain");
        expect(gain.has_value());
        expect(eq(gain->value_or(float{}), static_cast<float>(expectedGain))) << "the derivation reached the interior setting";
        const auto rate = inner->settings().get("rate");
        expect(rate.has_value());
        expect(eq(rate->value_or(float{}), 48000.0f)) << "a bare reference forwards";
        const auto label = inner->settings().get("label");
        expect(label.has_value());
        expect(eq(std::string(label->value_or(std::string_view{})), std::string("=verbatim"))) << "the escape yields a literal '='";
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
        const auto gain = inner->settings().get("gain");
        expect(gain.has_value());
        expect(eq(gain->value_or(float{}), 2.5f));

        gr::property_map parameters;
        parameters["sample_rate"] = 48000.0f;
        const auto refusal        = gr::detail::instantiateBlockFromYamlDefinition(loader, def, parameters);
        expect(!refusal.has_value());
        expect(refusal.error().message.contains("recipe_unknown_parameter")) << refusal.error().message;
    };
};

int main() { /* not needed for ut */ }
