// grinfo - report the framework, the directories it searches for blocks, and the blocks they bring.
//
// No block name is built in: the registry linked into this program, the libraries the plugin loader opens and a
// default-constructed instance of each registered key are the only sources, so a framework that gains a block
// reports it here without this program being rebuilt.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // dladdr, which names the file a block's type information was loaded from
#endif

#include <algorithm>
#include <array>
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
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/config.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>
#include <gnuradio-4.0/meta/formatter.hpp>

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
#include <dlfcn.h>
#endif

namespace {

constexpr std::string_view kProgram = "grinfo";

constexpr std::string_view kUsage = R"(grinfo - report the GNU Radio 4 framework, its plugins and its blocks

Usage: grinfo [command] [options]

  version            the framework, the directories searched and what each of them loaded (the default)
  blocks             every registered block key, by the file that registered it and by family
  block <name>       one block in detail: where it came from, its ports and its settings

  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --json             the same content as a pretty-printed JSON document
  --verbose          with blocks, the detail of every block rather than its name alone
  --help, -h         this text

The blocks come from the directories named by --plugin-dir, from GNURADIO4_PLUGIN_DIRECTORIES, the
colon-separated list the framework's own plugin loader reads, and from the plugin directory of this
installation, which is always searched. A directory named twice is searched once.

<name> is a registry key with its template parameters, gr::blocks::basic::Convert<int16, float32>, or a
name without them, gr::blocks::basic::Convert or Convert, which selects every instantiation of it.

What a block reports is read from a default-constructed instance of it. The file a key came from is the
file the dynamic linker holds that instance's type information in, so a block linked into this program is
told apart from one a library brought.

The JSON document carries "schema": 1 and one shape per command. Every key of a shape is always present:
null means the framework holds nothing there or the field was not read, and an empty array means the
block declares none of it.

Exit status is 0, 1 when a block named to block is not registered or a directory named by --plugin-dir
cannot be searched, and 2 when the command line cannot be used.
)";

enum class Command : std::uint8_t { Version, Blocks, Block };

struct Options {
    Command                  command = Command::Version;
    std::string              blockName;
    std::vector<std::string> pluginDirectories;
    bool                     json    = false;
    bool                     verbose = false;
    bool                     help    = false;
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
        if (argument == "--json") {
            options.json = true;
            ++index;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
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

struct Directory {
    std::string path;
    std::string source; // the command line, the environment, or this installation
    bool        present = false;
};

// where the blocks are looked for, in the order the directories are searched
[[nodiscard]] std::vector<Directory> searchDirectories(const std::vector<std::string>& fromCommandLine) {
    std::vector<Directory> directories;
    auto                   add = [&directories](std::string_view path, std::string_view source) {
        if (path.empty() || std::ranges::any_of(directories, [path](const Directory& held) { return held.path == path; })) {
            return;
        }
        std::error_code ignored;
        directories.push_back({.path = std::string(path), .source = std::string(source), .present = std::filesystem::is_directory(path, ignored)});
    };
    for (const std::string& directory : fromCommandLine) {
        add(directory, "command line");
    }
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        const std::string_view list(environment);
        for (std::size_t start = 0UZ; start < list.size();) {
            const std::size_t separator = list.find(':', start);
            const std::size_t end       = separator == std::string_view::npos ? list.size() : separator;
            add(list.substr(start, end - start), "environment");
            start = end + 1UZ;
        }
    }
    add(GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY, "installation");
    return directories;
}

#ifdef INTERNAL_ENABLE_BLOCK_PLUGINS
const int kAddressInThisProgram = 0;

// the file the dynamic linker holds `address` in, or nothing when it names none
[[nodiscard]] std::string fileHolding(const void* address) {
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    return canonicalPath(info.dli_fname);
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

struct Port {
    std::string direction;
    std::string index; // the port's index, or index.sub-index for one inside a collection
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
    std::string                   enumType;
    std::string                   enumValues;
    bool                          writable      = false;
    bool                          autoForwarded = false;
    std::optional<bool>           visible;
};

// one registered key, read from an instance of it
struct Instantiation {
    std::string          key;
    std::string          family;
    std::string          name;
    std::string          parameters;
    std::string          file;   // the library the type came from, empty where the linker named none
    std::string          source; // what that file is to this program
    std::string          typeName;
    std::string          blockCategory;
    std::string          uiCategory;
    std::string          error; // why no instance could be made; nothing below is filled then
    bool                 detailed = false;
    std::string          description;
    std::string          settingsError;
    std::vector<Port>    ports;
    std::vector<Setting> settings;
};

// one file a plugin directory held
struct FileFact {
    std::string file;
    std::string kind; // what the loader made of it
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
    auto one = [direction, &into](std::string index, std::string name, gr::DynamicPort& port, bool collection) {
        into.push_back({
            .direction   = std::string(direction),
            .index       = std::move(index),
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
                one(std::format("{}.{}", i, j), std::format("{}#{}", collection->name, j), collection->ports[j], true);
            }
            if (collection->ports.empty()) {
                Port empty;
                empty.direction  = std::string(direction);
                empty.index      = std::to_string(i);
                empty.name       = std::format("{} (empty collection)", collection->name);
                empty.collection = true;
                into.push_back(std::move(empty));
            }
        } else if (auto* port = std::get_if<gr::DynamicPort>(&ports[i]); port != nullptr) {
            one(std::to_string(i), std::string(port->metaInfo.name), *port, false);
        }
    }
}

void collectSettings(const gr::BlockModel& block, std::vector<Setting>& into) {
    const gr::SettingsBase&      settings  = block.settings();
    const gr::property_map&      meta      = block.metaInformation();
    const gr::property_map       defaults  = settings.defaultParameters();
    const std::set<std::string>& writable  = settings.writableMembers();
    const std::set<std::string>& forwarded = settings.autoForwardParameters();

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
        setting.enumType      = metaString(meta, name + "::enum_type");
        setting.enumValues    = metaString(meta, name + "::enum_values");
        setting.writable      = writable.contains(name);
        setting.autoForwarded = forwarded.contains(name);
        if (const auto visible = meta.find(name + "::visible"); visible != meta.cend()) {
            setting.visible = visible->second.value_or(true);
        }
        into.push_back(std::move(setting));
    }
}

// what a file the linker named is to this program
[[nodiscard]] std::string sourceOfFile(const Context& context, const std::string& file) {
    if (file.empty()) {
        return "not attributed";
    }
    if (file == context.program) {
        return "this program";
    }
    if (context.libraryFiles.contains(file)) {
        return "block library";
    }
    const std::string parent = std::filesystem::path(file).parent_path().string();
    if (std::ranges::any_of(context.directories, [&parent](const Directory& directory) { return canonicalPath(directory.path) == parent; })) {
        return "plugin";
    }
    return "linked library";
}

/**
 * @brief Reads one registered key from a default-constructed instance of it.
 *
 * `settings().init()` is the call that copies a block's annotations into its meta information and stores its
 * defaults, and it needs neither a progress counter nor a thread pool, so the block is never started. Anything the
 * instance throws on the way is kept as that key's error and every other key is read as before.
 */
[[nodiscard]] Instantiation readKey(Context& context, const std::string& key, bool detailed) {
    const KeyParts parts = partsOf(key);

    Instantiation fact;
    fact.key        = key;
    fact.family     = parts.family;
    fact.name       = parts.name;
    fact.parameters = parts.parameters;

    std::shared_ptr<gr::BlockModel> instance;
    try {
        instance = context.loader.instantiate(key, {});
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
        fact.source = sourceOfFile(context, fact.file);
        return fact;
    }

    fact.file          = fileOfType(*instance);
    fact.source        = sourceOfFile(context, fact.file);
    fact.typeName      = std::string(instance->typeName());
    fact.blockCategory = std::format("{}", instance->blockCategory());
    fact.uiCategory    = std::format("{}", instance->uiCategory());
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
    collectPorts(instance->dynamicInputPorts(), "input", fact.ports);
    collectPorts(instance->dynamicOutputPorts(), "output", fact.ports);
    collectSettings(*instance, fact.settings);
    return fact;
}

struct NamedBlock {
    std::string                name;
    std::vector<Instantiation> instantiations;
};

struct Family {
    std::string             name;
    std::vector<NamedBlock> blocks;
};

struct Library {
    std::string         file;
    std::string         source;
    std::size_t         keys = 0UZ;
    std::vector<Family> families;
};

// the keys grouped by the file that registered them, by family within a file and by name within a family
[[nodiscard]] std::vector<Library> group(std::vector<Instantiation> facts) {
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<Instantiation>>>> tree;
    std::map<std::string, std::string>                                                              sources;
    for (Instantiation& fact : facts) {
        sources[fact.file] = fact.source;
        tree[fact.file][fact.family][fact.name].push_back(std::move(fact));
    }

    std::vector<Library> libraries;
    for (auto& [file, families] : tree) {
        Library library;
        library.file   = file;
        library.source = sources[file];
        for (auto& [familyName, blocks] : families) {
            Family family;
            family.name = familyName;
            for (auto& [blockName, instantiations] : blocks) {
                library.keys += instantiations.size();
                family.blocks.push_back({.name = blockName, .instantiations = std::move(instantiations)});
            }
            library.families.push_back(std::move(family));
        }
        libraries.push_back(std::move(library));
    }
    // the program's own blocks first: they are there whatever the directories hold
    std::ranges::sort(libraries, [](const Library& left, const Library& right) {
        const bool leftIsProgram  = left.source == "this program";
        const bool rightIsProgram = right.source == "this program";
        return leftIsProgram != rightIsProgram ? leftIsProgram : left.file < right.file;
    });
    return libraries;
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
    void null() { raw("null"); }

    /// a string member, null where the framework holds nothing there
    void member(std::string_view name, std::string_view value) {
        key(name);
        if (value.empty()) {
            null();
        } else {
            string(value);
        }
    }
    void flag(std::string_view name, bool value) {
        key(name);
        boolean(value);
    }
    void count(std::string_view name, std::size_t value) {
        key(name);
        number(value);
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
    json.member("direction", port.direction);
    json.member("index", port.index);
    json.member("name", port.name);
    json.member("dataType", port.dataType);
    json.member("portType", port.portType);
    json.member("domain", port.domain);
    json.flag("optional", port.optional);
    json.flag("synchronous", port.synchronous);
    json.flag("collection", port.collection);
    json.count("minSamples", port.minSamples);
    json.key("maxSamples");
    // the largest representable count is the port's way of declaring no bound, which reads as no value at all
    if (port.maxSamples == std::numeric_limits<std::size_t>::max()) {
        json.null();
    } else {
        json.number(port.maxSamples);
    }
    json.endObject();
}

void writeSetting(JsonWriter& json, const Setting& setting) {
    json.beginObject();
    json.member("name", setting.name);
    json.member("type", setting.type);
    json.key("default");
    if (setting.defaultValue.has_value()) {
        writeValue(json, *setting.defaultValue);
    } else {
        json.null();
    }
    json.member("defaultText", setting.defaultValue.has_value() ? gr::pmt::to_string(*setting.defaultValue) : std::string{});
    json.member("unit", setting.unit);
    json.member("description", setting.description);
    json.member("documentation", setting.documentation);
    json.member("enumType", setting.enumType);
    json.member("enumValues", setting.enumValues);
    json.flag("writable", setting.writable);
    json.flag("autoForwarded", setting.autoForwarded);
    json.key("visible");
    if (setting.visible.has_value()) {
        json.boolean(*setting.visible);
    } else {
        json.null();
    }
    json.endObject();
}

// one instantiation; `description`, `ports` and `settings` are null where the key was listed rather than read in
// detail, and an empty array where the block declares none
void writeInstantiation(JsonWriter& json, const Instantiation& fact) {
    json.beginObject();
    json.member("key", fact.key);
    json.member("name", fact.name);
    json.member("family", fact.family);
    json.member("parameters", fact.parameters);
    json.member("file", fact.file);
    json.member("source", fact.source);
    json.member("typeName", fact.typeName);
    json.member("blockCategory", fact.blockCategory);
    json.member("uiCategory", fact.uiCategory);
    json.member("error", fact.error);
    json.member("settingsError", fact.settingsError);
    json.key("description");
    if (fact.detailed && !fact.description.empty()) {
        json.string(fact.description);
    } else {
        json.null();
    }
    json.key("ports");
    if (fact.detailed) {
        json.beginArray();
        for (const Port& port : fact.ports) {
            writePort(json, port);
        }
        json.endArray();
    } else {
        json.null();
    }
    json.key("settings");
    if (fact.detailed) {
        json.beginArray();
        for (const Setting& setting : fact.settings) {
            writeSetting(json, setting);
        }
        json.endArray();
    } else {
        json.null();
    }
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

// Rows printed as columns two spaces apart, with no trailing space on a line: a row whose last cells are empty ends
// where its last filled cell does. Without headers the rows are printed as they are, which is what a listing that
// names its columns in the line above it needs.
void printTable(std::string_view indent, std::span<const std::string_view> headers, std::span<const std::vector<std::string>> rows) {
    std::size_t columns = headers.size();
    for (const std::vector<std::string>& row : rows) {
        columns = std::max(columns, row.size());
    }
    std::vector<std::size_t> widths(columns, 0UZ);
    for (std::size_t i = 0UZ; i < headers.size(); ++i) {
        widths[i] = headers[i].size();
    }
    for (const std::vector<std::string>& row : rows) {
        for (std::size_t i = 0UZ; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }
    auto line = [indent, &widths](const auto& cells) {
        std::string text(indent);
        for (std::size_t i = 0UZ; i < cells.size(); ++i) {
            const std::string_view cell(cells[i]);
            text += cell;
            if (i + 1UZ < cells.size()) {
                text.append(widths[i] - cell.size() + 2UZ, ' ');
            }
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        std::println("{}", text);
    };
    if (!headers.empty()) {
        line(headers);
    }
    for (const std::vector<std::string>& row : rows) {
        line(row);
    }
}

// one `name  value` line per fact, the names padded to the widest
void printFacts(std::string_view indent, const std::vector<std::pair<std::string, std::string>>& facts) {
    std::size_t width = 0UZ;
    for (const auto& [name, value] : facts) {
        width = std::max(width, name.size());
    }
    for (const auto& [name, value] : facts) {
        std::println("{}{}{}  {}", indent, name, std::string(width - name.size(), ' '), value);
    }
}

[[nodiscard]] std::string sampleBound(std::size_t count) { return count == std::numeric_limits<std::size_t>::max() ? "unbounded" : std::to_string(count); }

[[nodiscard]] std::string yesNo(bool value) { return value ? "yes" : "no"; }

// A block's own text, one line at a time under `indent`, with a blank line before it and without the blank lines the
// annotation begins and ends with. A block that declares nothing prints nothing, not an empty paragraph.
void printDescription(std::string_view description, std::string_view indent) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0UZ; start <= description.size();) {
        const std::size_t end = std::min(description.find('\n', start), description.size());
        lines.push_back(description.substr(start, end - start));
        start = end + 1UZ;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::size_t first = 0UZ;
    while (first < lines.size() && lines[first].empty()) {
        ++first;
    }
    if (first >= lines.size()) {
        return;
    }
    std::print("\n");
    for (std::size_t i = first; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            std::print("\n");
        } else {
            std::println("{}{}", indent, lines[i]);
        }
    }
}

void printBlockDetail(const Instantiation& fact, std::string_view indent) {
    std::vector<std::pair<std::string, std::string>> facts;
    facts.emplace_back("file", fact.file.empty() ? fact.source : std::format("{} ({})", fact.file, fact.source));
    facts.emplace_back("family", fact.family.empty() ? "(none)" : fact.family);
    if (!fact.error.empty()) {
        facts.emplace_back("error", fact.error);
        printFacts(indent, facts);
        return;
    }
    facts.emplace_back("type name", fact.typeName);
    facts.emplace_back("category", std::format("{}, UI {}", fact.blockCategory, fact.uiCategory));
    if (!fact.settingsError.empty()) {
        facts.emplace_back("settings error", fact.settingsError);
    }
    printFacts(indent, facts);

    printDescription(fact.description, indent);

    std::print("\n");
    if (fact.ports.empty()) {
        std::println("{}no stream ports", indent);
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(fact.ports.size());
        for (const Port& port : fact.ports) {
            rows.push_back({port.direction, port.index, port.name, port.dataType, port.portType, yesNo(port.synchronous), yesNo(port.optional), yesNo(port.collection), port.domain, std::format("{} .. {}", sampleBound(port.minSamples), sampleBound(port.maxSamples))});
        }
        constexpr std::array<std::string_view, 10> headers{"direction", "index", "port", "data type", "port type", "sync", "optional", "collection", "domain", "samples per call"};
        printTable(indent, headers, rows);
    }

    std::print("\n");
    if (fact.settings.empty()) {
        std::println("{}no settings", indent);
        return;
    }
    std::vector<std::vector<std::string>> rows;
    rows.reserve(fact.settings.size());
    for (const Setting& setting : fact.settings) {
        std::string type = setting.type.empty() ? "(not stored)" : setting.type;
        if (!setting.enumValues.empty()) {
            type += std::format(" (enum {}: {})", setting.enumType, setting.enumValues);
        }
        rows.push_back({setting.name, type, setting.defaultValue.has_value() ? gr::pmt::to_string(*setting.defaultValue) : "(not stored)", setting.unit, yesNo(setting.writable), yesNo(setting.visible.value_or(true)), setting.description});
    }
    constexpr std::array<std::string_view, 7> headers{"setting", "type", "default", "unit", "writable", "visible", "description"};
    printTable(indent, headers, rows);
}

void reportVersionAsJson(const Context& context) {
    JsonWriter json;
    json.beginObject();
    json.count("schema", 1UZ);
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
    json.member("compilerPath", CXX_COMPILER_PATH);
    json.member("installPrefix", GR_TOOLS_INSTALLED_PREFIX);
    json.member("installedPluginDirectory", GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    json.member("dataCacheDirectory", GR_DATA_CACHE_DIR);
    json.member("program", context.program);
    json.key("pluginDirectoriesEnvironment");
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        json.string(environment);
    } else {
        json.null();
    }
    json.endObject();

    json.key("directories");
    json.beginArray();
    for (const Directory& directory : context.directories) {
        json.beginObject();
        json.member("path", directory.path);
        json.member("source", directory.source);
        json.flag("searched", directory.present);
        json.endObject();
    }
    json.endArray();

    json.key("libraries");
    json.beginArray();
    for (const FileFact& file : context.files) {
        json.beginObject();
        json.member("file", file.file);
        json.member("kind", file.kind);
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
        json.member("author", plugin.author);
        json.member("license", plugin.license);
        json.member("version", plugin.version);
        json.count("blocks", plugin.blocks);
        json.count("schedulers", plugin.schedulers);
        json.endObject();
    }
    json.endArray();

    json.key("schedulers");
    json.beginArray();
    for (const std::string& scheduler : context.schedulers) {
        json.string(scheduler);
    }
    json.endArray();

    writeTotals(json, context.totals);
    json.endObject();
    std::println("{}", json.text);
}

void reportVersion(const Context& context) {
    std::println("GNU Radio 4 {}", GR_TOOLS_CORE_VERSION);
    std::print("\n");

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
    facts.emplace_back("plugin directory", GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    facts.emplace_back("data cache", GR_DATA_CACHE_DIR);
    facts.emplace_back("this program", context.program.empty() ? "(not named by the linker)" : context.program);
    const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES");
    facts.emplace_back("GNURADIO4_PLUGIN_DIRECTORIES", environment == nullptr ? "not set" : environment);
    printFacts("  ", facts);

    std::print("\n");
    std::println("directories searched, in order");
    std::vector<std::vector<std::string>> directoryRows;
    directoryRows.reserve(context.directories.size());
    for (const Directory& directory : context.directories) {
        directoryRows.push_back({directory.path, directory.source, directory.present ? "searched" : "not a directory"});
    }
    constexpr std::array<std::string_view, 3> directoryHeaders{"directory", "named by", "state"};
    printTable("  ", directoryHeaders, directoryRows);

    if (!context.files.empty()) {
        std::print("\n");
        std::println("files");
        std::vector<std::vector<std::string>> fileRows;
        fileRows.reserve(context.files.size());
        for (const FileFact& file : context.files) {
            const std::string brought = file.reason.empty() ? std::format("{} block and {} scheduler registration(s)", file.blockRegistrations, file.schedulerRegistrations) : file.reason;
            fileRows.push_back({file.file, file.kind, brought});
        }
        constexpr std::array<std::string_view, 3> fileHeaders{"file", "kind", "what it brought"};
        printTable("  ", fileHeaders, fileRows);
    }

    if (!context.plugins.empty()) {
        std::print("\n");
        std::println("plugins");
        std::vector<std::vector<std::string>> pluginRows;
        pluginRows.reserve(context.plugins.size());
        for (const PluginFact& plugin : context.plugins) {
            pluginRows.push_back({plugin.name, plugin.author, plugin.license, plugin.version, std::to_string(plugin.blocks), std::to_string(plugin.schedulers)});
        }
        constexpr std::array<std::string_view, 6> pluginHeaders{"name", "author", "license", "version", "blocks", "schedulers"};
        printTable("  ", pluginHeaders, pluginRows);
    }

    if (!context.schedulers.empty()) {
        std::print("\n");
        std::println("schedulers");
        for (const std::string& scheduler : context.schedulers) {
            std::println("  {}", scheduler);
        }
    }

    std::print("\n");
    std::println("totals");
    printFacts("  ", {
                         {"block keys", std::to_string(context.totals.blockKeys)},
                         {"block families", std::to_string(context.totals.blockFamilies)},
                         {"scheduler keys", std::to_string(context.totals.schedulerKeys)},
                         {"block libraries", std::to_string(context.totals.blockLibraries)},
                         {"plugins", std::to_string(context.totals.plugins)},
                         {"files not loaded", std::to_string(context.totals.filesNotLoaded)},
                         {"files not opened", std::to_string(context.totals.filesNotOpened)},
                     });
}

void reportBlocks(Context& context, bool verbose, bool asJson) {
    std::vector<Instantiation> facts;
    facts.reserve(context.keys.size());
    for (const std::string& key : context.keys) {
        facts.push_back(readKey(context, key, verbose));
    }
    const std::vector<Library> libraries = group(std::move(facts));

    if (asJson) {
        JsonWriter json;
        json.beginObject();
        json.count("schema", 1UZ);
        json.member("command", "blocks");
        json.flag("verbose", verbose);
        json.key("libraries");
        json.beginArray();
        for (const Library& library : libraries) {
            json.beginObject();
            json.member("file", library.file);
            json.member("source", library.source);
            json.count("blockKeys", library.keys);
            json.key("families");
            json.beginArray();
            for (const Family& family : library.families) {
                json.beginObject();
                json.member("name", family.name);
                json.key("blocks");
                json.beginArray();
                for (const NamedBlock& block : family.blocks) {
                    json.beginObject();
                    json.member("name", block.name);
                    json.key("instantiations");
                    json.beginArray();
                    for (const Instantiation& fact : block.instantiations) {
                        writeInstantiation(json, fact);
                    }
                    json.endArray();
                    json.endObject();
                }
                json.endArray();
                json.endObject();
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();
        writeTotals(json, context.totals);
        json.endObject();
        std::println("{}", json.text);
        return;
    }

    for (const Library& library : libraries) {
        std::println("{} ({}, {} key(s))", library.file.empty() ? "not attributed" : library.file, library.source, library.keys);
        for (const Family& family : library.families) {
            std::print("\n");
            std::println("  {}", family.name.empty() ? "(no namespace)" : family.name);
            if (verbose) {
                for (const NamedBlock& block : family.blocks) {
                    for (const Instantiation& fact : block.instantiations) {
                        std::print("\n");
                        std::println("    {}", fact.key);
                        printBlockDetail(fact, "      ");
                    }
                }
                continue;
            }
            std::vector<std::vector<std::string>> rows;
            rows.reserve(family.blocks.size());
            for (const NamedBlock& block : family.blocks) {
                std::string instantiations;
                for (const Instantiation& fact : block.instantiations) {
                    if (fact.parameters.empty()) {
                        continue;
                    }
                    instantiations += instantiations.empty() ? "" : " ";
                    instantiations += std::format("<{}>", fact.parameters);
                }
                rows.push_back({block.name, std::move(instantiations)});
            }
            // the block's own name, and the type parameters of its instantiations after it; the family line above
            // names what the columns are
            printTable("    ", {}, rows);
        }
        std::print("\n");
    }
    std::println("{} block key(s), {} family(ies), {} file(s)", context.totals.blockKeys, context.totals.blockFamilies, libraries.size());
}

// 0 when the name selected something, 1 when nothing of that name is registered
[[nodiscard]] int reportBlock(Context& context, const std::string& wanted, bool asJson) {
    std::vector<Instantiation> selected;
    for (const std::string& key : context.keys) {
        const KeyParts    parts     = partsOf(key);
        const std::string qualified = parts.family.empty() ? parts.name : parts.family + "::" + parts.name;
        if (key != wanted && parts.name != wanted && qualified != wanted) {
            continue;
        }
        selected.push_back(readKey(context, key, true));
    }
    if (selected.empty()) {
        std::println(stderr, "{}: no block named {} is registered", kProgram, wanted);
        return 1;
    }

    if (asJson) {
        JsonWriter json;
        json.beginObject();
        json.count("schema", 1UZ);
        json.member("command", "block");
        json.member("name", wanted);
        json.key("instantiations");
        json.beginArray();
        for (const Instantiation& fact : selected) {
            writeInstantiation(json, fact);
        }
        json.endArray();
        json.endObject();
        std::println("{}", json.text);
        return 0;
    }

    for (std::size_t i = 0UZ; i < selected.size(); ++i) {
        if (i != 0UZ) {
            std::print("\n");
        }
        std::println("{}", selected[i].key);
        printBlockDetail(selected[i], "  ");
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

    std::vector<Directory> directories = searchDirectories(options.pluginDirectories);
    bool                   searchable  = true;
    for (const Directory& directory : directories) {
        if (directory.source == "command line" && !directory.present) {
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
        context.files.push_back({.file = library.file, .kind = "block library", .blockRegistrations = library.nBlockRegistrations, .schedulerRegistrations = library.nSchedulerRegistrations, .reason = {}});
    }
    for (const auto& [file, reason] : loader.failedPlugins()) {
        context.files.push_back({.file = file, .kind = "not loaded", .blockRegistrations = 0UZ, .schedulerRegistrations = 0UZ, .reason = reason});
    }
    for (const std::string& file : loader.skippedFiles()) {
        context.files.push_back({.file = file, .kind = "not opened", .blockRegistrations = 0UZ, .schedulerRegistrations = 0UZ, .reason = "its name only reads as a shared object"});
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
        if (options.json) {
            reportVersionAsJson(context);
        } else {
            reportVersion(context);
        }
        return 0;
    case Command::Blocks: reportBlocks(context, options.verbose, options.json); return 0;
    case Command::Block: return reportBlock(context, options.blockName, options.json);
    }
    return 0;
}
