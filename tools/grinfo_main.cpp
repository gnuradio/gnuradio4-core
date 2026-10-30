// grinfo - report the framework, the directories it searches for blocks, and the blocks and schedulers they bring.
//
// No block name is built in: the registry linked into this program, the libraries the plugin loader opens and a
// default-constructed instance of each registered key are the only sources, so a framework that gains a block
// reports it here without this program being rebuilt. The one exception is core's `Simple` scheduler, which this
// program registers itself under one key per execution policy.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // dladdr, which names the file a block's type information was loaded from
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/DeviceListing.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/config.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>
#include <gnuradio-4.0/meta/formatter.hpp>

#include "BlockLookup.hpp"
#include "DocWriter.hpp"
#include "ShippedSchedulers.hpp"

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace {

using gr::tools::Directory;
using gr::tools::searchDirectories;

constexpr std::string_view kProgram = "grinfo";

// Every line of the report fits a terminal this wide. A value that does not is cut with an ellipsis. The JSON, Markdown
// and HTML documents keep it whole.
constexpr std::size_t kWidth = 80UZ;

// The settings gr::Block declares on behalf of every block. They carry the same name, type and default everywhere,
// so the report leaves them out of a block's own table unless they are asked for.
constexpr std::array<std::string_view, 8> kFrameworkSettings{"compute_domain", "disconnect_on_done", "input_chunk_size", "name", "output_chunk_size", "stride", "ui_constraints", "unique_name"};

constexpr std::string_view kUsage = R"(grinfo - report the GNU Radio 4 framework, its plugins, blocks and schedulers

Usage: grinfo [command] [options]

  version            the framework, the directories searched and what each held
  blocks             every registered block, by library and by family
  block <name>       one block in detail: its types, its ports and its settings
  schedulers         every registered scheduler, by library, with its settings
  devices            the attached units every loaded device family lists

  --plugin-dir <dir> a directory of plugins and block libraries; repeatable
  --json             the same content as a pretty-printed JSON document
  --markdown         the same content as a CommonMark document
  --html             the same content as one HTML page that loads nothing else
  --verbose          with blocks, every block in detail rather than its name
  --all-settings     also the settings the framework declares on every block
  --allow-open       with devices, also the families whose listing opens a unit
  --help, -h         this text

Blocks and schedulers come from the directories named by --plugin-dir, from the
colon-separated list in GNURADIO4_PLUGIN_DIRECTORIES, and from the plugin
directory of this installation, which is always searched. A directory named
twice is searched once.

<name> is a registry key with its template parameters,
gr::blocks::basic::Convert<int16, float32>, or a name without them,
gr::blocks::basic::Convert or Convert, which selects every instantiation of it.

grinfo registers the schedulers rungraph registers, core's Simple scheduler
under one key per execution policy, so schedulers lists every key rungraph
--scheduler takes from the same directories.

A block is reported once however many instantiations it has: their type
parameters are listed together, and a port or a setting is written in terms of
those parameters wherever that is what tells the instantiations apart.

What a block or a scheduler reports is read from an instance the registry
creates with no parameters. The file a key came from is the file the dynamic
linker holds that instance's type information in, so a block linked into this
program is told apart from one a library brought.

--json, --markdown and --html each select one form of the report, and a
command line that selects two is refused.

The JSON document carries "schema": 2 and one shape per command. A field the
framework holds nothing in is left out rather than written as null; an array is
always present, and empty where the block declares none of it.

Exit status is 0, 1 when a block named to block is not registered or a directory
named by --plugin-dir cannot be searched, and 2 when the command line cannot be
used.
)";

enum class Command : std::uint8_t { Version, Blocks, Block, Schedulers, Devices };

enum class Form : std::uint8_t { Text, Json, Markdown, Html };

// the form an option selects, or nothing for an option that selects none
[[nodiscard]] std::optional<Form> formOf(std::string_view option) {
    if (option == "--json") {
        return Form::Json;
    }
    if (option == "--markdown") {
        return Form::Markdown;
    }
    if (option == "--html") {
        return Form::Html;
    }
    return std::nullopt;
}

struct Options {
    Command                  command = Command::Version;
    std::string              blockName;
    std::vector<std::string> pluginDirectories;
    Form                     form = Form::Text;
    std::string              formOption; // the option that selected `form`, for the line that refuses a second one
    bool                     verbose     = false;
    bool                     allSettings = false;
    bool                     allowOpen   = false; // whether devices asks the families whose listing opens a unit
    bool                     help        = false;
};

// the command line, or nothing when it cannot be used; every refusal is reported as it is found
[[nodiscard]] std::optional<Options> parse(std::span<const std::string_view> arguments) {
    Options options;
    bool    commandSeen = false;
    for (std::size_t index = 0UZ; index < arguments.size();) {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            ++index;
            continue;
        }
        if (const std::optional<Form> form = formOf(argument); form.has_value()) {
            if (options.form != Form::Text && options.form != *form) {
                std::println(stderr, "{}: {} and {} cannot be combined", kProgram, options.formOption, argument);
                return std::nullopt;
            }
            options.form       = *form;
            options.formOption = argument;
            ++index;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            ++index;
            continue;
        }
        if (argument == "--all-settings") {
            options.allSettings = true;
            ++index;
            continue;
        }
        if (argument == "--allow-open") {
            options.allowOpen = true;
            ++index;
            continue;
        }
        if (argument == "--plugin-dir") {
            if (index + 1UZ >= arguments.size()) {
                std::println(stderr, "{}: {} needs a value", kProgram, argument);
                return std::nullopt;
            }
            options.pluginDirectories.emplace_back(arguments[index + 1UZ]);
            index += 2UZ;
            continue;
        }
        if (argument.starts_with("-")) {
            std::println(stderr, "{}: unknown option '{}'", kProgram, argument);
            return std::nullopt;
        }
        if (!commandSeen) {
            if (argument == "version") {
                options.command = Command::Version;
            } else if (argument == "blocks") {
                options.command = Command::Blocks;
            } else if (argument == "block") {
                options.command = Command::Block;
            } else if (argument == "schedulers") {
                options.command = Command::Schedulers;
            } else if (argument == "devices") {
                options.command = Command::Devices;
            } else {
                std::println(stderr, "{}: unknown command '{}'", kProgram, argument);
                return std::nullopt;
            }
            commandSeen = true;
            ++index;
            continue;
        }
        if (options.command == Command::Block && options.blockName.empty()) {
            options.blockName.assign(argument);
            ++index;
            continue;
        }
        std::println(stderr, "{}: unexpected argument '{}'", kProgram, argument);
        return std::nullopt;
    }
    if (!options.help && options.command == Command::Block && options.blockName.empty()) {
        std::println(stderr, "{}: block needs the name of a block", kProgram);
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] std::string canonicalPath(std::string_view path) {
    std::error_code   failed;
    const std::string resolved = std::filesystem::weakly_canonical(path, failed).string();
    return failed ? std::string(path) : resolved;
}

// `text` cut to `width` with an ellipsis: a path keeps its tail, which is what tells one file from another, and
// anything else keeps its head
[[nodiscard]] std::string fit(std::string_view text, std::size_t width) {
    if (text.size() <= width) {
        return std::string(text);
    }
    if (width <= 3UZ) {
        return std::string(text.substr(0UZ, width));
    }
    if (text.contains('/')) {
        return std::format("...{}", text.substr(text.size() - (width - 3UZ)));
    }
    return std::format("{}...", text.substr(0UZ, width - 3UZ));
}

// where a line may be broken: anywhere a space stands, or only where one stands outside angle brackets, which is
// what keeps `<int16, float32>` on one line and off two
enum class Break : std::uint8_t { AtSpaces, BetweenTypes };

// `text` broken into lines of at most `width`; a word that does not fit a line of its own is cut
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, std::size_t width, Break where = Break::AtSpaces) {
    const std::size_t        room = std::max(width, 8UZ);
    std::vector<std::string> lines;
    std::string              line;
    std::size_t              depth = 0UZ;
    std::size_t              start = 0UZ;
    for (std::size_t i = 0UZ; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (where == Break::BetweenTypes && (text[i] == '<' || text[i] == '>')) {
                depth += text[i] == '<' ? 1UZ : 0UZ;
                depth -= text[i] == '>' && depth > 0UZ ? 1UZ : 0UZ;
                continue;
            }
            if (text[i] != ' ' || depth != 0UZ) {
                continue;
            }
        }
        const std::string_view word = text.substr(start, i - start);
        start                       = i + 1UZ;
        if (word.empty()) {
            continue;
        }
        if (line.empty()) {
            line = fit(word, room);
        } else if (line.size() + 1UZ + word.size() <= room) {
            line += ' ';
            line += word;
        } else {
            lines.push_back(std::move(line));
            line = fit(word, room);
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

void printWrapped(std::string_view indent, std::string_view continuation, std::string_view text) {
    const std::vector<std::string> lines = wrap(text, kWidth - std::max(indent.size(), continuation.size()));
    for (std::size_t i = 0UZ; i < lines.size(); ++i) {
        std::println("{}{}", i == 0UZ ? indent : continuation, lines[i]);
    }
}

enum class Align : std::uint8_t { Left, Right };

struct Column {
    std::string_view header;
    Align            align = Align::Left;
    // whether a column that reads the same on every row is worth taking out of the table: the column a row is
    // looked up by never is, however often it repeats
    bool collapsible = false;
};

// Rows printed as columns two spaces apart within the report's width: a column no wider than its widest cell until
// the line would run over, and then the widest column gives way and its cells are cut. A row whose last cells are
// empty ends where its last filled cell does. A set of headers that are all empty prints no header line, which is
// what a listing that names its columns in the line above it needs.
void printTable(std::string_view indent, std::span<const Column> columns, std::span<const std::vector<std::string>> rows) {
    if (columns.empty()) {
        return;
    }
    std::vector<std::size_t> widths(columns.size(), 0UZ);
    for (std::size_t i = 0UZ; i < columns.size(); ++i) {
        widths[i] = columns[i].header.size();
    }
    for (const std::vector<std::string>& row : rows) {
        for (std::size_t i = 0UZ; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }
    auto total = [indent, &widths, &columns] {
        std::size_t width = indent.size() + 2UZ * (columns.size() - 1UZ);
        for (const std::size_t column : widths) {
            width += column;
        }
        return width;
    };
    while (total() > kWidth) {
        const auto widest = std::ranges::max_element(widths);
        if (*widest <= 4UZ) {
            break;
        }
        --*widest;
    }

    auto line = [indent, &widths, &columns](const auto& cells) {
        std::string text(indent);
        for (std::size_t i = 0UZ; i < cells.size() && i < widths.size(); ++i) {
            const std::string cell    = fit(std::string_view(cells[i]), widths[i]);
            const std::size_t padding = widths[i] - cell.size();
            if (columns[i].align == Align::Right) {
                text.append(padding, ' ');
            }
            text += cell;
            if (i + 1UZ < cells.size()) {
                text.append(columns[i].align == Align::Right ? 2UZ : padding + 2UZ, ' ');
            }
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        std::println("{}", text);
    };

    if (std::ranges::any_of(columns, [](const Column& column) { return !column.header.empty(); })) {
        std::vector<std::string> headers;
        headers.reserve(columns.size());
        for (const Column& column : columns) {
            headers.emplace_back(column.header);
        }
        line(headers);
    }
    for (const std::vector<std::string>& row : rows) {
        line(row);
    }
}

// one `name  value` line per fact, the names padded to the widest and a value too long for what is left wrapped at
// its spaces under the first line, or cut where it has none
void printFacts(std::string_view indent, const std::vector<std::pair<std::string, std::string>>& facts, Break where = Break::AtSpaces) {
    std::size_t width = 0UZ;
    for (const auto& [name, value] : facts) {
        width = std::max(width, name.size());
    }
    const std::size_t taken = indent.size() + width + 2UZ;
    const std::size_t room  = taken + 8UZ < kWidth ? kWidth - taken : 8UZ;
    for (const auto& [name, value] : facts) {
        const std::vector<std::string> lines = wrap(value, room, where);
        for (std::size_t i = 0UZ; i < lines.size(); ++i) {
            std::println("{}{}{}  {}", indent, i == 0UZ ? name : std::string{}, std::string(width - (i == 0UZ ? name.size() : 0UZ), ' '), lines[i]);
        }
        if (lines.empty()) {
            std::println("{}{}", indent, name);
        }
    }
}

// A block's own text as paragraphs: the source lines of a paragraph joined, and the `@brief` marker of the first
// paragraph dropped because the paragraph's place in the report already says what it is.
[[nodiscard]] std::vector<std::string> paragraphsOf(std::string_view description) {
    std::vector<std::string> paragraphs;
    std::string              paragraph;
    for (std::size_t start = 0UZ; start <= description.size();) {
        const std::size_t      end  = std::min(description.find('\n', start), description.size());
        const std::string_view line = description.substr(start, end - start);
        start                       = end + 1UZ;
        if (line.empty()) {
            if (!paragraph.empty()) {
                paragraphs.push_back(std::move(paragraph));
                paragraph.clear();
            }
            continue;
        }
        paragraph += paragraph.empty() ? "" : " ";
        paragraph += line;
    }
    if (!paragraph.empty()) {
        paragraphs.push_back(std::move(paragraph));
    }
    if (paragraphs.empty()) {
        return paragraphs;
    }
    std::string& first = paragraphs.front();
    if (first.starts_with("@brief ")) {
        first.erase(0UZ, 7UZ);
    } else if (first.starts_with("(@brief ")) {
        first.erase(0UZ, 8UZ);
        if (first.ends_with(')')) {
            first.pop_back();
        }
    }
    return paragraphs;
}

// a block's own text under `indent`, each paragraph wrapped again to the report's width
void printDescription(std::string_view description, std::string_view indent, bool firstParagraphOnly) {
    for (const std::string& text : paragraphsOf(description)) {
        std::print("\n");
        printWrapped(indent, indent, text);
        if (firstParagraphOnly) {
            return;
        }
    }
}

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
const int kAddressInThisProgram = 0;

// the file the dynamic linker holds `address` in, or nothing when it names none
[[nodiscard]] std::string fileHolding(const void* address) {
#if defined(_WIN32)
    // resolve the module owning the address, then name its file. UNCHANGED_REFCOUNT leaves the
    // module's reference count untouched.
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) == 0) {
        return {};
    }
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))) == 0) {
        return {};
    }
    return canonicalPath(path);
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    return canonicalPath(info.dli_fname);
#endif
}

// The file a block's type information was loaded from: the plugin or the block library that registered the key, and
// this program itself for a key linked into it. The loader knows which files it opened but not which keys each of
// them brought, and the linker knows exactly that for the instance in hand.
[[nodiscard]] std::string fileOfType(const gr::BlockModel& block) { return fileHolding(static_cast<const void*>(std::addressof(typeid(block)))); }

[[nodiscard]] std::string thisProgramFile() { return fileHolding(static_cast<const void*>(std::addressof(kAddressInThisProgram))); }
#else
[[nodiscard]] std::string fileOfType(const gr::BlockModel&) { return {}; }

[[nodiscard]] std::string thisProgramFile() { return {}; }
#endif

struct KeyParts {
    std::string family;     // the namespace the block is declared in, empty for one declared in none
    std::string name;       // the block's own name, without its namespace and without its parameters
    std::string parameters; // the text between the angle brackets, empty for a key that has none
};

[[nodiscard]] KeyParts partsOf(std::string_view key) {
    KeyParts         parts;
    std::string_view head = key;
    if (const std::size_t open = key.find('<'); open != std::string_view::npos && key.ends_with('>')) {
        head             = key.substr(0UZ, open);
        parts.parameters = std::string(key.substr(open + 1UZ, key.size() - open - 2UZ));
    }
    if (const std::size_t separator = head.rfind("::"); separator != std::string_view::npos) {
        parts.family = std::string(head.substr(0UZ, separator));
        parts.name   = std::string(head.substr(separator + 2UZ));
    } else {
        parts.name = std::string(head);
    }
    return parts;
}

// the parameters of a key, one entry per top-level comma: `int16, float32` is two and `complex<float32>` is one
[[nodiscard]] std::vector<std::string> splitParameters(std::string_view text) {
    std::vector<std::string> parameters;
    std::size_t              depth = 0UZ;
    std::size_t              start = 0UZ;
    for (std::size_t i = 0UZ; i <= text.size(); ++i) {
        if (i == text.size() || (text[i] == ',' && depth == 0UZ)) {
            std::string_view piece = text.substr(start, i - start);
            while (!piece.empty() && piece.front() == ' ') {
                piece.remove_prefix(1UZ);
            }
            while (!piece.empty() && piece.back() == ' ') {
                piece.remove_suffix(1UZ);
            }
            if (!piece.empty()) {
                parameters.emplace_back(piece);
            }
            start = i + 1UZ;
        } else if (text[i] == '<' || text[i] == '(') {
            ++depth;
        } else if ((text[i] == '>' || text[i] == ')') && depth > 0UZ) {
            --depth;
        }
    }
    return parameters;
}

struct Port {
    std::string direction;
    std::string name;
    std::string dataType;
    std::string portType;
    std::string domain;
    bool        optional    = false;
    bool        synchronous = false;
    bool        collection  = false;
    std::size_t minSamples  = 0UZ;
    std::size_t maxSamples  = 0UZ;
};

struct Setting {
    std::string                   name;
    std::string                   type;
    std::optional<gr::pmt::Value> defaultValue; // nothing where the block stores no default under the name
    std::string                   unit;
    std::string                   description;
    std::string                   documentation;
    std::string                   enumValues;
    std::string                   tagKey; // the tag key the instance couples the setting to, empty for none
    bool                          writable = false;
    bool                          visible  = true;
};

enum class KeyOrigin : std::uint8_t { ThisProgram, BlockLibrary, Plugin, LinkedLibrary, NotAttributed };

[[nodiscard]] std::string_view nameOf(KeyOrigin origin) {
    switch (origin) {
    case KeyOrigin::ThisProgram: return "this-program";
    case KeyOrigin::BlockLibrary: return "block-library";
    case KeyOrigin::Plugin: return "plugin";
    case KeyOrigin::LinkedLibrary: return "linked-library";
    case KeyOrigin::NotAttributed: return "not-attributed";
    }
    std::unreachable();
}

enum class Registry : std::uint8_t { Blocks, Schedulers };

// one registered key, read from an instance of it
struct Instantiation {
    Registry                 registry = Registry::Blocks; // a scheduler has no ports and no block category
    std::string              key;
    std::string              family;
    std::string              name;
    std::vector<std::string> parameters;
    std::string              file; // the library the type came from, empty where the linker named none
    KeyOrigin                origin = KeyOrigin::NotAttributed;
    std::string              typeName;
    std::string              blockCategory;
    std::string              uiCategory;
    std::string              error; // why no instance could be made; nothing below is filled then
    bool                     detailed = false;
    std::string              description;
    std::string              settingsError;
    std::vector<Port>        ports;
    std::vector<Setting>     settings;
};

enum class FileKind : std::uint8_t { BlockLibrary, NotLoaded, NotOpened };

[[nodiscard]] std::string_view nameOf(FileKind kind) {
    switch (kind) {
    case FileKind::BlockLibrary: return "block-library";
    case FileKind::NotLoaded: return "not-loaded";
    case FileKind::NotOpened: return "not-opened";
    }
    std::unreachable();
}

[[nodiscard]] std::string_view nameOf(gr::DataCacheDirectory::Origin origin) {
    using enum gr::DataCacheDirectory::Origin;
    switch (origin) {
    case Environment: return "environment";
    case Compiled: return "compiled";
    case User: return "user";
    case None: return "none";
    }
    std::unreachable();
}

// one file a plugin directory held
struct FileFact {
    std::string file;
    FileKind    kind                   = FileKind::BlockLibrary;
    std::size_t blockRegistrations     = 0UZ;
    std::size_t schedulerRegistrations = 0UZ;
    std::string reason;
};

struct PluginFact {
    std::string name;
    std::string author;
    std::string license;
    std::string version;
    std::size_t blocks     = 0UZ;
    std::size_t schedulers = 0UZ;
};

struct Totals {
    std::size_t blockKeys      = 0UZ;
    std::size_t blockFamilies  = 0UZ;
    std::size_t schedulerKeys  = 0UZ;
    std::size_t blockLibraries = 0UZ;
    std::size_t plugins        = 0UZ;
    std::size_t filesNotLoaded = 0UZ;
    std::size_t filesNotOpened = 0UZ;
};

// what one run of the program has to report, read once
struct Context {
    gr::PluginLoader&        loader;
    std::vector<Directory>   directories;
    std::vector<std::string> keys;
    std::vector<std::string> schedulers;
    std::vector<FileFact>    files;
    std::vector<PluginFact>  plugins;
    std::set<std::string>    libraryFiles;
    std::string              program;
    Totals                   totals;
};

[[nodiscard]] std::string metaString(const gr::property_map& meta, const std::string& key) {
    const auto entry = meta.find(key);
    if (entry == meta.cend()) {
        return {};
    }
    const std::string_view text = entry->second.value_or(std::string_view{});
    return text.data() == nullptr ? gr::pmt::to_string(entry->second) : std::string(text);
}

void collectPorts(gr::BlockModel::DynamicPorts& ports, std::string_view direction, std::vector<Port>& into) {
    auto one = [direction, &into](std::string name, gr::DynamicPort& port, bool collection) {
        into.push_back({
            .direction   = std::string(direction),
            .name        = std::move(name),
            .dataType    = std::string(port.metaInfo.data_type),
            .portType    = std::format("{}", gr::port::decodePortType(port.portMaskInfo())),
            .domain      = std::string(port.domain()),
            .optional    = port.isOptional(),
            .synchronous = port.isSynchronous(),
            .collection  = collection,
            .minSamples  = port.min_samples,
            .maxSamples  = port.max_samples,
        });
    };
    for (std::size_t i = 0UZ; i < ports.size(); ++i) {
        if (auto* collection = std::get_if<gr::BlockModel::NamedPortCollection>(&ports[i]); collection != nullptr) {
            for (std::size_t j = 0UZ; j < collection->ports.size(); ++j) {
                one(std::format("{}#{}", collection->name, j), collection->ports[j], true);
            }
            if (collection->ports.empty()) {
                Port empty;
                empty.direction  = std::string(direction);
                empty.name       = std::format("{} (empty collection)", collection->name);
                empty.collection = true;
                into.push_back(std::move(empty));
            }
        } else if (auto* port = std::get_if<gr::DynamicPort>(&ports[i]); port != nullptr) {
            one(std::string(port->metaInfo.name), *port, false);
        }
    }
}

void collectSettings(const gr::BlockModel& block, std::vector<Setting>& into) {
    const gr::SettingsBase&      settings  = block.settings();
    const gr::property_map&      meta      = block.metaInformation();
    const gr::property_map       defaults  = settings.defaultParameters();
    const std::set<std::string>& writable  = settings.writableMembers();
    const gr::property_map       couplings = settings.tagCouplings();

    std::set<std::string> names(writable.begin(), writable.end());
    for (const auto& [name, _] : defaults) {
        names.emplace(std::string_view(name));
    }
    for (const std::string& name : names) {
        Setting setting;
        setting.name      = name;
        const auto stored = defaults.find(std::string_view(name));
        if (stored != defaults.cend()) {
            setting.defaultValue = stored->second;
            setting.type         = gr::pmt::detail::type_name(stored->second);
        }
        setting.unit          = metaString(meta, name + "::unit");
        setting.description   = metaString(meta, name + "::description");
        setting.documentation = metaString(meta, name + "::documentation");
        setting.enumValues    = metaString(meta, name + "::enum_values");
        setting.writable      = writable.contains(name);
        setting.tagKey        = metaString(couplings, name);
        if (const auto visible = meta.find(name + "::visible"); visible != meta.cend()) {
            setting.visible = visible->second.value_or(true);
        }
        into.push_back(std::move(setting));
    }
}

// what a file the linker named is to this program
[[nodiscard]] KeyOrigin originOfFile(const Context& context, const std::string& file) {
    if (file.empty()) {
        return KeyOrigin::NotAttributed;
    }
    if (file == context.program) {
        return KeyOrigin::ThisProgram;
    }
    if (context.libraryFiles.contains(file)) {
        return KeyOrigin::BlockLibrary;
    }
    const std::string parent = std::filesystem::path(file).parent_path().string();
    if (std::ranges::any_of(context.directories, [&parent](const Directory& directory) { return canonicalPath(directory.path) == parent; })) {
        return KeyOrigin::Plugin;
    }
    return KeyOrigin::LinkedLibrary;
}

/**
 * @brief Reads one registered key from a default-constructed instance of it.
 *
 * `settings().init()` is the call that copies a block's annotations into its meta information and stores its
 * defaults, and it needs neither a progress counter nor a thread pool, so the block is never started. Anything the
 * instance throws on the way is kept as that key's error and every other key is read as before.
 */
[[nodiscard]] Instantiation readKey(Context& context, const std::string& key, Registry registry, bool detailed) {
    const KeyParts parts = partsOf(key);

    Instantiation fact;
    fact.registry   = registry;
    fact.key        = key;
    fact.family     = parts.family;
    fact.name       = parts.name;
    fact.parameters = splitParameters(parts.parameters);

    std::shared_ptr<gr::BlockModel> instance;
    try {
        instance = registry == Registry::Blocks ? context.loader.instantiate(key, {}) : gr::SchedulerModel::asBlockModelPtr(context.loader.instantiateScheduler(key, {}));
        if (instance == nullptr) {
            fact.error = "nothing of that name could be instantiated";
        }
    } catch (const gr::exception& error) {
        // the message alone: what() appends the source location of the throw, which is a path on the machine that
        // built the library and says nothing to the reader
        fact.error = error.message;
    } catch (const std::exception& error) {
        fact.error = error.what();
    } catch (...) {
        fact.error = "an exception that is not a std::exception";
    }
    if (instance == nullptr) {
        fact.origin = originOfFile(context, fact.file);
        return fact;
    }

    fact.file     = fileOfType(*instance);
    fact.origin   = originOfFile(context, fact.file);
    fact.typeName = std::string(instance->typeName());
    if (registry == Registry::Blocks) {
        fact.blockCategory = std::format("{}", instance->blockCategory());
        fact.uiCategory    = std::format("{}", instance->uiCategory());
    }
    if (!detailed) {
        return fact;
    }

    fact.detailed = true;
    try {
        instance->settings().init();
    } catch (const gr::exception& error) {
        fact.settingsError = error.message;
    } catch (const std::exception& error) {
        fact.settingsError = error.what();
    } catch (...) {
        fact.settingsError = "an exception that is not a std::exception";
    }
    fact.description = metaString(instance->metaInformation(), "description");
    if (registry == Registry::Blocks) {
        collectPorts(instance->dynamicInputPorts(), "input", fact.ports);
        collectPorts(instance->dynamicOutputPorts(), "output", fact.ports);
    }
    collectSettings(*instance, fact.settings);
    return fact;
}

// one block: every key of one name in one family, in the one file that registered them
struct NamedBlock {
    std::string                family;
    std::string                name;
    std::string                file;
    KeyOrigin                  origin = KeyOrigin::NotAttributed;
    std::vector<Instantiation> instantiations;
};

struct Family {
    std::string             name;
    std::vector<NamedBlock> blocks;
};

struct Library {
    std::string         file;
    KeyOrigin           origin = KeyOrigin::NotAttributed;
    std::size_t         keys   = 0UZ;
    std::vector<Family> families;
};

// the keys grouped by the file that registered them, by family within a file and by name within a family
[[nodiscard]] std::vector<Library> group(std::vector<Instantiation> facts) {
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<Instantiation>>>> tree;
    std::map<std::string, KeyOrigin>                                                                origins;
    for (Instantiation& fact : facts) {
        origins[fact.file] = fact.origin;
        tree[fact.file][fact.family][fact.name].push_back(std::move(fact));
    }

    std::vector<Library> libraries;
    for (auto& [file, families] : tree) {
        Library library;
        library.file   = file;
        library.origin = origins[file];
        for (auto& [familyName, blocks] : families) {
            Family family;
            family.name = familyName;
            for (auto& [blockName, instantiations] : blocks) {
                library.keys += instantiations.size();
                family.blocks.push_back({.family = familyName, .name = blockName, .file = file, .origin = library.origin, .instantiations = std::move(instantiations)});
            }
            library.families.push_back(std::move(family));
        }
        libraries.push_back(std::move(library));
    }
    // the program's own blocks first: they are there whatever the directories hold
    std::ranges::sort(libraries, [](const Library& left, const Library& right) {
        const bool leftIsProgram  = left.origin == KeyOrigin::ThisProgram;
        const bool rightIsProgram = right.origin == KeyOrigin::ThisProgram;
        return leftIsProgram != rightIsProgram ? leftIsProgram : left.file < right.file;
    });
    return libraries;
}

// The type parameters of a block's instantiations, and the name each position is written under where every
// instantiation has the same number of them: `T` for a block of one parameter and `T1`, `T2` for one of several.
struct TypeParameters {
    std::size_t count = 0UZ;

    [[nodiscard]] std::string placeholder(std::size_t index) const { return count == 1UZ ? "T" : std::format("T{}", index + 1UZ); }
};

[[nodiscard]] TypeParameters typeParametersOf(const NamedBlock& block) {
    TypeParameters types;
    types.count = block.instantiations.front().parameters.size();
    for (const Instantiation& fact : block.instantiations) {
        if (fact.parameters.size() != types.count) {
            types.count = 0UZ;
        }
    }
    return types;
}

[[nodiscard]] std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return text;
    }
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// a pattern's `{i}` replaced by the i-th type parameter, and every other `{` left where it is: a default value of
// `{}` is text, not a placeholder
[[nodiscard]] std::string substitute(std::string_view pattern, const std::vector<std::string>& parameters) {
    std::string out;
    for (std::size_t i = 0UZ; i < pattern.size();) {
        const std::size_t close = pattern.find('}', i);
        std::size_t       index = 0UZ;
        if (pattern[i] == '{' && close != std::string_view::npos && close > i + 1UZ) {
            const std::string_view digits = pattern.substr(i + 1UZ, close - i - 1UZ);
            if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc{} && index < parameters.size()) {
                out += parameters[index];
                i = close + 1UZ;
                continue;
            }
        }
        out += pattern[i];
        ++i;
    }
    return out;
}

/**
 * @brief The one text every instantiation writes, with a type parameter put back where it stands.
 *
 * A port's data type is `float32` in one instantiation and `int16` in another because it is the block's first or
 * second type parameter, and saying so once is what lets one entry stand for them all. The candidates are taken
 * from the first instantiation and each is kept only if it reproduces every other one, so a block whose ports
 * happen to agree in one instantiation is not described by an accident of that instantiation. Nothing is returned
 * where the instantiations differ beyond their parameters; the caller reports that as a difference.
 */
[[nodiscard]] std::optional<std::string> generalize(std::span<const std::string> values, std::span<const std::vector<std::string>> parameters, const TypeParameters& types) {
    if (values.empty()) {
        return std::nullopt;
    }
    if (std::ranges::all_of(values, [&values](const std::string& value) { return value == values.front(); })) {
        return values.front();
    }
    if (types.count == 0UZ) {
        return std::nullopt;
    }

    std::vector<std::string> candidates;
    for (std::size_t i = 0UZ; i < types.count; ++i) {
        candidates.push_back(replaceAll(values.front(), parameters.front()[i], std::format("{{{}}}", i)));
    }
    std::vector<std::size_t> byLength(types.count);
    for (std::size_t i = 0UZ; i < types.count; ++i) {
        byLength[i] = i;
    }
    std::ranges::sort(byLength, [&parameters](std::size_t left, std::size_t right) { return parameters.front()[left].size() > parameters.front()[right].size(); });
    std::string everyParameter = values.front();
    for (const std::size_t i : byLength) {
        everyParameter = replaceAll(everyParameter, parameters.front()[i], std::format("{{{}}}", i));
    }
    candidates.push_back(std::move(everyParameter));

    for (const std::string& candidate : candidates) {
        if (candidate == values.front()) {
            continue;
        }
        bool reproducesEveryOne = true;
        for (std::size_t i = 0UZ; i < values.size(); ++i) {
            reproducesEveryOne = reproducesEveryOne && substitute(candidate, parameters[i]) == values[i];
        }
        if (reproducesEveryOne) {
            std::string named = candidate;
            for (std::size_t i = 0UZ; i < types.count; ++i) {
                named = replaceAll(std::move(named), std::format("{{{}}}", i), types.placeholder(i));
            }
            return named;
        }
    }
    return std::nullopt;
}

// one port or one setting as the instantiations that declare it write it
struct Merged {
    std::string                           name;
    std::vector<std::string>              cells; // one value per column, written for every instantiation at once
    std::vector<std::size_t>              declaredBy;
    std::vector<std::vector<std::string>> parameters;
};

// the values a column takes, each once and in the order they were first written: what a reader is told where no
// one text stands for them all
[[nodiscard]] std::string alternatives(std::span<const std::string> values) {
    std::vector<std::string> seen;
    std::string              joined;
    for (const std::string& value : values) {
        if (std::ranges::contains(seen, value)) {
            continue;
        }
        seen.push_back(value);
        joined += joined.empty() ? "" : "|";
        joined += value;
    }
    return joined;
}

// `columns` values of one item across the instantiations that declare it, each written in terms of the type
// parameters where that is what tells them apart
void mergeCells(Merged& merged, const std::vector<std::vector<std::string>>& perInstantiation, const TypeParameters& types) {
    const std::size_t columns = perInstantiation.front().size();
    merged.cells.resize(columns);
    std::vector<std::string> values(perInstantiation.size());
    for (std::size_t column = 0UZ; column < columns; ++column) {
        for (std::size_t i = 0UZ; i < perInstantiation.size(); ++i) {
            values[i] = perInstantiation[i][column];
        }
        merged.cells[column] = generalize(values, merged.parameters, types).value_or(alternatives(values));
    }
}

[[nodiscard]] std::string yesNo(bool value) { return value ? "yes" : "no"; }

[[nodiscard]] std::string sampleBound(std::size_t count) { return count == std::numeric_limits<std::size_t>::max() ? "unbounded" : std::to_string(count); }

[[nodiscard]] std::string defaultText(const Setting& setting) { return setting.defaultValue.has_value() ? gr::pmt::to_string(*setting.defaultValue) : "(not stored)"; }

[[nodiscard]] std::string parameterList(const Instantiation& fact) {
    std::string text;
    for (const std::string& parameter : fact.parameters) {
        text += text.empty() ? "" : ", ";
        text += parameter;
    }
    return text.empty() ? std::string{} : std::format("<{}>", text);
}

// the block as a reader names it: the whole key where it has one instantiation, and the name with a placeholder
// per type parameter where it has several, the parameters themselves listed once under it
[[nodiscard]] std::string headingOf(const NamedBlock& block, const TypeParameters& types, bool qualified) {
    const std::string name = qualified && !block.family.empty() ? std::format("{}::{}", block.family, block.name) : block.name;
    if (block.instantiations.size() == 1UZ) {
        return std::format("{}{}", name, parameterList(block.instantiations.front()));
    }
    if (types.count == 0UZ) {
        return name;
    }
    std::string placeholders;
    for (std::size_t i = 0UZ; i < types.count; ++i) {
        placeholders += placeholders.empty() ? "" : ", ";
        placeholders += types.placeholder(i);
    }
    return std::format("{}<{}>", name, placeholders);
}

// A column whose value is the same on every row of a table says nothing where it stands and something where the
// table is named, so it is taken out of the table and put in the line above it as `header value`. One row repeats
// nothing, so nothing is taken out of a table of one.
void splitConstantColumns(std::span<const Column> columns, std::vector<std::vector<std::string>>& rows, std::vector<Column>& kept, std::string& constant) {
    std::vector<bool> keep(columns.size(), true);
    for (std::size_t i = 0UZ; i < columns.size() && rows.size() > 1UZ; ++i) {
        const bool same = std::ranges::all_of(rows, [i, &rows](const std::vector<std::string>& row) { return row[i] == rows.front()[i]; });
        if (columns[i].collapsible && same && !rows.front()[i].empty()) {
            keep[i] = false;
            constant += constant.empty() ? "" : ", ";
            constant += std::format("{} {}", columns[i].header, rows.front()[i]);
        }
    }
    for (std::size_t i = 0UZ; i < columns.size(); ++i) {
        if (keep[i]) {
            kept.push_back(columns[i]);
        }
    }
    for (std::vector<std::string>& row : rows) {
        std::vector<std::string> narrowed;
        for (std::size_t i = 0UZ; i < columns.size(); ++i) {
            if (keep[i]) {
                narrowed.push_back(std::move(row[i]));
            }
        }
        row = std::move(narrowed);
    }
}

// The rows of one table of the report and the columns they stand under, with the columns every row agrees on taken
// out and written as `header value` pairs in `constant`
struct Table {
    std::vector<Column>                   columns;
    std::vector<std::vector<std::string>> rows;
    std::string                           constant;
};

[[nodiscard]] Table tableOf(std::span<const Column> columns, std::vector<std::vector<std::string>> rows) {
    Table table;
    table.rows = std::move(rows);
    splitConstantColumns(columns, table.rows, table.columns, table.constant);
    return table;
}

// the line that names a table, followed by the values its rows share
[[nodiscard]] std::string sectionLine(std::string_view name, std::string_view constant) { return constant.empty() ? std::string(name) : std::format("{} ({})", name, constant); }

void printSection(std::string_view indent, std::string_view name, std::string_view constant) { printWrapped(indent, std::format("{}  ", indent), sectionLine(name, constant)); }

constexpr std::array<Column, 9> kPortColumns{Column{.header = "port"}, Column{.header = "direction"}, Column{.header = "type"}, Column{.header = "port type", .collapsible = true}, Column{.header = "domain", .collapsible = true}, Column{.header = "sync", .collapsible = true}, Column{.header = "optional", .collapsible = true}, Column{.header = "collection", .collapsible = true}, Column{.header = "samples", .collapsible = true}};

constexpr std::array<Column, 5> kSettingColumns{Column{.header = "setting"}, Column{.header = "type"}, Column{.header = "default"}, Column{.header = "unit", .collapsible = true}, Column{.header = "writable", .collapsible = true}};

constexpr std::array<Column, 3> kDifferenceColumns{Column{.header = "item"}, Column{.header = "name"}, Column{.header = "types"}};

[[nodiscard]] std::string hiddenSettingsLine(std::size_t count) { return std::format("{} settings the framework declares on every block are not shown; --all-settings shows them", count); }

// The ports and the settings of one block: one table for all its instantiations, and a table of what only some of
// them declare. A scheduler has no port table.
struct BlockTables {
    std::optional<Table> ports;
    Table                settings;
    std::size_t          hiddenSettings = 0UZ; // the framework's settings left out of `settings`
    Table                differences;
};

[[nodiscard]] BlockTables tablesOf(const NamedBlock& block, const TypeParameters& types, bool allSettings) {
    std::vector<Merged> ports;
    std::vector<Merged> settings;

    // the item under `name`, entered where this is the first instantiation to declare it
    auto place = [](std::vector<Merged>& into, std::vector<std::vector<std::vector<std::string>>>& cells, const std::string& name, std::size_t instantiation, const std::vector<std::string>& parameters) -> std::vector<std::vector<std::string>>& {
        auto held = std::ranges::find(into, name, &Merged::name);
        if (held == into.end()) {
            into.push_back({.name = name, .cells = {}, .declaredBy = {}, .parameters = {}});
            cells.emplace_back();
            held = std::prev(into.end());
        }
        const std::size_t at = static_cast<std::size_t>(std::distance(into.begin(), held));
        held->declaredBy.push_back(instantiation);
        held->parameters.push_back(parameters);
        return cells[at];
    };

    std::vector<std::vector<std::vector<std::string>>> portCells;
    std::vector<std::vector<std::vector<std::string>>> settingCells;
    std::set<std::string>                              hidden;
    for (std::size_t i = 0UZ; i < block.instantiations.size(); ++i) {
        const Instantiation& fact = block.instantiations[i];
        for (const Port& port : fact.ports) {
            place(ports, portCells, std::format("{}\t{}", port.direction, port.name), i, fact.parameters).push_back({port.dataType, port.portType, port.domain, yesNo(port.synchronous), yesNo(port.optional), yesNo(port.collection), std::format("{}..{}", sampleBound(port.minSamples), sampleBound(port.maxSamples))});
        }
        for (const Setting& setting : fact.settings) {
            if (!allSettings && std::ranges::contains(kFrameworkSettings, std::string_view(setting.name))) {
                hidden.insert(setting.name);
                continue;
            }
            std::string type = setting.type.empty() ? "(not stored)" : setting.type;
            if (!setting.enumValues.empty()) {
                type += std::format(" (enum: {})", setting.enumValues);
            }
            place(settings, settingCells, setting.name, i, fact.parameters).push_back({std::move(type), defaultText(setting), setting.unit, yesNo(setting.writable)});
        }
    }

    for (std::size_t i = 0UZ; i < ports.size(); ++i) {
        mergeCells(ports[i], portCells[i], types);
    }
    for (std::size_t i = 0UZ; i < settings.size(); ++i) {
        mergeCells(settings[i], settingCells[i], types);
    }

    BlockTables tables;
    if (block.instantiations.front().registry == Registry::Blocks) {
        std::vector<std::vector<std::string>> rows;
        for (const Merged& port : ports) {
            const std::size_t        tab = port.name.find('\t');
            std::vector<std::string> row{port.name.substr(tab + 1UZ), port.name.substr(0UZ, tab)};
            for (const std::string& cell : port.cells) {
                row.push_back(cell);
            }
            rows.push_back(std::move(row));
        }
        tables.ports = tableOf(kPortColumns, std::move(rows));
    }

    std::vector<std::vector<std::string>> rows;
    for (const Merged& setting : settings) {
        std::vector<std::string> row{setting.name};
        for (const std::string& cell : setting.cells) {
            row.push_back(cell);
        }
        rows.push_back(std::move(row));
    }
    tables.settings       = tableOf(kSettingColumns, std::move(rows));
    tables.hiddenSettings = hidden.size();

    tables.differences.columns.assign(kDifferenceColumns.begin(), kDifferenceColumns.end());
    auto difference = [&block, &tables](std::string_view what, const Merged& item) {
        if (item.declaredBy.size() == block.instantiations.size()) {
            return;
        }
        std::string declared;
        for (const std::size_t i : item.declaredBy) {
            declared += declared.empty() ? "" : " ";
            declared += parameterList(block.instantiations[i]);
        }
        const std::size_t tab = item.name.find('\t');
        tables.differences.rows.push_back({std::string(what), tab == std::string::npos ? item.name : item.name.substr(tab + 1UZ), declared});
    };
    for (const Merged& port : ports) {
        difference("port", port);
    }
    for (const Merged& setting : settings) {
        difference("setting", setting);
    }
    return tables;
}

void printPortsAndSettings(const BlockTables& tables, std::string_view indent) {
    const std::string inner = std::format("{}  ", indent);
    if (tables.ports.has_value()) {
        std::print("\n");
        if (tables.ports->rows.empty()) {
            std::println("{}no stream ports", indent);
        } else {
            printSection(indent, "ports", tables.ports->constant);
            printTable(inner, tables.ports->columns, tables.ports->rows);
        }
    }

    std::print("\n");
    if (tables.settings.rows.empty() && tables.hiddenSettings == 0UZ) {
        std::println("{}no settings", indent);
    } else if (!tables.settings.rows.empty()) {
        printSection(indent, "settings", tables.settings.constant);
        printTable(inner, tables.settings.columns, tables.settings.rows);
    }
    if (tables.hiddenSettings != 0UZ) {
        printWrapped(indent, indent, hiddenSettingsLine(tables.hiddenSettings));
    }

    if (!tables.differences.rows.empty()) {
        std::print("\n");
        std::println("{}declared by some types only", indent);
        printTable(inner, tables.differences.columns, tables.differences.rows);
    }
}

// The facts of one block as `name  value` pairs: the file it came from, its types, its category, the type it is an
// alias of, and the error its settings gave, if any. A block that could not be read has its error in place of the
// category.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> factsOf(const NamedBlock& block) {
    std::vector<std::pair<std::string, std::string>> facts;
    facts.emplace_back("library", block.file.empty() ? std::string(nameOf(block.origin)) : std::format("{} ({})", std::filesystem::path(block.file).filename().string(), nameOf(block.origin)));
    if (block.instantiations.size() > 1UZ) {
        std::string list;
        for (const Instantiation& fact : block.instantiations) {
            list += list.empty() ? "" : " ";
            list += parameterList(fact);
        }
        facts.emplace_back("types", list);
    }
    const Instantiation& first = block.instantiations.front();
    if (!first.error.empty()) {
        facts.emplace_back("error", first.error);
        return facts;
    }
    if (first.registry == Registry::Blocks) {
        facts.emplace_back("category", std::format("{}, UI {}", first.blockCategory, first.uiCategory));
        // A key registered under a name of its own reports the type it is an alias of, and only then is the row
        // worth a line: the type name of a key that is not an alias is the key itself.
        const auto alias = std::ranges::find_if(block.instantiations, [](const Instantiation& fact) { return !fact.typeName.empty() && fact.typeName != fact.key; });
        if (alias != block.instantiations.end()) {
            facts.emplace_back("alias of", alias->typeName);
        }
    }
    if (!first.settingsError.empty()) {
        facts.emplace_back("settings error", first.settingsError);
    }
    return facts;
}

// One block, whatever the number of its instantiations: what it is, what its text says, its ports and its
// settings, each once. `qualified` names the block in full where no family heading stands above it.
void printBlock(const NamedBlock& block, std::string_view indent, bool qualified, bool wholeDescription, bool allSettings) {
    const TypeParameters types  = typeParametersOf(block);
    const std::string    detail = std::format("{}  ", indent);
    std::println("{}{}", indent, headingOf(block, types, qualified));
    printFacts(detail, factsOf(block), Break::BetweenTypes);

    const Instantiation& first = block.instantiations.front();
    if (!first.error.empty() || !first.detailed) {
        return;
    }
    printDescription(first.description, detail, !wholeDescription);
    printPortsAndSettings(tablesOf(block, types, allSettings), detail);
}

struct JsonWriter {
    std::string text;
    std::size_t depth    = 0UZ;
    bool        first    = true;
    bool        afterKey = false;

    void separate() {
        if (afterKey) {
            afterKey = false;
            return;
        }
        if (!first) {
            text += ',';
        }
        if (!(first && depth == 0UZ)) {
            text += '\n';
            text.append(depth * 2UZ, ' ');
        }
        first = false;
    }
    void open(char bracket) {
        separate();
        text += bracket;
        ++depth;
        first = true;
    }
    void close(char bracket) {
        --depth;
        if (!first) {
            text += '\n';
            text.append(depth * 2UZ, ' ');
        }
        text += bracket;
        first = false;
    }

    void beginObject() { open('{'); }
    void endObject() { close('}'); }
    void beginArray() { open('['); }
    void endArray() { close(']'); }

    static void appendQuoted(std::string& out, std::string_view value) {
        out += '"';
        for (const char character : value) {
            switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    out += std::format("\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(character)));
                } else {
                    out += character;
                }
                break;
            }
        }
        out += '"';
    }

    void key(std::string_view name) {
        separate();
        appendQuoted(text, name);
        text += ": ";
        afterKey = true;
    }
    void raw(std::string_view value) {
        separate();
        text += value;
    }
    void string(std::string_view value) {
        separate();
        appendQuoted(text, value);
    }
    void boolean(bool value) { raw(value ? "true" : "false"); }
    void number(std::size_t value) { raw(std::to_string(value)); }

    /// a string member, left out where the framework holds nothing there
    void member(std::string_view name, std::string_view value) {
        if (value.empty()) {
            return;
        }
        key(name);
        string(value);
    }
    void flag(std::string_view name, bool value) {
        key(name);
        boolean(value);
    }
    void count(std::string_view name, std::size_t value) {
        key(name);
        number(value);
    }
    void strings(std::string_view name, std::span<const std::string> values) {
        key(name);
        beginArray();
        for (const std::string& value : values) {
            string(value);
        }
        endArray();
    }
};

// a setting's default as JSON where its type has a JSON form, and as the text the framework's own formatter prints
// where it has not: a complex number, a tensor and a map are strings, and so is a number that does not print as one
void writeValue(JsonWriter& json, const gr::pmt::Value& value) {
    using ValueType = gr::pmt::Value::ValueType;
    if (value.value_type() == ValueType::Bool && value.container_type() == gr::pmt::Value::ContainerType::Scalar) {
        json.boolean(value.value_or(false));
        return;
    }
    if (value.is_signed_integral() || value.is_unsigned_integral()) {
        json.raw(gr::pmt::to_string(value));
        return;
    }
    if (value.is_floating_point()) {
        const double number = value.value_type() == ValueType::Float32 ? static_cast<double>(value.value_or(0.0f)) : value.value_or(0.0);
        if (std::isfinite(number)) {
            json.raw(std::format("{}", number));
        } else {
            json.string(gr::pmt::to_string(value));
        }
        return;
    }
    if (value.is_string()) {
        json.string(value.value_or(std::string_view{}));
        return;
    }
    json.string(gr::pmt::to_string(value));
}

void writePort(JsonWriter& json, const Port& port) {
    json.beginObject();
    json.member("name", port.name);
    json.member("direction", port.direction);
    json.member("dataType", port.dataType);
    json.member("portType", port.portType);
    json.member("domain", port.domain);
    json.flag("synchronous", port.synchronous);
    json.flag("optional", port.optional);
    json.flag("collection", port.collection);
    json.count("minSamples", port.minSamples);
    // the largest representable count is the port's way of declaring no bound, and a member that is not there is
    // how this document says a thing has no value
    if (port.maxSamples != std::numeric_limits<std::size_t>::max()) {
        json.count("maxSamples", port.maxSamples);
    }
    json.endObject();
}

void writeSetting(JsonWriter& json, const Setting& setting) {
    json.beginObject();
    json.member("name", setting.name);
    json.member("type", setting.type);
    if (setting.defaultValue.has_value()) {
        json.key("default");
        writeValue(json, *setting.defaultValue);
    }
    json.member("unit", setting.unit);
    json.member("description", setting.description);
    json.member("documentation", setting.documentation);
    json.member("enumValues", setting.enumValues);
    json.flag("writable", setting.writable);
    json.member("tagKey", setting.tagKey);
    json.flag("visible", setting.visible);
    json.flag("framework", std::ranges::contains(kFrameworkSettings, std::string_view(setting.name)));
    json.endObject();
}

// one instantiation: its key, what the key is made of, and what an instance of it declares
void writeInstantiation(JsonWriter& json, const Instantiation& fact) {
    json.beginObject();
    json.member("key", fact.key);
    json.strings("parameters", fact.parameters);
    json.member("typeName", fact.typeName);
    json.member("blockCategory", fact.blockCategory);
    json.member("uiCategory", fact.uiCategory);
    json.member("error", fact.error);
    json.member("settingsError", fact.settingsError);
    if (fact.detailed) {
        if (fact.registry == Registry::Blocks) {
            json.key("ports");
            json.beginArray();
            for (const Port& port : fact.ports) {
                writePort(json, port);
            }
            json.endArray();
        }
        json.key("settings");
        json.beginArray();
        for (const Setting& setting : fact.settings) {
            writeSetting(json, setting);
        }
        json.endArray();
    }
    json.endObject();
}

// one block: what every instantiation of it shares, and then the instantiations
void writeBlock(JsonWriter& json, const NamedBlock& block, bool withFamily) {
    json.beginObject();
    json.member("name", block.name);
    if (withFamily) {
        json.member("family", block.family);
    }
    json.member("file", block.file);
    json.member("origin", nameOf(block.origin));
    // the text a block annotates its type with, which is the one line a graph editor has to show beside its name in
    // a palette; it belongs to the type, so every instantiation carries the same
    for (const Instantiation& fact : block.instantiations) {
        if (!fact.description.empty()) {
            json.member("description", fact.description);
            break;
        }
    }
    json.key("instantiations");
    json.beginArray();
    for (const Instantiation& fact : block.instantiations) {
        writeInstantiation(json, fact);
    }
    json.endArray();
    json.endObject();
}

void writeTotals(JsonWriter& json, const Totals& totals) {
    json.key("totals");
    json.beginObject();
    json.count("blockKeys", totals.blockKeys);
    json.count("blockFamilies", totals.blockFamilies);
    json.count("schedulerKeys", totals.schedulerKeys);
    json.count("blockLibraries", totals.blockLibraries);
    json.count("plugins", totals.plugins);
    json.count("filesNotLoaded", totals.filesNotLoaded);
    json.count("filesNotOpened", totals.filesNotOpened);
    json.endObject();
}

void reportVersionAsJson(const Context& context) {
    JsonWriter json;
    json.beginObject();
    json.count("schema", 2UZ);
    json.member("command", "version");

    json.key("framework");
    json.beginObject();
    json.member("version", GR_TOOLS_CORE_VERSION);
#ifdef GR_ENABLE_BLOCK_REGISTRY
    json.flag("blockRegistry", true);
#else
    json.flag("blockRegistry", false);
#endif
#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
    json.flag("pluginSystem", true);
#else
    json.flag("pluginSystem", false);
#endif
    json.member("compilerId", CXX_COMPILER_ID);
    json.member("compilerVersion", CXX_COMPILER_VERSION);
    json.member("installPrefix", GR_TOOLS_INSTALLED_PREFIX);
    json.member("installedPluginDirectory", GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    const gr::DataCacheDirectory dataCache = gr::dataCacheDirectory();
    json.member("dataCacheDirectory", dataCache.path);
    json.member("dataCacheOrigin", nameOf(dataCache.origin));
    json.member("compiledDataCacheDirectory", dataCache.compiled);
    json.member("whyNotCompiledDataCacheDirectory", dataCache.whyNotCompiled);
    json.endObject();

    json.key("directories");
    json.beginArray();
    for (const Directory& directory : context.directories) {
        json.beginObject();
        json.member("path", directory.path);
        json.member("origin", directory.origin);
        json.flag("searched", directory.present);
        json.endObject();
    }
    json.endArray();

    json.key("libraries");
    json.beginArray();
    for (const FileFact& file : context.files) {
        json.beginObject();
        json.member("file", file.file);
        json.member("kind", nameOf(file.kind));
        json.count("blockRegistrations", file.blockRegistrations);
        json.count("schedulerRegistrations", file.schedulerRegistrations);
        json.member("reason", file.reason);
        json.endObject();
    }
    json.endArray();

    json.key("plugins");
    json.beginArray();
    for (const PluginFact& plugin : context.plugins) {
        json.beginObject();
        json.member("name", plugin.name);
        json.member("version", plugin.version);
        json.member("license", plugin.license);
        json.member("author", plugin.author);
        json.count("blocks", plugin.blocks);
        json.count("schedulers", plugin.schedulers);
        json.endObject();
    }
    json.endArray();

    json.strings("schedulers", context.schedulers);

    writeTotals(json, context.totals);
    json.endObject();
    std::println("{}", json.text);
}

// a path under `prefix` written relative to it: the prefix is on the line above, and a report that repeats it on
// every line below is longer without saying more
[[nodiscard]] std::string underPrefix(std::string_view path, std::string_view prefix) {
    if (prefix.empty() || !path.starts_with(prefix)) {
        return std::string(path);
    }
    std::string_view rest = path.substr(prefix.size());
    while (rest.starts_with('/')) {
        rest.remove_prefix(1UZ);
    }
    return rest.empty() ? std::string(".") : std::string(rest);
}

// The data cache directory in use for the report, with what chose it when that is not the compiled path: the
// environment variable, or the user's cache directory and why the compiled one cannot be used.
[[nodiscard]] std::string dataCacheFact(const gr::DataCacheDirectory& dataCache) {
    using enum gr::DataCacheDirectory::Origin;
    switch (dataCache.origin) {
    case Environment: return std::format("{}, from GR_DATA_CACHE_DIR", dataCache.path);
    case Compiled: return underPrefix(dataCache.path, GR_TOOLS_INSTALLED_PREFIX);
    case User: return std::format("{}, the user's cache directory; the compiled {} cannot be used: {}", dataCache.path, dataCache.compiled, dataCache.whyNotCompiled);
    case None: break;
    }
    return std::format("none, caching is off; the compiled {} cannot be used: {}; {}", dataCache.compiled, dataCache.whyNotCompiled, dataCache.user.empty() ? dataCache.whyNotUser : std::format("the user's {} cannot be used: {}", dataCache.user, dataCache.whyNotUser));
}

// the files of one directory under the directory's own line, by the name that tells them apart
[[nodiscard]] std::vector<std::pair<std::string, std::vector<const FileFact*>>> byDirectory(const std::vector<FileFact>& files) {
    std::vector<std::pair<std::string, std::vector<const FileFact*>>> directories;
    for (const FileFact& file : files) {
        const std::string parent = std::filesystem::path(file.file).parent_path().string();
        auto              held   = std::ranges::find(directories, parent, [](const auto& entry) -> const std::string& { return entry.first; });
        if (held == directories.end()) {
            directories.emplace_back(parent, std::vector<const FileFact*>{});
            held = std::prev(directories.end());
        }
        held->second.push_back(&file);
    }
    return directories;
}

// what this build of the framework is and where this installation keeps its plugins and its cache
[[nodiscard]] std::vector<std::pair<std::string, std::string>> frameworkFacts() {
    std::vector<std::pair<std::string, std::string>> facts;
#ifdef GR_ENABLE_BLOCK_REGISTRY
    facts.emplace_back("block registry", "enabled");
#else
    facts.emplace_back("block registry", "disabled");
#endif
#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
    facts.emplace_back("plugin system", "enabled");
#else
    facts.emplace_back("plugin system", "disabled");
#endif
    facts.emplace_back("compiler", std::format("{} {}", CXX_COMPILER_ID, CXX_COMPILER_VERSION));
    facts.emplace_back("install prefix", GR_TOOLS_INSTALLED_PREFIX);
    facts.emplace_back("plugin directory", underPrefix(GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY, GR_TOOLS_INSTALLED_PREFIX));
    facts.emplace_back("data cache", dataCacheFact(gr::dataCacheDirectory()));
    return facts;
}

constexpr std::array<Column, 3> kDirectoryColumns{Column{.header = "directory"}, Column{.header = "origin"}, Column{.header = "result"}};

[[nodiscard]] std::vector<std::vector<std::string>> directoryRows(const Context& context) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(context.directories.size());
    for (const Directory& directory : context.directories) {
        rows.push_back({directory.path, directory.origin, directory.present ? "searched" : "not a directory"});
    }
    return rows;
}

constexpr std::array<Column, 4> kFileColumns{Column{.header = "library"}, Column{.header = "kind", .collapsible = true}, Column{.header = "blocks", .align = Align::Right}, Column{.header = "schedulers", .align = Align::Right}};

// the files of one directory by their names, with the counts of what each registered
[[nodiscard]] Table fileTable(std::span<const FileFact* const> files) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(files.size());
    for (const FileFact* file : files) {
        rows.push_back({std::filesystem::path(file->file).filename().string(), std::string(nameOf(file->kind)), file->reason.empty() ? std::to_string(file->blockRegistrations) : "-", file->reason.empty() ? std::to_string(file->schedulerRegistrations) : "-"});
    }
    return tableOf(kFileColumns, std::move(rows));
}

// `file: reason` for each file of one directory that did not load
[[nodiscard]] std::vector<std::string> refusals(std::span<const FileFact* const> files) {
    std::vector<std::string> lines;
    for (const FileFact* file : files) {
        if (!file->reason.empty()) {
            lines.push_back(std::format("{}: {}", std::filesystem::path(file->file).filename().string(), file->reason));
        }
    }
    return lines;
}

constexpr std::array<Column, 5> kPluginColumns{Column{.header = "plugin"}, Column{.header = "version"}, Column{.header = "license"}, Column{.header = "blocks", .align = Align::Right}, Column{.header = "schedulers", .align = Align::Right}};

[[nodiscard]] std::vector<std::vector<std::string>> pluginRows(const Context& context) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(context.plugins.size());
    for (const PluginFact& plugin : context.plugins) {
        rows.push_back({plugin.name, plugin.version, plugin.license, std::to_string(plugin.blocks), std::to_string(plugin.schedulers)});
    }
    return rows;
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> versionTotals(const Context& context) {
    return {
        {"block keys", std::to_string(context.totals.blockKeys)},
        {"block families", std::to_string(context.totals.blockFamilies)},
        {"scheduler keys", std::to_string(context.totals.schedulerKeys)},
        {"libraries", std::to_string(context.totals.blockLibraries)},
        {"plugins", std::to_string(context.totals.plugins)},
        {"files not loaded", std::to_string(context.totals.filesNotLoaded)},
        {"files not opened", std::to_string(context.totals.filesNotOpened)},
    };
}

// the totals as a table of two columns without a header line, the counts aligned on their last digit
void printTotals(const std::vector<std::pair<std::string, std::string>>& totals) {
    std::print("\n");
    std::println("totals");
    constexpr std::array<Column, 2>       totalColumns{Column{.header = ""}, Column{.header = "", .align = Align::Right}};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(totals.size());
    for (const auto& [name, count] : totals) {
        rows.push_back({name, count});
    }
    printTable("  ", totalColumns, rows);
}

void reportVersion(const Context& context) {
    std::println("GNU Radio 4 {}", GR_TOOLS_CORE_VERSION);
    std::print("\n");
    printFacts("  ", frameworkFacts());

    std::print("\n");
    std::println("search path");
    printTable("  ", kDirectoryColumns, directoryRows(context));

    if (!context.files.empty()) {
        std::print("\n");
        std::println("libraries");
        for (const auto& [directory, files] : byDirectory(context.files)) {
            std::println("  {}", fit(directory, kWidth - 2UZ));
            const Table table = fileTable(files);
            if (!table.constant.empty()) {
                printWrapped("    ", "    ", std::format("({})", table.constant));
            }
            printTable("    ", table.columns, table.rows);
            for (const std::string& refusal : refusals(files)) {
                printWrapped("    ", "      ", refusal);
            }
        }
    }

    if (!context.plugins.empty()) {
        std::print("\n");
        std::println("plugins");
        printTable("  ", kPluginColumns, pluginRows(context));
    }

    if (!context.schedulers.empty()) {
        std::print("\n");
        std::println("schedulers");
        for (const std::string& scheduler : context.schedulers) {
            std::println("  {}", fit(scheduler, kWidth - 2UZ));
        }
    }

    printTotals(versionTotals(context));
}

// a block's name and the type parameters of its instantiations after it, the list carried on under the name where
// it does not fit one line
void printBlockLine(std::string_view indent, const NamedBlock& block, std::size_t nameWidth) {
    std::vector<std::string> pieces;
    for (const Instantiation& fact : block.instantiations) {
        if (!fact.parameters.empty()) {
            pieces.push_back(parameterList(fact));
        }
    }
    if (pieces.empty()) {
        std::println("{}{}", indent, block.name);
        return;
    }
    std::string list;
    for (const std::string& piece : pieces) {
        list += list.empty() ? "" : " ";
        list += piece;
    }
    const std::vector<std::string> lines = wrap(list, kWidth - indent.size() - nameWidth - 2UZ, Break::BetweenTypes);
    for (std::size_t i = 0UZ; i < lines.size(); ++i) {
        const std::string name = i == 0UZ ? fit(block.name, nameWidth) : std::string{};
        std::println("{}{}{}  {}", indent, name, std::string(nameWidth - name.size(), ' '), lines[i]);
    }
}

// one library object per file, each holding its families and they their blocks or their schedulers
void writeLibraries(JsonWriter& json, const std::vector<Library>& libraries, Registry registry) {
    json.key("libraries");
    json.beginArray();
    for (const Library& library : libraries) {
        json.beginObject();
        json.member("file", library.file);
        json.member("origin", nameOf(library.origin));
        json.count(registry == Registry::Blocks ? "blockKeys" : "schedulerKeys", library.keys);
        json.key("families");
        json.beginArray();
        for (const Family& family : library.families) {
            json.beginObject();
            json.member("name", family.name);
            json.key(registry == Registry::Blocks ? "blocks" : "schedulers");
            json.beginArray();
            for (const NamedBlock& block : family.blocks) {
                writeBlock(json, block, false);
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
}

// the libraries under the directories that held them, each with its families and in each family one line per entry,
// or each entry in detail
void printLibraries(const std::vector<Library>& libraries, bool detailed, bool allSettings) {
    std::string lastDirectory;
    bool        firstDirectory = true;
    for (const Library& library : libraries) {
        const std::string directory = std::filesystem::path(library.file).parent_path().string();
        if (directory != lastDirectory || firstDirectory) {
            lastDirectory = directory;
            if (!firstDirectory) {
                std::print("\n");
            }
            firstDirectory = false;
            std::println("{}", fit(directory.empty() ? "not attributed" : directory, kWidth));
        }
        std::print("\n");
        const std::string name = library.file.empty() ? std::string(nameOf(library.origin)) : std::filesystem::path(library.file).filename().string();
        printWrapped("  ", "    ", std::format("{} ({}, {} keys)", name, nameOf(library.origin), library.keys));
        for (const Family& family : library.families) {
            std::print("\n");
            std::println("    {}", fit(family.name.empty() ? "(no namespace)" : family.name, kWidth - 4UZ));
            if (detailed) {
                for (const NamedBlock& block : family.blocks) {
                    std::print("\n");
                    printBlock(block, "      ", false, false, allSettings);
                }
                continue;
            }
            std::size_t nameWidth = 0UZ;
            for (const NamedBlock& block : family.blocks) {
                nameWidth = std::max(nameWidth, block.name.size());
            }
            nameWidth = std::min(nameWidth, kWidth / 2UZ);
            for (const NamedBlock& block : family.blocks) {
                printBlockLine("      ", block, nameWidth);
            }
        }
    }
}

using gr::tools::DocWriter;

// The Markdown and HTML forms of the report. They carry what the text report prints, from the same facts and the same
// tables, as headings, lists and tables. A value is never cut, because a document has no line width.
[[nodiscard]] DocWriter documentFor(Form form, std::string title) { return DocWriter(form == Form::Html ? gr::tools::Format::Html : gr::tools::Format::Markdown, std::move(title)); }

[[nodiscard]] std::vector<std::string_view> headersOf(std::span<const Column> columns) {
    std::vector<std::string_view> headers;
    headers.reserve(columns.size());
    for (const Column& column : columns) {
        headers.push_back(column.header);
    }
    return headers;
}

void writeTable(DocWriter& document, std::span<const Column> columns, std::span<const std::vector<std::string>> rows) { document.table(headersOf(columns), rows); }

// one `name: value` item per fact
void writeFacts(DocWriter& document, const std::vector<std::pair<std::string, std::string>>& facts) {
    std::vector<std::string> items;
    items.reserve(facts.size());
    for (const auto& [name, value] : facts) {
        items.push_back(document.labeled(name, value));
    }
    document.rawBullets(items);
}

void writeTotals(DocWriter& document, const std::vector<std::pair<std::string, std::string>>& totals) {
    document.heading(2UZ, "Totals", document.anchor("totals"));
    writeFacts(document, totals);
}

// one block as a section under a heading of `level`
void documentBlock(DocWriter& document, const NamedBlock& block, std::size_t level, std::string_view anchorName, bool qualified, bool wholeDescription, bool allSettings) {
    const TypeParameters types = typeParametersOf(block);
    document.heading(level, headingOf(block, types, qualified), anchorName);
    writeFacts(document, factsOf(block));

    const Instantiation& first = block.instantiations.front();
    if (!first.error.empty() || !first.detailed) {
        return;
    }
    for (const std::string& paragraph : paragraphsOf(first.description)) {
        document.paragraph(paragraph);
        if (!wholeDescription) {
            break;
        }
    }

    const BlockTables tables = tablesOf(block, types, allSettings);
    if (tables.ports.has_value()) {
        if (tables.ports->rows.empty()) {
            document.paragraph("no stream ports");
        } else {
            document.paragraph(sectionLine("ports", tables.ports->constant));
            writeTable(document, tables.ports->columns, tables.ports->rows);
        }
    }
    if (tables.settings.rows.empty() && tables.hiddenSettings == 0UZ) {
        document.paragraph("no settings");
    } else if (!tables.settings.rows.empty()) {
        document.paragraph(sectionLine("settings", tables.settings.constant));
        writeTable(document, tables.settings.columns, tables.settings.rows);
    }
    if (tables.hiddenSettings != 0UZ) {
        document.paragraph(hiddenSettingsLine(tables.hiddenSettings));
    }
    if (!tables.differences.rows.empty()) {
        document.paragraph("declared by some types only");
        writeTable(document, tables.differences.columns, tables.differences.rows);
    }
}

// A section per library and in it one per family. Each family is a table of its entries. With `detailed`, each entry
// has a section of its own, and the document opens with a link to every entry's section.
void documentLibraries(DocWriter& document, const std::vector<Library>& libraries, bool detailed, bool allSettings) {
    std::vector<std::string> anchors; // one per entry, in the order of their sections
    if (detailed) {
        std::vector<std::string> links;
        for (const Library& library : libraries) {
            for (const Family& family : library.families) {
                for (const NamedBlock& block : family.blocks) {
                    const std::string name = headingOf(block, typeParametersOf(block), true);
                    anchors.push_back(document.anchor(family.name.empty() ? block.name : std::format("{}::{}", family.name, block.name)));
                    links.push_back(document.link(name, anchors.back()));
                }
            }
        }
        document.rawBullets(links);
    }

    std::size_t entry = 0UZ;
    for (const Library& library : libraries) {
        const std::string name      = library.file.empty() ? std::string(nameOf(library.origin)) : std::filesystem::path(library.file).filename().string();
        const std::string directory = std::filesystem::path(library.file).parent_path().string();
        document.heading(2UZ, name, document.anchor(name));
        writeFacts(document, {{"directory", directory.empty() ? "not attributed" : directory}, {"origin", std::string(nameOf(library.origin))}, {"keys", std::to_string(library.keys)}});
        for (const Family& family : library.families) {
            const std::string familyName = family.name.empty() ? "(no namespace)" : family.name;
            document.heading(3UZ, familyName, document.anchor(std::format("{} {}", name, familyName)));
            if (detailed) {
                for (const NamedBlock& block : family.blocks) {
                    documentBlock(document, block, 4UZ, anchors[entry++], false, false, allSettings);
                }
                continue;
            }
            std::vector<std::vector<std::string>> rows;
            rows.reserve(family.blocks.size());
            for (const NamedBlock& block : family.blocks) {
                std::string list;
                for (const Instantiation& fact : block.instantiations) {
                    if (!fact.parameters.empty()) {
                        list += list.empty() ? "" : " ";
                        list += parameterList(fact);
                    }
                }
                rows.push_back({block.name, std::move(list)});
            }
            constexpr std::array<Column, 2> blockColumns{Column{.header = "block"}, Column{.header = "types"}};
            writeTable(document, blockColumns, rows);
        }
    }
}

void documentVersion(DocWriter& document, const Context& context) {
    document.paragraph(std::format("GNU Radio 4 {}", GR_TOOLS_CORE_VERSION));
    writeFacts(document, frameworkFacts());

    document.heading(2UZ, "Search path", document.anchor("search path"));
    writeTable(document, kDirectoryColumns, directoryRows(context));

    if (!context.files.empty()) {
        document.heading(2UZ, "Libraries", document.anchor("libraries"));
        for (const auto& [directory, files] : byDirectory(context.files)) {
            document.heading(3UZ, directory, document.anchor(directory));
            const Table table = fileTable(files);
            document.paragraph(table.constant);
            writeTable(document, table.columns, table.rows);
            std::vector<std::string> items;
            for (const std::string& refusal : refusals(files)) {
                items.push_back(document.text(refusal));
            }
            document.rawBullets(items);
        }
    }

    if (!context.plugins.empty()) {
        document.heading(2UZ, "Plugins", document.anchor("plugins"));
        writeTable(document, kPluginColumns, pluginRows(context));
    }

    if (!context.schedulers.empty()) {
        document.heading(2UZ, "Schedulers", document.anchor("schedulers"));
        std::vector<std::string> items;
        for (const std::string& scheduler : context.schedulers) {
            items.push_back(document.text(scheduler));
        }
        document.rawBullets(items);
    }

    writeTotals(document, versionTotals(context));
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> blockTotals(const Context& context, const std::vector<Library>& libraries) {
    return {
        {"block keys", std::to_string(context.totals.blockKeys)},
        {"block families", std::to_string(context.totals.blockFamilies)},
        {"libraries", std::to_string(libraries.size())},
    };
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> schedulerTotals(const Context& context, const std::vector<Library>& libraries) {
    return {
        {"scheduler keys", std::to_string(context.totals.schedulerKeys)},
        {"libraries", std::to_string(libraries.size())},
    };
}

void reportBlocks(Context& context, const Options& options) {
    std::vector<Instantiation> facts;
    facts.reserve(context.keys.size());
    for (const std::string& key : context.keys) {
        facts.push_back(readKey(context, key, Registry::Blocks, options.verbose));
    }
    const std::vector<Library> libraries = group(std::move(facts));

    if (options.form == Form::Json) {
        JsonWriter json;
        json.beginObject();
        json.count("schema", 2UZ);
        json.member("command", "blocks");
        writeLibraries(json, libraries, Registry::Blocks);
        writeTotals(json, context.totals);
        json.endObject();
        std::println("{}", json.text);
        return;
    }
    if (options.form != Form::Text) {
        DocWriter document = documentFor(options.form, "grinfo blocks");
        documentLibraries(document, libraries, options.verbose, options.allSettings);
        writeTotals(document, blockTotals(context, libraries));
        std::print("{}", document.finish());
        return;
    }

    printLibraries(libraries, options.verbose, options.allSettings);
    printTotals(blockTotals(context, libraries));
}

void reportSchedulers(Context& context, const Options& options) {
    std::vector<Instantiation> facts;
    facts.reserve(context.schedulers.size());
    for (const std::string& key : context.schedulers) {
        facts.push_back(readKey(context, key, Registry::Schedulers, true));
    }
    const std::vector<Library> libraries = group(std::move(facts));

    if (options.form == Form::Json) {
        JsonWriter json;
        json.beginObject();
        json.count("schema", 2UZ);
        json.member("command", "schedulers");
        writeLibraries(json, libraries, Registry::Schedulers);
        writeTotals(json, context.totals);
        json.endObject();
        std::println("{}", json.text);
        return;
    }
    if (options.form != Form::Text) {
        DocWriter document = documentFor(options.form, "grinfo schedulers");
        documentLibraries(document, libraries, true, options.allSettings);
        writeTotals(document, schedulerTotals(context, libraries));
        std::print("{}", document.finish());
        return;
    }

    printLibraries(libraries, true, options.allSettings);
    printTotals(schedulerTotals(context, libraries));
}

// the directions of a family's blocks, as the report words them
[[nodiscard]] std::string_view directionsText(bool receive, bool transmit) {
    if (receive && transmit) {
        return "receive and transmit";
    }
    if (receive || transmit) {
        return receive ? "receive" : "transmit";
    }
    return "no direction";
}

void writeDevicesAsJson(const gr::DeviceList& list) {
    JsonWriter json;
    json.beginObject();
    json.count("schema", 2UZ);
    json.member("command", "devices");
    json.key("units");
    json.beginArray();
    for (const gr::ListedUnit& unit : list.units) {
        json.beginObject();
        json.member("family", unit.family);
        json.member("selector", unit.selector);
        json.member("label", unit.label);
        json.flag("receive", unit.receive);
        json.flag("transmit", unit.transmit);
        json.endObject();
    }
    json.endArray();
    json.key("families");
    json.beginArray();
    for (const gr::ListedFamily& family : list.families) {
        json.beginObject();
        json.member("family", family.family);
        json.flag("opens", family.opens);
        json.flag("listed", family.listed);
        json.member("reason", family.reason);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::println("{}", json.text);
}

// Each family under its word: the directions of its blocks as its held listing states them and whether that listing
// opens a unit, then the selector and the label of each unit it listed, or why it listed none.
void reportDevices(const Context& context, const Options& options) {
    const gr::DeviceList list = context.loader.listDevices(options.allowOpen ? gr::Opening::Allowed : gr::Opening::Avoided);
    if (options.form == Form::Json) {
        writeDevicesAsJson(list);
        return;
    }

    struct FamilyReport {
        std::string                                      heading;
        std::vector<std::pair<std::string, std::string>> units; // each unit's selector and label
        std::string                                      reason;
    };
    std::vector<FamilyReport>             families;
    bool                                  leftOutForOpening = false;
    const std::vector<gr::DeviceListing>& held              = gr::globalDeviceListingRegistry().listings();
    for (const gr::ListedFamily& family : list.families) {
        std::vector<std::pair<std::string, std::string>> units;
        std::string                                      traits;
        if (const auto listing = std::ranges::find(held, family.family, &gr::DeviceListing::family); listing != held.end()) {
            traits = directionsText(listing->receive, listing->transmit);
        }
        for (const gr::ListedUnit& unit : list.units) {
            if (unit.family != family.family) {
                continue;
            }
            units.emplace_back(unit.selector.empty() ? std::string("(no selector)") : unit.selector, unit.label);
        }
        if (family.opens) {
            traits += traits.empty() ? "opens a unit" : ", opens a unit";
        }
        families.push_back({.heading = traits.empty() ? family.family : std::format("{} ({})", family.family, traits), .units = std::move(units), .reason = family.reason.empty() ? std::string{} : std::format("{}: {}", family.listed ? "listed" : "not listed", family.reason)});
        leftOutForOpening = leftOutForOpening || (family.opens && !family.listed && !options.allowOpen);
    }
    constexpr std::string_view kNoFamily  = "no device family is registered";
    constexpr std::string_view kAllowOpen = "A family whose listing opens a unit is asked with --allow-open.";

    if (options.form != Form::Text) {
        DocWriter document = documentFor(options.form, "grinfo devices");
        if (families.empty()) {
            document.paragraph(kNoFamily);
        }
        for (const FamilyReport& family : families) {
            document.heading(2UZ, family.heading, document.anchor(family.heading));
            writeFacts(document, family.units);
            document.paragraph(family.reason);
        }
        if (leftOutForOpening) {
            document.paragraph(kAllowOpen);
        }
        std::print("{}", document.finish());
        return;
    }

    if (families.empty()) {
        std::println("{}", kNoFamily);
    }
    for (const FamilyReport& family : families) {
        if (&family != &families.front()) {
            std::print("\n");
        }
        std::println("{}", family.heading);
        printFacts("  ", family.units);
        if (!family.reason.empty()) {
            printWrapped("  ", "    ", family.reason);
        }
    }
    if (leftOutForOpening) {
        std::print("\n");
        std::println("{}", kAllowOpen);
    }
}

// 0 when the name selected something, 1 when nothing of that name is registered
[[nodiscard]] int reportBlock(Context& context, const Options& options) {
    const std::string&         wanted = options.blockName;
    std::vector<Instantiation> selected;
    for (const std::string& key : context.keys) {
        const KeyParts    parts     = partsOf(key);
        const std::string qualified = parts.family.empty() ? parts.name : parts.family + "::" + parts.name;
        if (key != wanted && parts.name != wanted && qualified != wanted) {
            continue;
        }
        selected.push_back(readKey(context, key, Registry::Blocks, true));
    }
    if (selected.empty()) {
        std::println(stderr, "{}: no block named {} is registered", kProgram, wanted);
        return 1;
    }

    std::vector<NamedBlock> blocks;
    for (const Library& library : group(std::move(selected))) {
        for (const Family& family : library.families) {
            for (const NamedBlock& block : family.blocks) {
                blocks.push_back(block);
            }
        }
    }

    if (options.form == Form::Json) {
        JsonWriter json;
        json.beginObject();
        json.count("schema", 2UZ);
        json.member("command", "block");
        json.member("query", wanted);
        json.key("blocks");
        json.beginArray();
        for (const NamedBlock& block : blocks) {
            writeBlock(json, block, true);
        }
        json.endArray();
        json.endObject();
        std::println("{}", json.text);
        return 0;
    }
    if (options.form != Form::Text) {
        DocWriter document = documentFor(options.form, std::format("grinfo block {}", wanted));
        for (const NamedBlock& block : blocks) {
            documentBlock(document, block, 2UZ, document.anchor(std::format("{}::{}", block.family, block.name)), true, true, options.allSettings);
        }
        std::print("{}", document.finish());
        return 0;
    }

    for (std::size_t i = 0UZ; i < blocks.size(); ++i) {
        if (i != 0UZ) {
            std::print("\n");
        }
        printBlock(blocks[i], "", true, true, options.allSettings);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    const std::optional<Options> parsed = parse(arguments);
    if (!parsed.has_value()) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    const Options& options = *parsed;
    if (options.help) {
        std::print("{}", kUsage);
        return 0;
    }

    std::vector<Directory> directories = searchDirectories(options.pluginDirectories, GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    bool                   searchable  = true;
    for (const Directory& directory : directories) {
        if (directory.origin == "option" && !directory.present) {
            std::println(stderr, "{}: {} could not be searched; it is not a directory", kProgram, directory.path);
            searchable = false;
        }
    }
    if (!searchable) {
        return 1;
    }

    std::vector<std::string> paths;
    paths.reserve(directories.size());
    for (const Directory& directory : directories) {
        paths.push_back(directory.path);
    }
    gr::detail::registerShippedSchedulers();
    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), paths);

    Context context{.loader = loader, .directories = std::move(directories), .keys = {}, .schedulers = {}, .files = {}, .plugins = {}, .libraryFiles = {}, .program = thisProgramFile(), .totals = {}};

    context.keys = loader.availableBlocks();
    std::ranges::sort(context.keys);
    context.keys.erase(std::ranges::unique(context.keys).begin(), context.keys.end());
    context.schedulers = loader.availableSchedulers();
    std::ranges::sort(context.schedulers);
    context.schedulers.erase(std::ranges::unique(context.schedulers).begin(), context.schedulers.end());

    std::set<std::string> families;
    for (const std::string& key : context.keys) {
        families.emplace(partsOf(key).family);
    }

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
    for (const gr::PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
        context.libraryFiles.emplace(canonicalPath(library.file));
        context.files.push_back({.file = library.file, .kind = FileKind::BlockLibrary, .blockRegistrations = library.nBlockRegistrations, .schedulerRegistrations = library.nSchedulerRegistrations, .reason = {}});
    }
    for (const auto& [file, reason] : loader.failedPlugins()) {
        context.files.push_back({.file = file, .kind = FileKind::NotLoaded, .blockRegistrations = 0UZ, .schedulerRegistrations = 0UZ, .reason = reason});
    }
    for (const std::string& file : loader.skippedFiles()) {
        context.files.push_back({.file = file, .kind = FileKind::NotOpened, .blockRegistrations = 0UZ, .schedulerRegistrations = 0UZ, .reason = "its name only reads as a shared object"});
    }
    for (const auto& plugin : loader.plugins()) {
        context.plugins.push_back({.name = plugin->metadata.plugin_name, .author = plugin->metadata.plugin_author, .license = plugin->metadata.plugin_license, .version = plugin->metadata.plugin_version, .blocks = plugin->availableBlocks().size(), .schedulers = plugin->availableSchedulers().size()});
    }
    context.totals.blockLibraries = loader.blockLibraries().size();
    context.totals.plugins        = loader.plugins().size();
    context.totals.filesNotLoaded = loader.failedPlugins().size();
    context.totals.filesNotOpened = loader.skippedFiles().size();
#endif
    std::ranges::sort(context.files, [](const FileFact& left, const FileFact& right) { return left.file < right.file; });
    std::ranges::sort(context.plugins, [](const PluginFact& left, const PluginFact& right) { return left.name < right.name; });
    context.totals.blockKeys     = context.keys.size();
    context.totals.blockFamilies = families.size();
    context.totals.schedulerKeys = context.schedulers.size();

    switch (options.command) {
    case Command::Version:
        if (options.form == Form::Json) {
            reportVersionAsJson(context);
        } else if (options.form != Form::Text) {
            DocWriter document = documentFor(options.form, "grinfo version");
            documentVersion(document, context);
            std::print("{}", document.finish());
        } else {
            reportVersion(context);
        }
        return 0;
    case Command::Blocks: reportBlocks(context, options); return 0;
    case Command::Block: return reportBlock(context, options);
    case Command::Schedulers: reportSchedulers(context, options); return 0;
    case Command::Devices: reportDevices(context, options); return 0;
    }
    return 0;
}
