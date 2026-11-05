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

    gr::Annotated<float, "gain">              gain  = 1.0f;
    gr::Annotated<float, "rate">              rate  = 0.0f;
    gr::Annotated<std::string, "label">       label = "";
    gr::Annotated<std::vector<float>, "taps"> taps{};

    GR_MAKE_REFLECTABLE(RecipeScale, in, out, gain, rate, label, taps);

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

[[nodiscard]] std::size_t interiorEdgeBufferSize(const std::shared_ptr<gr::BlockModel>& composite) {
    if (composite == nullptr || composite->graph() == nullptr || composite->graph()->edges().empty()) {
        expect(false) << "the definition must produce a composite with an interior edge";
        return 0UZ;
    }
    return composite->graph()->edges().front().minBufferSize();
}

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
        const auto refused  = wrapper->applyRecipeParameters(beyond);
        expect(!refused.has_value()) << "1e300 does not fit the float32 deviation";
        if (!refused.has_value()) {
            expect(refused.error().message.contains("deviation") && refused.error().message.contains("float32")) << refused.error().message;
        }
        gr::property_map atEdge;
        atEdge["deviation"] = static_cast<double>(std::numeric_limits<float>::max());
        const auto applied  = wrapper->applyRecipeParameters(atEdge);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
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
        const auto applied  = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto staged = inner->settings().stagedParameters();
        const auto gainIt = staged.find("gain");
        expect(gainIt != staged.end()) << "the derived setting is staged on the interior block";
        if (gainIt != staged.end()) {
            // setStaged converts to the member's own float; read whichever numeric arrived
            const double stagedGain = gr::recipe::detail::doubleOf(gainIt->second).value_or(0.0);
            expect(eq(static_cast<float>(stagedGain), static_cast<float>(48000.0 / (2.0 * std::numbers::pi * 5000.0)))) << "re-evaluated with the changed parameter";
        }

        gr::property_map unknown;
        unknown["bandwidth"] = 1.0f;
        expect(!wrapper->applyRecipeParameters(unknown).has_value()) << "an undeclared parameter refuses";

        gr::property_map mistyped;
        mistyped["deviation"]  = std::pmr::string("wide");
        const auto refusedType = wrapper->applyRecipeParameters(mistyped);
        expect(!refusedType.has_value()) << "a live value its declared type cannot hold refuses";
        if (!refusedType.has_value()) {
            expect(refusedType.error().message.contains("float32")) << refusedType.error().message;
        }

        gr::property_map zero;
        zero["deviation"] = 0.0f;
        expect(!wrapper->applyRecipeParameters(zero).has_value()) << "a non-finite derivation refuses the change whole";

        gr::property_map next;
        next["sample_rate"]    = 96000.0f;
        const auto nextApplied = wrapper->applyRecipeParameters(next);
        expect(nextApplied.has_value()) << (nextApplied.has_value() ? "" : nextApplied.error().message);
        const auto stagedAfter = inner->settings().stagedParameters();
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

    "an expression in a context's parameters or a tag coupling is refused by name"_test = [] {
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

        const auto contextual = gr::detail::instantiateBlockFromYamlDefinition(loader, definitionWith("          ctx_parameters:\n            - context: tuned\n              time: 0\n              parameters:\n                gain: \"=sample_rate\"\n"), parameters);
        expect(!contextual.has_value()) << "an expression in ctx_parameters is not evaluated, and is refused";
        if (!contextual.has_value()) {
            expect(contextual.error().message.contains("ctx_parameters")) << contextual.error().message;
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

        const auto taps = inner->settings().get("taps");
        expect(taps.has_value());
        const auto* derived = taps.has_value() ? taps->get_if<gr::Tensor<float>>() : nullptr;
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
        const auto applied = wrapper->applyRecipeParameters(change);
        expect(applied.has_value()) << (applied.has_value() ? "" : applied.error().message);
        const auto staged = inner->settings().stagedParameters();
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
