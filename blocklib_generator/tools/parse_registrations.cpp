/// parse_registrations.cpp
/// ---------------------------------------------------------------------------
/// This parser scans a single .hpp for lines containing:
///
///   GR_REGISTER_BLOCK("OptionalQuotedName", MyTemplate, (paramPack?), [ expansions ]...)
///
/// for example:
///   GR_REGISTER_BLOCK("MyBlockName", gr::basic::Block1, ([T], [U]), [ float, double ], [int])
///   GR_REGISTER_BLOCK(gr::basic::Block0)
///   GR_REGISTER_BLOCK("blockN.hpp", gr::basic::BlockN, ([T],[U],3UZ,SomeAlgo<[T]>), [ short, int], [double])
///
/// * the GR_REGISTER_BLOCK macros must be each be on a single line (no multi-line support).
///
/// Usage:
///   parse_registrations <headerFile.hpp> <outputDir> [--split | -s] [--max-per-tu <N>]
///                       [--registry-header <include>] [--registry-instance <expression>]
///   e.g. parse_registrations block0.hpp build/generated
///        parse_registrations blockN.hpp build/generated --split
///        parse_registrations blockN.hpp build/generated --max-per-tu 16
///        parse_registrations plugin.hpp build/generated --registry-header plugin_instance.hpp --registry-instance grPluginInstance
///
/// Each registered (block, type) produces two translation units:
///
/// * a DEFINITION unit, <stem>_block_<n>.cpp, which names the block type: it defines the factory
///   that builds a BlockWrapper<TBlock>, explicitly instantiates that wrapper, and exports the
///   registry key and alias the typed insert<TBlock>() path would derive. This is the only unit
///   that parses the block header, and the only one that materialises the framework's per-type
///   machinery.
/// * a share of a REGISTRATION unit, which declares the definition unit's exported symbol and
///   registers it through gr::insertBlockFactory(). A registration unit instantiates nothing; it
///   includes gnuradio-4.0/BlockRegistration.hpp and, when one is named, the registry header.
///
/// Both generated initializers -- gr_blocklib_init_unit_<stem> per registration unit, and
/// gr_blocklib_init_module_<module> over all of them -- return the number of blocks the call newly
/// entered into the registry they are given. Registering a block the registry already holds is a
/// no-op and not a failure, so calling an initializer on a registry the definition units' own static
/// initializers have already populated returns zero and changes nothing. That makes initialization
/// idempotent and safe to repeat, and still leaves a caller building its own registry a usable
/// count.
///
/// The switches below group the registration units; definition units are always one per (block, type).
/// * default: each macro line => one registration .cpp file.
/// * --split (-s): cartesian expansion -> each block-type combination gets its own registration .cpp.
/// * --max-per-tu <N>: balanced chunking -> all registrations of the header (across macro lines)
///   are packed into registration .cpp files of at most N entries each.
///
/// The registry options name where the registrations go:
/// * --registry-instance <expression>: the registry the registration units insert into, called as
///   <expression>(); defaults to gr::globalBlockRegistry.
/// * --registry-header <include>: the header that declares that instance. The definition units
///   include it in place of gnuradio-4.0/BlockRegistry.hpp, and the registration units include it
///   as well, because gnuradio-4.0/BlockRegistration.hpp declares the default instances alone.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <numeric>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr std::string_view kRegistrationMacroName = "GR_REGISTER_BLOCK";

namespace detail {

[[nodiscard]] constexpr std::string_view trim(std::string_view sv) noexcept {
    constexpr static auto is_space = [](unsigned char c) constexpr { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
    constexpr static auto notSpace = [](char c) constexpr { return !is_space(c); };

    const auto first = std::ranges::find_if(sv, notSpace);
    if (first == sv.end()) {
        return {};
    }
    const auto last = std::ranges::find_if(sv.rbegin(), sv.rend(), notSpace).base();
    return sv.substr(static_cast<std::size_t>(first - sv.begin()), static_cast<std::size_t>(last - first));
}

constexpr std::expected<std::vector<std::string_view>, std::string> splitTopLevelCommaSeparatedValues(std::string_view input) {
    std::vector<std::string_view> tokens;
    std::vector<char>             bracketStack;
    std::size_t                   start = 0;

    input = trim(input);
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '(' || c == '[') {
            bracketStack.push_back(c == '(' ? ')' : ']');
        } else if (c == ')' || c == ']') {
            if (bracketStack.empty() || bracketStack.back() != c) {
                return std::unexpected("mismatched bracket");
            }
            bracketStack.pop_back();
        } else if (c == ',' && bracketStack.empty()) {
            tokens.push_back(trim(input.substr(start, i - start)));
            start = i + 1;
        }
    }

    if (!bracketStack.empty()) {
        return std::unexpected("mismatched bracket");
    }

    tokens.push_back(trim(input.substr(start)));
    return tokens;
}

} // namespace detail

struct Options {
    std::filesystem::path headerPath;
    std::filesystem::path outDir;
    std::string           registryHeader      = "gnuradio-4.0/BlockRegistry.hpp";
    std::string           registryInstance    = "gr::globalBlockRegistry";
    bool                  registryHeaderGiven = false; // a named header declares the instance, so the registration units include it too
    bool                  expansionsSplit     = false; // true: each block instantiation creates its own file
    std::size_t           maxPerTu            = 0UZ;   // 0: one file per macro line; >0: balanced chunks of <= maxPerTu registrations

    Options(int argc, char** argv) {
        headerPath = argv[1];
        outDir     = argv[2];
        for (int index = 3; index < argc; index++) {
            if ((std::strcmp(argv[index], "--split") == 0) || (std::strcmp(argv[index], "-s") == 0)) {
                expansionsSplit = true;
            } else if (argc > index + 1 && (std::strcmp(argv[index], "--registry-header") == 0)) {
                registryHeader      = argv[index + 1];
                registryHeaderGiven = true;
            } else if (argc > index + 1 && (std::strcmp(argv[index], "--registry-instance") == 0)) {
                registryInstance = argv[index + 1];
            } else if (argc > index + 1 && (std::strcmp(argv[index], "--max-per-tu") == 0)) {
                maxPerTu = static_cast<std::size_t>(std::strtoul(argv[index + 1], nullptr, 10));
            }
        }
    }
};

struct RegisterBlock {
    std::string_view                           baseName;     // e.g. "MyBlock" if quoted
    std::string_view                           templateName; // e.g. "gr::basic::Block1"
    std::string_view                           paramPack;    // e.g. "([T], [U])"
    std::vector<std::vector<std::string_view>> expansions;   // e.g. [float, double], [int] => expansions = { {"float","double"}, {"int"} }
};

std::ofstream openFile(std::string fileName) {
    std::cout << std::format("\t=> Generating file: '{}'\n", fileName);
    std::ofstream result(fileName);
    if (!result.is_open()) {
        throw std::format("Failed to open '{}' for writing", fileName);
    }
    return result;
}

struct GeneratedFiles {
    std::ofstream registrations;
    std::ofstream declarations;
    std::ofstream rawCalls;

    GeneratedFiles(std::string outFileBase)
        : registrations(openFile(outFileBase + ".cpp")),      //
          declarations(outFileBase + "_declarations.hpp.in"), //
          rawCalls(outFileBase + "_raw_calls.hpp.in") {}
};

struct InstantiationNames {
    std::string templateName; // e.g. "gr::basic::Block1<float, double>"
    std::string finalName;    // registry name override, empty if none
};

struct PendingRegistration {
    std::string templateName;
    std::string finalName;
    std::size_t lineNum;
    std::size_t macroIndex;
};

// the symbol a definition unit exports and a registration unit declares
[[nodiscard]] std::string registrationSymbol(std::string_view stem, std::size_t index) { return std::format("gr_blocklib_registration_{}_{}", stem, index); }

[[nodiscard]] std::string factorySymbol(std::string_view stem, std::size_t index) { return std::format("gr_blocklib_factory_{}_{}", stem, index); }

// the only unit that names the block type, and so the only one that materialises it
void emitDefinitionUnit(std::string_view commandPath, std::string_view stem, std::size_t index, const PendingRegistration& registration, const Options& options) {
    std::ofstream fout = openFile((options.outDir / std::format("{}_block_{}.cpp", stem, index)).string());

    fout << std::format("// auto-generated by {}, do not edit.\n", commandPath);
    fout << std::format("#include <{}>\n#include \"{}\" // for details: {}:{}\n\n", options.registryHeader, options.headerPath.string(), options.headerPath.string(), registration.lineNum);
    fout << "#ifdef GR_ENABLE_BLOCK_REGISTRY\n\n";
    // The factory below is what materialises the wrapper: constructing and destroying it here
    // odr-uses every member this unit must emit, with ordinary vague linkage. An explicit
    // `template class gr::BlockWrapper<...>` on top of it adds nothing a consumer can link
    // against — no unit declares an extern template — and under Clang it suppresses emission of
    // the wrapped members' implicitly instantiated destructors, which then stay undefined in the
    // whole build.
    fout << std::format("std::unique_ptr<gr::BlockModel> {}(gr::property_map params) {{ return std::make_unique<gr::BlockWrapper<{}>>(std::move(params)); }}\n\n", factorySymbol(stem, index), registration.templateName);
    fout << std::format("const gr::BlockRegistration& {}() {{\n", registrationSymbol(stem, index));
    fout << std::format("    static const gr::BlockRegistration registration = gr::makeBlockRegistration<{}, \"{}\">(&{});\n", registration.templateName, registration.finalName, factorySymbol(stem, index));
    fout << "    return registration;\n}\n\n";
    fout << "#endif // GR_ENABLE_BLOCK_REGISTRY\n";
    fout << "// end of auto-generated code\n";
}

// declaration-only: it names no block type, so it instantiates nothing
[[nodiscard]] std::string emitRegistrationUnit(std::string_view commandPath, std::string_view stem, std::string_view namePrefix, const std::vector<std::size_t>& indices, const std::vector<PendingRegistration>& registrations, const Options& options) {
    GeneratedFiles output((options.outDir / namePrefix).string());

    output.registrations << std::format("// auto-generated by {}, do not edit.\n", commandPath);
    if (options.registryHeaderGiven) {
        output.registrations << std::format("#include <{}>\n", options.registryHeader);
    }
    output.registrations << "#include <gnuradio-4.0/BlockRegistration.hpp>\n\n#include <cstddef>\n\n";
    output.registrations << "#ifdef GR_ENABLE_BLOCK_REGISTRY\n\n";
    for (std::size_t index : indices) {
        output.registrations << std::format("extern const gr::BlockRegistration& {}(); // {} -- for details: {}:{}\n", registrationSymbol(stem, index), registrations[index].templateName, options.headerPath.string(), registrations[index].lineNum);
    }

    const std::string initFunction = std::format("gr_blocklib_init_unit_{}", namePrefix);
    output.registrations << std::format("\nextern \"C\" {{\nGNURADIO_EXPORT std::size_t {}(gr::BlockRegistry& registry) {{\n    std::size_t result = 0UZ;\n", initFunction);
    for (std::size_t index : indices) {
        output.registrations << std::format("    result += ( gr::insertBlockFactory(registry, {}()) ? 1UZ : 0UZ );\n", registrationSymbol(stem, index));
    }
    output.registrations << "    return result;\n}\n}\n\n";
    output.registrations << std::format("auto {0}_invoked = {0}({1}());\n\n", initFunction, options.registryInstance);
    output.registrations << "#endif // GR_ENABLE_BLOCK_REGISTRY\n";
    output.registrations << std::format("// To initialize, call {}\n", initFunction);
    output.registrations << "// end of auto-generated code\n";

    const std::string declarationsGuard = std::format("HEADER_GUARD_{}_HPP", initFunction);
    output.declarations << std::format("#ifndef {}\n#define {}\n", declarationsGuard, declarationsGuard);
    output.declarations << "extern \"C\" { std::size_t " << initFunction << "(gr::BlockRegistry&); }\n";
    output.declarations << std::format("#endif // {}\n", declarationsGuard);
    output.rawCalls << "result += " << initFunction << "(registry);\n";

    return initFunction;
}

static std::expected<RegisterBlock, std::string> parseRegisterBlockMacro(std::string_view line) {
    auto macroPos = line.find(kRegistrationMacroName);
    if (macroPos == std::string_view::npos) {
        return std::unexpected("no macro found on this line.");
    }

    std::size_t open = line.find('(', macroPos + kRegistrationMacroName.size());
    if (open == std::string::npos) {
        return std::unexpected("missing '(' after macro");
    }
    std::size_t close = line.rfind(')');
    if ((close == std::string::npos) || (close <= open)) {
        return std::unexpected("missing ')' after macro");
    }

    // content of GR_REGISTER_BLOCK(...)
    std::string_view macroContent = detail::trim(line.substr(open + 1, close - open - 1));
    auto             exParts      = detail::splitTopLevelCommaSeparatedValues(macroContent);
    if (!exParts.has_value()) {
        return std::unexpected("erroneous or empty macro body?");
    }
    auto parts = exParts.value();

    RegisterBlock rb;
    std::size_t   idx = 0;

    // if first part is quoted => user-defined baseName
    if ((parts[idx].size() >= 2) && (parts[idx].front() == '"') && (parts[idx].back() == '"')) { // remove outer quotes
        rb.baseName = {parts[idx].data() + 1, parts[idx].size() - 2};
        idx++;
    }

    // parse template base name
    if (idx >= parts.size()) {
        return std::unexpected("missing templateName argument");
    }
    rb.templateName = detail::trim(parts[idx]);
    idx++;

    // parse template type/NTTP parameter list
    if (idx < parts.size()) {
        rb.paramPack = detail::trim(parts[idx]);
        idx++;
    }

    // remainder: parse specific template types [T] that need to be instantiated, e.g. "[ int, float, double, ], [ int, short], ..."
    for (; idx < parts.size(); idx++) {
        auto chunk = detail::trim(parts[idx]);
        if ((chunk.size() >= 2) && (chunk.front() == '[') && (chunk.back() == ']')) {
            chunk = detail::trim(chunk.substr(1, chunk.size() - 2));
        }
        auto exSub = detail::splitTopLevelCommaSeparatedValues(chunk);
        if (!exSub.has_value()) {
            return std::unexpected(std::format("couldn't parse '{}' error: {}", chunk, exSub.error()));
        }
        std::vector<std::string_view> sub = exSub.value();
        std::vector<std::string_view> group;
        group.reserve(sub.size());
        for (const auto& sv : sub) {
            auto val = detail::trim(sv);
            if (!val.empty()) {
                group.emplace_back(val);
            }
        }
        if (!group.empty()) {
            rb.expansions.push_back(std::move(group));
        }
    }

    return rb;
}

// cartesianProduct => expansions => combos
static std::vector<std::vector<std::string_view>> cartesianProduct(const std::vector<std::vector<std::string_view>>& groups) {
    if (groups.empty()) {
        return {{}};
    }
    std::vector<std::vector<std::string_view>> result{{}};
    for (auto& g : groups) {
        std::vector<std::vector<std::string_view>> temp;
        for (const auto& prefix : result) {
            for (auto& val : g) {
                auto row = prefix;
                row.push_back(val);
                temp.push_back(std::move(row));
            }
        }
        result = std::move(temp);
    }
    return result;
}

// replacePlaceholders => paramPack="([T],[U])" => expansions => "float,int"
static std::string replacePlaceholders(std::string param, const std::vector<std::string_view>& vars) {
    // remove leading '(' or ' ' and trailing ')' or ' '
    while (!param.empty() && ((param.front() == '(') || (param.front() == ' '))) {
        param.erase(param.begin());
    }
    while (!param.empty() && ((param.back() == ')') || (param.back() == ' '))) {
        param.pop_back();
    }

    constexpr std::array placeholders = {"[T]", "[U]", "[A]", "[B]", "[X]", "[Y]", "[Z]", "[S]"};
    for (int i = 0; (i < placeholders.size()) && (i < static_cast<int>(vars.size())); i++) {
        auto&       val = vars[i];
        auto        ph  = placeholders[i];
        std::size_t pos = 0;
        while ((pos = param.find(ph, pos)) != std::string::npos) {
            param.replace(pos, std::strlen(ph), val);
            pos += val.size();
        }
    }
    return param;
}

static InstantiationNames makeInstantiationNames(const RegisterBlock& info, const std::vector<std::string_view>& vars) {
    const auto  replaced = replacePlaceholders(std::string(info.paramPack), vars);
    std::string finalName;
    if (!info.baseName.empty() && !replaced.empty()) {
        finalName = std::format("{}<{}>", info.baseName, replaced);
    } else {
        finalName = std::string(info.baseName);
    }
    std::string templateName = std::format("{}{}", info.templateName, replaced.empty() ? "" : std::format("<{}>", replaced));
    return {std::move(templateName), std::move(finalName)};
}

int main(int argc, char** argv) try {
    std::filesystem::path commandPath = argv[0];
    if (argc < 3) {
        std::cerr << std::format("Usage: {} <header.hpp> <outputDir> [--split | -s] [--max-per-tu <N>] [--registry-header <include>] [--registry-instance <expression>]\n", commandPath.string());
        return 1;
    }

    Options options(argc, argv);

    if (options.expansionsSplit && (options.maxPerTu > 0UZ)) {
        std::cerr << "error: --split and --max-per-tu are mutually exclusive.\n";
        return 1;
    }

    std::cout << std::format("parsing header: '{}' -> '{}'  split: {}  max-per-tu: {} \n", options.headerPath.string(), options.outDir.string(), options.expansionsSplit ? "Yes" : "No", options.maxPerTu);

    if (!std::filesystem::exists(options.headerPath)) {
        std::cerr << std::format("error: file '{}' not found.\n", options.headerPath.string());
        return 1;
    }

    std::ifstream fin(options.headerPath);
    if (!fin.is_open()) {
        throw std::format("cannot open '{}' for writing", options.headerPath.string());
    }

    std::filesystem::create_directories(options.outDir);

    auto stem       = options.headerPath.stem().string();
    int  macroCount = 0UZ;
    int  fileCount  = 0UZ;

    const auto moduleName = options.outDir.filename().string();

    const auto integratorSourceFile = (options.outDir / "integrator.cpp");
    if (!std::filesystem::exists(integratorSourceFile)) {
        std::ofstream integrator = openFile(integratorSourceFile.string());
        integrator << std::format(R"cppcode(
            #include <gnuradio-4.0/BlockRegistry.hpp>

            #include "declarations.hpp"

            extern "C" {{
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_{}(gr::BlockRegistry& registry) {{
                    std::size_t result = 0UZ;
                    #include "raw_calls.hpp"
                    return result;
                }}
            }}
        )cppcode",
            moduleName);
    }

    const auto integratorHeaderFile = (options.outDir / (moduleName + ".hpp"));
    if (!std::filesystem::exists(integratorHeaderFile)) {
        std::ofstream integrator = openFile(integratorHeaderFile.string());
        integrator << std::format(R"cppcode(
            #ifndef GR_BLOCKLIB_INIT_MODULE_{0}
            #define GR_BLOCKLIB_INIT_MODULE_{0}
            namespace gr {{ class BlockRegistry; }}

            extern "C" {{
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_{0}(gr::BlockRegistry& registry);
            }}

            namespace gr::blocklib {{
                inline
                std::size_t init{0}(gr::BlockRegistry& registry) {{
                    return gr_blocklib_init_module_{0}(registry);
                }}
            }}
            #endif
        )cppcode",
            moduleName);
    }

    std::string                      line;
    std::size_t                      lineNum = 0UZ;
    std::vector<PendingRegistration> pendingRegistrations; // every registration of this header, in source order
    while (std::getline(fin, line)) {
        lineNum++;
        auto trimmed = detail::trim(line);
        if (trimmed.empty() || trimmed.starts_with("//") || !trimmed.contains(kRegistrationMacroName)) {
            continue;
        }

        std::cout << std::format("\tfound macro on line {}: '{}'\n", lineNum, trimmed);

        auto maybe = parseRegisterBlockMacro(trimmed);
        if (!maybe) {
            std::cerr << std::format("\terror: parse failure in {}:{} => {}\n", options.headerPath.filename().string(), lineNum, maybe.error());
            continue;
        }
        auto& info = *maybe;

        for (const auto& vars : cartesianProduct(info.expansions)) {
            auto [templateName, finalName] = makeInstantiationNames(info, vars);
            pendingRegistrations.push_back({std::move(templateName), std::move(finalName), lineNum, static_cast<std::size_t>(macroCount)});
        }

        macroCount++;
        std::cout << std::endl;
    }

    for (std::size_t index = 0UZ; index < pendingRegistrations.size(); index++) {
        emitDefinitionUnit(commandPath.string(), stem, index, pendingRegistrations[index], options);
        fileCount++;
    }

    // group the registrations into declaration-only units: balanced chunks when maxPerTu is set,
    // one unit per registration when expansionsSplit is set, one unit per macro line otherwise
    std::vector<std::pair<std::string, std::vector<std::size_t>>> registrationUnits;
    if (options.maxPerTu > 0UZ && !pendingRegistrations.empty()) {
        // sizes differ by at most one, none exceeding maxPerTu
        const std::size_t total   = pendingRegistrations.size();
        const std::size_t nChunks = (total + options.maxPerTu - 1UZ) / options.maxPerTu;
        std::size_t       next    = 0UZ;
        for (std::size_t chunk = 0UZ; chunk < nChunks; chunk++) {
            const std::size_t count = (total / nChunks) + ((chunk < (total % nChunks)) ? 1UZ : 0UZ);

            std::vector<std::size_t> indices(count);
            std::ranges::iota(indices, next);
            next += count;
            registrationUnits.emplace_back(std::format("{}_{}", stem, chunk), std::move(indices));
        }
    } else {
        std::size_t localIdx = 0UZ;
        for (std::size_t index = 0UZ; index < pendingRegistrations.size(); index++) {
            const std::size_t macroIndex = pendingRegistrations[index].macroIndex;
            localIdx                     = (index > 0UZ && pendingRegistrations[index - 1UZ].macroIndex == macroIndex) ? localIdx + 1UZ : 0UZ;

            if (options.expansionsSplit) {
                registrationUnits.emplace_back(std::format("{}_{}_{}", stem, macroIndex, localIdx), std::vector{index});
            } else if (localIdx == 0UZ) {
                registrationUnits.emplace_back(std::format("{}_{}", stem, macroIndex), std::vector{index});
            } else {
                registrationUnits.back().second.push_back(index);
            }
        }
    }

    for (const auto& [namePrefix, indices] : registrationUnits) {
        const std::string initFunction = emitRegistrationUnit(commandPath.string(), stem, namePrefix, indices, pendingRegistrations, options);
        std::cout << std::format("\t=> To initialize, call {}\n", initFunction);
        fileCount++;
    }

    std::cout << std::format("parse_registrations: Wrote {} file(s) for {} macro definition(s).\n", fileCount, macroCount);
    return 0;
} catch (const std::string& error) {
    std::cerr << "ERROR: " << error << '\n';
    return EXIT_FAILURE;
}
