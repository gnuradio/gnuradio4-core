#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/**
 * grinfo, driven as the program a caller runs.
 *
 * The tool's contract is its exit status and what it prints, and neither is visible from inside the process, so
 * every case here runs the built executable over core's own test plugins and test block libraries. The cases cover:
 * - the framework report
 * - the block listing, and one block in detail
 * - the scheduler listing
 * - scheduler libraries refused at an earlier plugin ABI version or at none
 * - a plugin refused for its plugin ABI version
 * - the attached units of the device families a block library and a plugin register
 * - a name nothing is registered under
 * - a command line that cannot be used
 * - the Markdown and HTML documents of each command, a value with markup characters in them, and the ids of their
 *   sections
 *
 * The shape of the report is part of that contract - no line wider than a terminal, one entry per block however many
 * instantiations it has, and a JSON document on standard output that carries no null - so each is pinned here too.
 */
namespace qa_grinfo {

#ifdef _WIN32
constexpr auto openPipe  = _popen;
constexpr auto closePipe = _pclose;
#else
constexpr auto openPipe  = popen;
constexpr auto closePipe = pclose;
#endif

struct Result {
    int         exitCode = -1;
    std::string output; // what the run wrote to the collected streams, in the order it wrote it
};

enum class Streams { both, standardOutput };

// runs the tool with `arguments` and collects what it wrote to `streams` and the status it exited with
[[nodiscard]] Result run(const std::vector<std::string>& arguments, Streams streams = Streams::both) {
    std::string command = std::format("\"{}\"", GR_TOOLS_GRINFO);
    for (const std::string& argument : arguments) {
        command += std::format(" \"{}\"", argument);
    }
#ifdef _WIN32
    command += streams == Streams::both ? " 2>&1" : " 2>NUL";
#else
    command += streams == Streams::both ? " 2>&1" : " 2>/dev/null";
#endif

    Result     result;
    std::FILE* pipe = openPipe(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    std::array<char, 4096UZ> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }
    const int status = closePipe(pipe);
#ifdef _WIN32
    result.exitCode = status;
#else
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return result;
}

// sets or clears one environment variable for the programs a case runs, and puts back the value it found
class EnvironmentVariable {
    std::string                _name;
    std::optional<std::string> _found;

public:
    EnvironmentVariable(std::string name, const std::optional<std::string>& value) : _name(std::move(name)) {
        if (const char* found = std::getenv(_name.c_str()); found != nullptr) {
            _found = found;
        }
        if (value.has_value()) {
            ::setenv(_name.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }
    ~EnvironmentVariable() {
        if (_found.has_value()) {
            ::setenv(_name.c_str(), _found->c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }
    EnvironmentVariable(const EnvironmentVariable&)            = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;
};

// whether `text` is exactly one JSON document. The document opens the text. Every bracket outside a string closes in
// order. Only white space follows the document.
[[nodiscard]] bool isOneJsonDocument(std::string_view text) {
    std::size_t depth    = 0UZ;
    bool        inString = false;
    bool        escaped  = false;
    bool        opened   = false;
    for (const char character : text) {
        const bool outside = depth == 0UZ && !inString;
        if (outside && character != ' ' && character != '\n' && character != '\r' && character != '\t' && (opened || (character != '{' && character != '['))) {
            return false;
        }
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        switch (character) {
        case '"': inString = true; break;
        case '{':
        case '[':
            ++depth;
            opened = true;
            break;
        case '}':
        case ']':
            if (depth == 0UZ) {
                return false;
            }
            --depth;
            break;
        default: break;
        }
    }
    return opened && depth == 0UZ && !inString;
}

// the report is written for a terminal, so this is the number every one of its lines has to stay under
constexpr std::size_t kWidth = 80UZ;

[[nodiscard]] std::size_t widestLine(std::string_view text) {
    std::size_t widest = 0UZ;
    for (std::size_t start = 0UZ; start <= text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        widest                = std::max(widest, end - start);
        start                 = end + 1UZ;
    }
    return widest;
}

[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle) {
    std::size_t found = 0UZ;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

// `text` with each run of spaces and line breaks replaced by one space; a value the report wrapped reads as one line
[[nodiscard]] std::string withSingleSpaces(std::string_view text) {
    std::string joined;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\n';
        if (!space) {
            joined += c;
        } else if (!joined.empty() && joined.back() != ' ') {
            joined += ' ';
        }
    }
    return joined;
}

// The targets of every link that `opening` starts and `closing` ends, in the order they appear: `](#` and `)` for a
// Markdown document, `href="#` and `"` for an HTML page.
[[nodiscard]] std::vector<std::string> linkTargets(std::string_view text, std::string_view opening, std::string_view closing) {
    std::vector<std::string> targets;
    for (std::size_t at = text.find(opening); at != std::string_view::npos; at = text.find(opening, at + opening.size())) {
        const std::size_t start = at + opening.size();
        const std::size_t end   = text.find(closing, start);
        if (end == std::string_view::npos) {
            break;
        }
        targets.emplace_back(text.substr(start, end - start));
    }
    return targets;
}

// The cells of one Markdown table row, split at each `|` that no backslash escapes, without the spaces around them. The
// row opens and closes with a `|`.
[[nodiscard]] std::vector<std::string> cellsOf(std::string_view row) {
    std::vector<std::string> pieces(1UZ);
    for (std::size_t i = 0UZ; i < row.size(); ++i) {
        if (row[i] == '|') {
            pieces.emplace_back();
            continue;
        }
        pieces.back() += row[i];
        if (row[i] == '\\' && i + 1UZ < row.size()) {
            pieces.back() += row[++i];
        }
    }
    std::vector<std::string> cells;
    for (std::size_t i = 1UZ; i + 1UZ < pieces.size(); ++i) {
        const std::size_t first = pieces[i].find_first_not_of(' ');
        cells.push_back(first == std::string::npos ? std::string{} : pieces[i].substr(first, pieces[i].find_last_not_of(' ') - first + 1UZ));
    }
    return cells;
}

// The text a CommonMark renderer shows for the source of one table cell, or nothing when the source holds a character
// that acts as markup there. A backslash before ASCII punctuation makes that character literal, and a literal `&`
// starts no entity. An unescaped `*`, backtick, `[`, `]`, `<`, `>`, `&`, `~` or `|`, or an `_` that does not stand
// between two letters or digits, opens emphasis, a code span, a link, raw HTML, an entity, a strikethrough or a cell.
[[nodiscard]] std::optional<std::string> renderedCell(std::string_view source) {
    constexpr std::string_view kMarkup         = "*`[]<>&~|";
    const auto                 isWordCharacter = [source](std::size_t i) { return i < source.size() && std::isalnum(static_cast<unsigned char>(source[i])) != 0; };
    std::string                shown;
    for (std::size_t i = 0UZ; i < source.size(); ++i) {
        const char c = source[i];
        if (c == '\\' && i + 1UZ < source.size() && std::ispunct(static_cast<unsigned char>(source[i + 1UZ])) != 0) {
            shown += source[++i];
            continue;
        }
        const bool intraword = c == '_' && i > 0UZ && isWordCharacter(i - 1UZ) && isWordCharacter(i + 1UZ);
        if (kMarkup.contains(c) || (c == '_' && !intraword)) {
            return std::nullopt;
        }
        shown += c;
    }
    return shown;
}

// One navigation link: the text it shows and the fragment it targets.
struct Link {
    std::string label;
    std::string target;
};

// The links of the list that opens a document, in order: `- [label](#target)` lines in Markdown, `<li><a
// href="#target">label</a>` items in HTML.
[[nodiscard]] std::vector<Link> navigationLinks(std::string_view text, bool html) {
    const std::string_view opening   = html ? "<li><a href=\"#" : "\n- [";
    const std::string_view separator = html ? "\">" : "](#";
    const std::string_view closing   = html ? "</a>" : ")";
    std::vector<Link>      links;
    for (std::size_t at = text.find(opening); at != std::string_view::npos; at = text.find(opening, at + opening.size())) {
        const std::size_t start  = at + opening.size();
        const std::size_t middle = text.find(separator, start);
        const std::size_t end    = text.find(closing, middle);
        if (middle == std::string_view::npos || end == std::string_view::npos) {
            break;
        }
        const std::string first(text.substr(start, middle - start));
        const std::string second(text.substr(middle + separator.size(), end - middle - separator.size()));
        links.push_back(html ? Link{.label = second, .target = first} : Link{.label = first, .target = second});
    }
    return links;
}

// The heading of the section whose id is `id`: the line after `<a id="id"></a>` in Markdown, without its `#` marks,
// and the text of the heading element that carries the id in HTML. Nothing when no section carries the id.
[[nodiscard]] std::optional<std::string> headingWithId(std::string_view text, std::string_view id, bool html) {
    if (html) {
        const std::string tag   = std::format(" id=\"{}\">", id);
        const std::size_t start = text.find(tag);
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        const std::size_t from = start + tag.size();
        return std::string(text.substr(from, text.find("</h", from) - from));
    }
    const std::string anchor = std::format("<a id=\"{}\"></a>\n\n", id);
    const std::size_t start  = text.find(anchor);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t from = text.find_first_not_of('#', start + anchor.size()) + 1UZ;
    return std::string(text.substr(from, text.find('\n', from) - from));
}

// the first line of `text` that starts with `prefix`, or nothing
[[nodiscard]] std::optional<std::string> lineStartingWith(std::string_view text, std::string_view prefix) {
    for (std::size_t start = 0UZ; start < text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        if (text.substr(start, end - start).starts_with(prefix)) {
            return std::string(text.substr(start, end - start));
        }
        start = end + 1UZ;
    }
    return std::nullopt;
}

// whether a Markdown table in `text` has a row whose cells a renderer shows as `shown`
[[nodiscard]] bool hasRenderedRow(std::string_view text, const std::vector<std::string>& shown) {
    for (std::size_t start = 0UZ; start < text.size();) {
        const std::size_t              end   = std::min(text.find('\n', start), text.size());
        const std::string_view         line  = text.substr(start, end - start);
        const std::vector<std::string> cells = line.starts_with('|') ? cellsOf(line) : std::vector<std::string>{};
        if (cells.size() == shown.size() && std::ranges::equal(cells, shown, [](const std::string& cell, const std::string& expected) { return renderedCell(cell) == expected; })) {
            return true;
        }
        start = end + 1UZ;
    }
    return false;
}

// what an HTML page may not carry if it is to render with no network: a script, a style sheet or an image loaded
// from elsewhere, or any address
constexpr std::array<std::string_view, 5> kExternalReferences{"<script", "<link", "<img", "http", "@import"};

// what a reader sees of a path the report had to cut: the tool keeps a path's tail, which is what tells one
// directory from another
[[nodiscard]] std::string_view tailOf(std::string_view path, std::size_t count) { return path.size() <= count ? path : path.substr(path.size() - count); }

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
// the two directories core's own tests build: one of plugins, one of shared objects that register blocks without
// being plugins
[[nodiscard]] std::vector<std::string> overTestDirectories(const std::vector<std::string>& arguments) {
    std::vector<std::string> all(arguments);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_CORE_TEST_PLUGINS);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_TEST_BLOCK_LIBRARY);
    return all;
}

// the directory of the three shared objects that register a scheduler without being plugins. They are built at this
// plugin ABI version, at an earlier version and without a version.
[[nodiscard]] std::vector<std::string> overSchedulerLibraries(const std::vector<std::string>& arguments) {
    std::vector<std::string> all(arguments);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_TEST_SCHEDULER_LIBRARY);
    return all;
}
#endif

#ifdef GR_TOOLS_TEST_DEVICE_LISTING
// the directory of a block library and a plugin that register device listings of invented units
[[nodiscard]] std::vector<std::string> overDeviceListings(const std::vector<std::string>& arguments) {
    std::vector<std::string> all(arguments);
    all.emplace_back("--plugin-dir");
    all.emplace_back(GR_TOOLS_TEST_DEVICE_LISTING);
    return all;
}

// whether `text` has a line that names `name` and then `value`, as the text report prints a unit's selector and label
[[nodiscard]] bool hasFact(std::string_view text, std::string_view name, std::string_view value) {
    const std::optional<std::string> line = lineStartingWith(text, std::format("  {} ", name));
    return line.has_value() && withSingleSpaces(*line) == std::format("{} {}", name, value);
}

// the text of the JSON object that holds `needle`, braces included; empty when there is none
[[nodiscard]] std::string_view enclosingJsonObject(std::string_view text, std::string_view needle) {
    const std::size_t at = text.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t begin = text.rfind('{', at);
    const std::size_t end   = text.find('}', at);
    return begin == std::string_view::npos || end == std::string_view::npos ? std::string_view{} : text.substr(begin, end - begin + 1UZ);
}
#endif

} // namespace qa_grinfo

const boost::ut::suite<"GrInfo"> grInfoTests = [] {
    using namespace boost::ut;
    using namespace qa_grinfo;

    "a command line that cannot be used is refused with the usage text"_test = [] {
        const Result unknownOption = run({"--no-such-option"});
        expect(eq(unknownOption.exitCode, 2)) << unknownOption.output;
        expect(unknownOption.output.contains("unknown option '--no-such-option'")) << unknownOption.output;
        expect(unknownOption.output.contains("Usage: grinfo")) << unknownOption.output;

        const Result unknownCommand = run({"blocks-please"});
        expect(eq(unknownCommand.exitCode, 2)) << unknownCommand.output;
        expect(unknownCommand.output.contains("unknown command 'blocks-please'")) << unknownCommand.output;

        const Result withoutName = run({"block"});
        expect(eq(withoutName.exitCode, 2)) << withoutName.output;
        expect(withoutName.output.contains("block needs the name of a block")) << withoutName.output;

        const Result withoutValue = run({"--plugin-dir"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--plugin-dir needs a value")) << withoutValue.output;
    };

    "a command line that selects two forms is refused with a line naming both"_test = [] {
        const Result jsonAndMarkdown = run({"--json", "--markdown"});
        expect(eq(jsonAndMarkdown.exitCode, 2)) << jsonAndMarkdown.output;
        expect(jsonAndMarkdown.output.contains("--json and --markdown cannot be combined")) << jsonAndMarkdown.output;

        const Result markdownAndHtml = run({"blocks", "--markdown", "--html"});
        expect(eq(markdownAndHtml.exitCode, 2)) << markdownAndHtml.output;
        expect(markdownAndHtml.output.contains("--markdown and --html cannot be combined")) << markdownAndHtml.output;
    };

    "a plugin directory that cannot be searched ends the run"_test = [] {
        const Result missing = run({"--plugin-dir", "/gnuradio4-plugin-directory-that-does-not-exist"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("could not be searched")) << missing.output;
    };

    "--help is not an error"_test = [] {
        const Result help = run({"--help"});
        expect(eq(help.exitCode, 0)) << help.output;
        expect(help.output.contains("Usage: grinfo")) << help.output;
        expect(help.output.contains("schedulers")) << "the command that lists the schedulers" << help.output;
    };

    "the JSON check takes one document with nothing before or after it"_test = [] {
        expect(isOneJsonDocument("{\"a\": [1, \"}\"]}\n"));
        expect(!isOneJsonDocument("warning: a line\n{\"a\": 1}\n")) << "a line ahead of the document";
        expect(!isOneJsonDocument("{\"a\": 1}\n{\"b\": 2}\n")) << "a second document";
        expect(!isOneJsonDocument("{\"a\": 1}\ntrailing\n")) << "a line after the document";
        expect(!isOneJsonDocument("{\"a\": 1")) << "a document left open";
    };

    // The text report cuts a long path to its tail and wraps a long value across lines. The text arms join the lines and
    // look for the path's last component; the JSON arms look for the whole path.
    "version reports the data cache directory in use, the one GR_DATA_CACHE_DIR names when it is set"_test = [] {
        const std::string named        = (std::filesystem::temp_directory_path() / "gr4_qa_grinfo_cache").string();
        const std::string fromVariable = "gr4_qa_grinfo_cache, from GR_DATA_CACHE_DIR";
        {
            const EnvironmentVariable unset("GR_DATA_CACHE_DIR", std::nullopt);
            const Result              compiled = run({"version", "--json"});
            expect(eq(compiled.exitCode, 0)) << compiled.output;
            expect(compiled.output.contains("\"dataCacheOrigin\": \"")) << "the instrument: the report names an origin" << compiled.output;
            expect(!compiled.output.contains("\"dataCacheOrigin\": \"environment\"")) << "without the variable the origin is not the environment" << compiled.output;
            expect(!compiled.output.contains(named)) << compiled.output;

            const Result text = run({"version"});
            expect(eq(text.exitCode, 0)) << text.output;
            expect(text.output.contains("data cache")) << text.output;
            expect(!withSingleSpaces(text.output).contains(fromVariable)) << "without the variable the text names no variable" << text.output;
        }

        const EnvironmentVariable variable("GR_DATA_CACHE_DIR", named);
        const Result              json = run({"version", "--json"});
        expect(eq(json.exitCode, 0)) << json.output;
        expect(json.output.contains(std::format("\"dataCacheDirectory\": \"{}\"", named))) << json.output;
        expect(json.output.contains("\"dataCacheOrigin\": \"environment\"")) << json.output;

        const Result text = run({"version"});
        expect(eq(text.exitCode, 0)) << text.output;
        expect(withSingleSpaces(text.output).contains(fromVariable)) << text.output;
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "version names the directories searched and what they held"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains(tailOf(GR_TOOLS_CORE_TEST_PLUGINS, 30UZ))) << "the directory it was given" << version.output;
        expect(version.output.contains(tailOf(GR_TOOLS_TEST_BLOCK_LIBRARY, 30UZ))) << "the directory it was given" << version.output;
        expect(version.output.contains("block-library")) << "a shared object that registers without being a plugin" << version.output;
        expect(version.output.contains("block_library")) << "and the file it is" << version.output;
        expect(version.output.contains("not-loaded")) << "the plugin whose ABI version does not match" << version.output;
        expect(version.output.contains("Good Math Plugin")) << "a plugin that did load" << version.output;
        expect(version.output.contains("block keys")) << version.output;
    };

    "a library is listed by its own name under the directory that held it"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains("\n    libblock_library.so")) << "the file by its name alone, indented under its directory" << version.output;
        expect(eq(occurrences(version.output, GR_TOOLS_TEST_BLOCK_LIBRARY "/libblock_library.so"), 0UZ)) << "and not by a path repeated on every line" << version.output;
    };

    "version --json is one document carrying the shape a reader relies on"_test = [] {
        const Result version = run(overTestDirectories({"version", "--json"}), Streams::standardOutput);
        expect(eq(version.exitCode, 0)) << version.output;
        expect(isOneJsonDocument(version.output)) << version.output;
        expect(version.output.contains("\"schema\": 2")) << version.output;
        for (const std::string_view key : {"\"framework\"", "\"directories\"", "\"libraries\"", "\"plugins\"", "\"schedulers\"", "\"totals\"", "\"blockKeys\""}) {
            expect(version.output.contains(key)) << key << version.output;
        }
        expect(version.output.contains(GR_TOOLS_CORE_TEST_PLUGINS)) << "a path the text report cut is whole here" << version.output;
        expect(version.output.contains("\"origin\": \"option\"")) << "the directory came from the command line" << version.output;
        expect(version.output.contains("\"kind\": \"block-library\"")) << "the kinds are an enumeration" << version.output;
    };

    "blocks lists a block under the file that registered it and under its family"_test = [] {
        const Result blocks = run(overTestDirectories({"blocks"}));
        expect(eq(blocks.exitCode, 0)) << blocks.output;
        expect(blocks.output.contains("block_library")) << "the file the block came from" << blocks.output;
        expect(blocks.output.contains("gr::testing")) << "its family" << blocks.output;
        expect(blocks.output.contains("LibraryDoubler")) << "its name" << blocks.output;
        expect(blocks.output.contains("good")) << "the family of the test plugins' blocks" << blocks.output;
        expect(blocks.output.contains("fixed_source")) << "a block a plugin brought" << blocks.output;
        expect(blocks.output.contains("<float32>")) << "the instantiations collapsed onto the block's line" << blocks.output;
    };

    "blocks --json is one document shaped by library, family and block"_test = [] {
        const Result blocks = run(overTestDirectories({"blocks", "--json"}), Streams::standardOutput);
        expect(eq(blocks.exitCode, 0)) << blocks.output;
        expect(isOneJsonDocument(blocks.output)) << blocks.output;
        expect(blocks.output.contains("\"schema\": 2")) << blocks.output;
        for (const std::string_view key : {"\"libraries\"", "\"families\"", "\"instantiations\"", "\"LibraryDoubler\""}) {
            expect(blocks.output.contains(key)) << key << blocks.output;
        }
    };

    "block on a bare name prints its ports and its settings"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(block.output.contains("gr::testing::LibraryDoubler")) << block.output;
        expect(block.output.contains("block_library")) << "the file it came from" << block.output;
        expect(block.output.contains("input")) << "its input port" << block.output;
        expect(block.output.contains("output")) << "its output port" << block.output;
        expect(block.output.contains("float32")) << "the type the ports carry" << block.output;
        expect(block.output.contains("extra_gain")) << "its setting" << block.output;
        expect(block.output.contains("dB")) << "the unit the annotation carries" << block.output;
        expect(block.output.contains("doubles its input")) << "the description the block declares" << block.output;
    };

    "the settings the framework declares on every block are behind an option"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(!block.output.contains("unique_name")) << "a framework setting is not in the block's own table" << block.output;
        expect(block.output.contains("--all-settings")) << "and the report says where it is" << block.output;

        const Result all = run(overTestDirectories({"block", "LibraryDoubler", "--all-settings"}));
        expect(eq(all.exitCode, 0)) << all.output;
        expect(all.output.contains("unique_name")) << all.output;
        expect(all.output.contains("extra_gain")) << "the block's own settings are still there" << all.output;
    };

    "a block of several instantiations is one entry written in its type parameters"_test = [] {
        const Result block = run(overTestDirectories({"block", "convert"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(block.output.contains("good::convert<T1, T2>")) << "named by its parameters" << block.output;
        expect(block.output.contains("<float64, float32>")) << "which are listed once" << block.output;
        expect(block.output.contains("<float32, float64>")) << block.output;
        expect(eq(occurrences(block.output, "direction"), 1UZ)) << "one port table for every instantiation" << block.output;
        expect(block.output.contains("T1")) << "the input port carries the first parameter" << block.output;
        expect(block.output.contains("T2")) << "and the output port the second" << block.output;
    };

    "block --json carries the settings with their defaults typed"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler", "--json"}), Streams::standardOutput);
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"command\": \"block\"")) << block.output;
        expect(block.output.contains("\"query\": \"LibraryDoubler\"")) << block.output;
        expect(block.output.contains("\"name\": \"extra_gain\"")) << block.output;
        expect(block.output.contains("\"default\": 1")) << "a number is a number, not a string" << block.output;
        expect(block.output.contains("\"unit\": \"dB\"")) << block.output;
        expect(block.output.contains("\"framework\": false")) << "a setting of the block's own" << block.output;
        expect(!block.output.contains(": null")) << "a field the framework holds nothing in is left out" << block.output;
    };

    "block --json keys every instantiation in full, which the text report does not"_test = [] {
        const Result block = run(overTestDirectories({"block", "convert", "--json"}), Streams::standardOutput);
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"key\": \"good::convert<float64, float32>\"")) << block.output;
        expect(block.output.contains("\"key\": \"good::convert<float32, float64>\"")) << block.output;
        expect(block.output.contains("\"parameters\": [")) << "split into one entry per parameter" << block.output;
        expect(eq(occurrences(block.output, "\"dataType\": \"float64\""), 2UZ)) << "the concrete type of each port of each key" << block.output;
    };

    "schedulers lists the program's own schedulers and a plugin's, each with its settings"_test = [] {
        const Result schedulers = run(overTestDirectories({"schedulers"}));
        expect(eq(schedulers.exitCode, 0)) << schedulers.output;
        expect(schedulers.output.contains("gr::scheduler")) << "the family of core's schedulers" << schedulers.output;
        expect(schedulers.output.contains("this-program")) << "the keys the program registers itself" << schedulers.output;
        for (const std::string_view policy : {"<singleThreaded>", "<multiThreaded>", "<singleThreadedBlocking>", "<multiThreadedBlocking>"}) {
            expect(schedulers.output.contains(policy)) << "one key per execution policy" << policy << schedulers.output;
        }
        expect(schedulers.output.contains("GoodMathScheduler")) << "the scheduler a plugin registers" << schedulers.output;
        expect(schedulers.output.contains("good_math_plugin")) << "under the file that registered it" << schedulers.output;
        expect(schedulers.output.contains("timeout_ms")) << "a scheduler's own setting" << schedulers.output;
        expect(schedulers.output.contains("scheduler keys")) << schedulers.output;
        expect(!schedulers.output.contains("unique_name")) << "the framework's settings stay behind --all-settings" << schedulers.output;
        for (const std::string_view blockOnly : {"no stream ports", "ports", "alias of", "category"}) {
            expect(!schedulers.output.contains(blockOnly)) << "a scheduler entry prints nothing only a block has:" << blockOnly << schedulers.output;
        }
    };

    "schedulers --json is one document shaped by library, family and scheduler"_test = [] {
        const Result schedulers = run(overTestDirectories({"schedulers", "--json"}), Streams::standardOutput);
        expect(eq(schedulers.exitCode, 0)) << schedulers.output;
        expect(isOneJsonDocument(schedulers.output)) << schedulers.output;
        expect(schedulers.output.contains("\"command\": \"schedulers\"")) << schedulers.output;
        for (const std::string_view key : {"\"libraries\"", "\"schedulerKeys\"", "\"families\"", "\"schedulers\"", "\"instantiations\"", "\"totals\""}) {
            expect(schedulers.output.contains(key)) << key << schedulers.output;
        }
        expect(schedulers.output.contains("\"key\": \"gr::scheduler::Simple<singleThreaded>\"")) << schedulers.output;
        expect(schedulers.output.contains("\"key\": \"gr::scheduler::Simple<multiThreaded>\"")) << schedulers.output;
        expect(schedulers.output.contains("\"key\": \"good::GoodMathScheduler\"")) << schedulers.output;
        expect(schedulers.output.contains("\"name\": \"timeout_ms\"")) << schedulers.output;
        expect(schedulers.output.contains("\"unit\": \"ms\"")) << "the unit the annotation carries" << schedulers.output;
        expect(!schedulers.output.contains(": null")) << schedulers.output;
        for (const std::string_view blockOnly : {"\"ports\"", "\"blockCategory\"", "\"uiCategory\""}) {
            expect(!schedulers.output.contains(blockOnly)) << "a scheduler entry carries nothing only a block has:" << blockOnly << schedulers.output;
        }
    };

    "version names the registered schedulers and counts them"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains("gr::scheduler::Simple<singleThreaded>")) << version.output;
        expect(version.output.contains("good::GoodMathScheduler")) << version.output;
        expect(version.output.contains("scheduler keys")) << version.output;
    };

    "schedulers lists a shared object's scheduler of this ABI version and not one of an earlier version or none"_test = [] {
        const Result schedulers = run(overSchedulerLibraries({"schedulers"}));
        expect(eq(schedulers.exitCode, 0)) << schedulers.output;
        expect(schedulers.output.contains("libscheduler_library.so (block-library, 1 keys)")) << "the kept library and its scheduler" << schedulers.output;
        expect(schedulers.output.contains("\n      library_scheduler\n")) << "the scheduler under its family" << schedulers.output;
        expect(!schedulers.output.contains("libscheduler_library_v1.so (")) << "the refused library heads no listing" << schedulers.output;
        expect(!schedulers.output.contains("libscheduler_library_unversioned.so (")) << "the refused library heads no listing" << schedulers.output;
        expect(eq(occurrences(schedulers.output, "library_scheduler_v1"), 1UZ)) << "its key appears in the loader's warning alone" << schedulers.output;

        const Result version = run(overSchedulerLibraries({"version", "--json"}), Streams::standardOutput);
        expect(eq(version.exitCode, 0)) << version.output;
        expect(isOneJsonDocument(version.output)) << version.output;
        expect(version.output.contains("\"kind\": \"not-loaded\"")) << "the refused library is a file that did not load" << version.output;
        expect(version.output.contains("\"reason\": \"scheduler test::library_scheduler_v1 has plugin ABI version 1, which does not match the host's plugin ABI version")) << "with the reason the loader gave" << version.output;
        expect(version.output.contains("\"reason\": \"scheduler test::library_scheduler_unversioned carries no plugin ABI version")) << "with the reason the loader gave" << version.output;
        expect(version.output.contains("\"test::library_scheduler\"")) << "the kept library's scheduler is registered" << version.output;
    };

    "a plugin refused for its plugin ABI version is reported on standard error and not in the document"_test = [] {
        const std::vector<std::string> arguments{"blocks", "--markdown", "--plugin-dir", GR_TOOLS_TEST_PLUGIN_ABI};
        const Result                   document = run(arguments, Streams::standardOutput);
        expect(eq(document.exitCode, 0)) << document.output;
        expect(document.output.starts_with("# grinfo blocks\n")) << "the document opens standard output" << document.output;
        expect(!document.output.contains("not loaded")) << document.output;

        const Result both = run(arguments);
        expect(eq(both.exitCode, 0)) << both.output;
        expect(both.output.contains("libabi_probe_plugin_v1.so not loaded: plugin ABI version 1 does not match")) << "the warning reaches standard error" << both.output;
    };

    "version --markdown and --html carry the search path and the plugins as table rows"_test = [] {
        const Result markdown = run(overTestDirectories({"version", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo version\n")) << "the command is the title" << markdown.output;
        expect(hasRenderedRow(markdown.output, {GR_TOOLS_CORE_TEST_PLUGINS, "option", "searched"})) << "a directory row, its path whole" << markdown.output;
        expect(markdown.output.contains("| Good Math Plugin | v1 | MIT | 4 | 1 |")) << "a plugin row" << markdown.output;
        expect(markdown.output.contains("- gr::scheduler::Simple\\<singleThreaded\\>\n")) << "a scheduler key, its brackets escaped" << markdown.output;

        const Result html = run(overTestDirectories({"version", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains(std::format("<tr><td>{}</td><td>option</td><td>searched</td></tr>", GR_TOOLS_CORE_TEST_PLUGINS))) << html.output;
        expect(html.output.contains("<tr><td>Good Math Plugin</td><td>v1</td><td>MIT</td><td>4</td><td>1</td></tr>")) << html.output;
    };

    "blocks --markdown and --html list each family's blocks with their types"_test = [] {
        const Result markdown = run(overTestDirectories({"blocks", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo blocks\n")) << markdown.output;
        expect(markdown.output.contains("| multiply | \\<float32\\> \\<float64\\> |")) << "a block row, the angle brackets of its types escaped" << markdown.output;
        expect(markdown.output.contains("| LibraryDoubler |  |")) << "a block of no type parameters" << markdown.output;

        const Result html = run(overTestDirectories({"blocks", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<tr><td>multiply</td><td>&lt;float32&gt; &lt;float64&gt;</td></tr>")) << html.output;
    };

    "blocks --verbose --markdown and --html carry each block's port and setting rows"_test = [] {
        const Result markdown = run(overTestDirectories({"blocks", "--verbose", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.contains("| in | input | float32 |")) << "a port with its type" << markdown.output;
        expect(markdown.output.contains("| extra_gain | float32 | 1.000000 | dB | yes |")) << "a setting with its type and default" << markdown.output;
        expect(markdown.output.contains("| in | input | T |")) << "a port written in the block's type parameter" << markdown.output;

        const Result html = run(overTestDirectories({"blocks", "--verbose", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<tr><td>in</td><td>input</td><td>float32</td></tr>")) << html.output;
        expect(html.output.contains("<tr><td>extra_gain</td><td>float32</td><td>1.000000</td><td>dB</td><td>yes</td></tr>")) << html.output;
    };

    "every link of blocks --verbose leads to an anchor of the same document"_test = [] {
        const Result markdown = run(overTestDirectories({"blocks", "--verbose", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        const std::vector<std::string> markdownLinks = linkTargets(markdown.output, "](#", ")");
        expect(markdown.output.contains("- [gr::testing::LibraryDoubler](#")) << "the list opens with a link per block" << markdown.output;
        expect(ge(markdownLinks.size(), 2UZ)) << markdown.output;
        for (const std::string& target : markdownLinks) {
            expect(markdown.output.contains(std::format("<a id=\"{}\"></a>", target))) << target << markdown.output;
        }

        const Result html = run(overTestDirectories({"blocks", "--verbose", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        const std::vector<std::string> htmlLinks = linkTargets(html.output, "href=\"#", "\"");
        expect(eq(htmlLinks.size(), markdownLinks.size())) << "the same list in both forms" << html.output;
        for (const std::string& target : htmlLinks) {
            expect(html.output.contains(std::format(" id=\"{}\">", target))) << target << html.output;
        }
    };

    "block --markdown and --html carry the block's port and setting rows"_test = [] {
        const Result markdown = run(overTestDirectories({"block", "LibraryDoubler", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo block LibraryDoubler\n")) << markdown.output;
        expect(markdown.output.contains("## gr::testing::LibraryDoubler\n")) << "the block named in full" << markdown.output;
        expect(markdown.output.contains("| out | output | float32 |")) << markdown.output;
        expect(markdown.output.contains("| extra_gain | float32 | 1.000000 | dB | yes |")) << markdown.output;
        expect(markdown.output.contains("doubles its input")) << "the description the block declares" << markdown.output;

        const Result html = run(overTestDirectories({"block", "LibraryDoubler", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<tr><td>out</td><td>output</td><td>float32</td></tr>")) << html.output;
        expect(html.output.contains("<tr><td>extra_gain</td><td>float32</td><td>1.000000</td><td>dB</td><td>yes</td></tr>")) << html.output;
    };

    "a templated key in a Markdown title and table cell keeps its angle brackets"_test = [] {
        const Result markdown = run(overTestDirectories({"block", "good::multiply<float32>", "--all-settings", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo block good::multiply\\<float32\\>\n")) << "the title, escaped" << markdown.output;
        expect(markdown.output.contains("| name | string | \"good::multiply\\<float32\\>\" |  | yes |")) << "the default of name, escaped" << markdown.output;
        expect(!markdown.output.contains("\"good::multiply<float32>\"")) << "and nowhere raw" << markdown.output;
    };

    "an alias is named as an alias in both documents"_test = [] {
        const Result markdown = run(overTestDirectories({"block", "library_doubler", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.contains("- **alias of**: gr::testing::LibraryDoubler\n")) << markdown.output;

        const Result html = run(overTestDirectories({"block", "library_doubler", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<li><strong>alias of</strong>: gr::testing::LibraryDoubler</li>")) << html.output;
    };

    "the documents leave out the framework's settings unless asked for them"_test = [] {
        for (const std::string_view form : {"--markdown", "--html"}) {
            const Result hidden = run(overTestDirectories({"block", "LibraryDoubler", std::string(form)}), Streams::standardOutput);
            expect(eq(hidden.exitCode, 0)) << hidden.output;
            expect(!hidden.output.contains("unique_name")) << form << hidden.output;
            expect(hidden.output.contains("--all-settings shows them")) << form << hidden.output;

            const Result shown = run(overTestDirectories({"block", "LibraryDoubler", std::string(form), "--all-settings"}), Streams::standardOutput);
            expect(eq(shown.exitCode, 0)) << shown.output;
            expect(shown.output.contains(form == "--html" ? "<tr><td>unique_name</td><td>string</td>" : "| unique_name | string |")) << form << shown.output;
        }
    };

    "schedulers --markdown and --html carry a scheduler's setting row"_test = [] {
        const Result markdown = run(overTestDirectories({"schedulers", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo schedulers\n")) << markdown.output;
        expect(markdown.output.contains("| timeout_ms | uint32 | 100 | ms |")) << "a setting with its type, default and unit" << markdown.output;
        expect(!markdown.output.contains("| port |")) << "a scheduler has no port table" << markdown.output;

        const Result html = run(overTestDirectories({"schedulers", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<tr><td>timeout_ms</td><td>uint32</td><td>100</td><td>ms</td></tr>")) << html.output;
    };

    "every HTML page is one self-contained page"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"version"}, {"blocks"}, {"blocks", "--verbose"}, {"block", "convert"}, {"schedulers"}}) {
            std::vector<std::string> withForm(arguments);
            withForm.emplace_back("--html");
            const Result page = run(overTestDirectories(withForm), Streams::standardOutput);
            expect(eq(page.exitCode, 0)) << page.output;
            expect(page.output.starts_with("<!DOCTYPE html>\n")) << arguments.front() << page.output;
            for (const std::string_view reference : kExternalReferences) {
                expect(!page.output.contains(reference)) << arguments.front() << reference << page.output;
            }
        }
    };

    "no line of any report is wider than a terminal"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"version"}, {"blocks"}, {"blocks", "--verbose"}, {"block", "convert"}, {"block", "LibraryDoubler", "--all-settings"}, {"schedulers"}, {"schedulers", "--all-settings"}}) {
            const Result report = run(overTestDirectories(arguments));
            expect(eq(report.exitCode, 0)) << report.output;
            expect(le(widestLine(report.output), kWidth)) << arguments.front() << report.output;
        }
    };
#endif

    "the Markdown cell reader shows escaped characters as written and refuses markup"_test = [] {
        expect(eq(renderedCell("\\*literal\\* \\&lt;").value_or("(markup)"), std::string("*literal* &lt;")));
        expect(eq(renderedCell("sample_rate").value_or("(markup)"), std::string("sample_rate"))) << "an underscore inside a word";
        expect(!renderedCell("*literal*").has_value()) << "emphasis";
        expect(!renderedCell("&lt;").has_value()) << "an entity";
        expect(!renderedCell("_x").has_value()) << "an underscore at a word's start";
        expect(cellsOf("| a | b\\|c |  |") == std::vector<std::string>{"a", "b\\|c", ""}) << "a row split at its unescaped bars";
    };

#ifdef GR_TOOLS_TEST_DOC_PROBE_LIBRARY
    "a string default keeps its characters in the Markdown cell, the HTML cell and the JSON document"_test = [] {
        const std::string written  = "*literal* &lt;";
        const Result      markdown = run({"block", "probe::Stage", "--markdown", "--plugin-dir", GR_TOOLS_TEST_DOC_PROBE_LIBRARY}, Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        const std::optional<std::string> row = lineStartingWith(markdown.output, "| label | string | ");
        expect(fatal(row.has_value())) << "the setting's row" << markdown.output;
        const std::vector<std::string> cells = cellsOf(*row);
        expect(fatal(ge(cells.size(), 3UZ))) << *row;
        expect(eq(renderedCell(cells[2]).value_or("(markup)"), std::format("\"{}\"", written))) << "the default as a renderer shows it" << *row;

        const Result html = run({"block", "probe::Stage", "--html", "--plugin-dir", GR_TOOLS_TEST_DOC_PROBE_LIBRARY}, Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.contains("<td>label</td><td>string</td><td>&quot;*literal* &amp;lt;&quot;</td>")) << html.output;

        const Result json = run({"block", "probe::Stage", "--json", "--plugin-dir", GR_TOOLS_TEST_DOC_PROBE_LIBRARY}, Streams::standardOutput);
        expect(eq(json.exitCode, 0)) << json.output;
        expect(json.output.contains(std::format("\"default\": \"{}\"", written))) << json.output;
    };

    // The three type names make the slugs probe-stage, probe-stage and probe-stage-1: the suffix that sets the second
    // apart from the first gives the third's slug. A detailed entry's heading is its type name without the namespace.
    "every id of blocks --verbose is unique and each link reaches its own block's section"_test = [] {
        for (const bool html : {false, true}) {
            const std::string_view form     = html ? "--html" : "--markdown";
            const Result           document = run({"blocks", "--verbose", std::string(form), "--plugin-dir", GR_TOOLS_TEST_DOC_PROBE_LIBRARY}, Streams::standardOutput);
            expect(eq(document.exitCode, 0)) << document.output;

            std::vector<std::string> ids = linkTargets(document.output, html ? " id=\"" : "<a id=\"", "\"");
            expect(ge(ids.size(), 6UZ)) << form << "the library, its two families and its three blocks" << document.output;
            std::ranges::sort(ids);
            expect(std::ranges::adjacent_find(ids) == ids.end()) << form << "an id appears twice" << document.output;

            const std::vector<Link>  links = navigationLinks(document.output, html);
            std::vector<std::string> labels;
            for (const Link& link : links) {
                labels.push_back(link.label);
                const std::size_t                qualifier = link.label.rfind("::");
                const std::string                expected  = qualifier == std::string::npos ? link.label : link.label.substr(qualifier + 2UZ);
                const std::optional<std::string> heading   = headingWithId(document.output, link.target, html);
                expect(eq(heading.value_or("(no section)"), expected)) << form << link.label << "leads to" << link.target << document.output;
            }
            std::ranges::sort(labels);
            expect(labels == std::vector<std::string>{"probe::Stage", "probe_Stage", "probe_Stage_1"}) << form << "a link per block" << document.output;
        }
    };
#endif

#ifdef GR_TOOLS_TEST_DEVICE_LISTING
    "devices lists the units of every loaded family under its word, and why a family listed none"_test = [] {
        const Result devices = run(overDeviceListings({"devices"}));
        expect(eq(devices.exitCode, 0)) << devices.output;
        expect(devices.output.contains("fakelibrary (receive and transmit)\n")) << "a family with the directions of its blocks" << devices.output;
        expect(hasFact(devices.output, "fakelibrary:serial=FAKE0001", "Fake unit A")) << "a unit by its selector and its label" << devices.output;
        expect(hasFact(devices.output, "(no selector)", "Fake unit without a serial")) << devices.output;
        expect(hasFact(devices.output, "fakeplugin:index=0", "Fake plugin unit")) << "a family a plugin's file registers" << devices.output;
        expect(devices.output.contains("fakeopening (receive, opens a unit)\n  not listed: its listing opens a unit, and the call does not allow that\n")) << devices.output;
        expect(!devices.output.contains("fakeopening:bus=1")) << "the listing that opens a unit is not called" << devices.output;
        expect(devices.output.contains("fakethrowing (receive)\n  not listed: the fake bus does not answer\n")) << "a listing that throws, with its message" << devices.output;
        expect(devices.output.contains("fakeempty (transmit)\n  listed: the listing found no unit\n")) << devices.output;
        expect(devices.output.contains("is asked with --allow-open")) << devices.output;

        const Result opened = run(overDeviceListings({"devices", "--allow-open"}));
        expect(eq(opened.exitCode, 0)) << opened.output;
        expect(opened.output.contains("fakeopening (receive, opens a unit)\n")) << opened.output;
        expect(hasFact(opened.output, "fakeopening:bus=1", "Fake opened unit")) << opened.output;
        expect(!opened.output.contains("--allow-open")) << "no family is left out" << opened.output;
    };

    "devices --json carries the units and the families in the shape a reader relies on"_test = [] {
        const Result devices = run(overDeviceListings({"devices", "--json"}), Streams::standardOutput);
        expect(eq(devices.exitCode, 0)) << devices.output;
        expect(isOneJsonDocument(devices.output)) << devices.output;
        expect(devices.output.contains("\"command\": \"devices\"")) << devices.output;

        const std::string_view unit = enclosingJsonObject(devices.output, "\"label\": \"Fake unit A\"");
        for (const std::string_view member : {"\"family\": \"fakelibrary\"", "\"selector\": \"fakelibrary:serial=FAKE0001\"", "\"receive\": true", "\"transmit\": true"}) {
            expect(unit.contains(member)) << member << devices.output;
        }
        const std::string_view unnamed = enclosingJsonObject(devices.output, "\"label\": \"Fake unit without a serial\"");
        expect(!unnamed.empty() && !unnamed.contains("selector")) << "a unit listed without a selector carries none" << devices.output;

        const std::string_view opening = enclosingJsonObject(devices.output, "\"family\": \"fakeopening\",\n      \"opens\"");
        for (const std::string_view member : {"\"opens\": true", "\"listed\": false", "\"reason\": \"its listing opens a unit, and the call does not allow that\""}) {
            expect(opening.contains(member)) << member << devices.output;
        }
        const std::string_view throwing = enclosingJsonObject(devices.output, "\"family\": \"fakethrowing\",\n      \"opens\"");
        expect(throwing.contains("\"listed\": false") && throwing.contains("\"reason\": \"the fake bus does not answer\"")) << devices.output;
        const std::string_view listed = enclosingJsonObject(devices.output, "\"family\": \"fakelibrary\",\n      \"opens\"");
        expect(listed.contains("\"listed\": true") && !listed.contains("reason")) << "a family that listed a unit carries no reason" << devices.output;
    };

    "devices --markdown and --html carry a section per family with its units and its reason"_test = [] {
        const Result markdown = run(overDeviceListings({"devices", "--markdown"}), Streams::standardOutput);
        expect(eq(markdown.exitCode, 0)) << markdown.output;
        expect(markdown.output.starts_with("# grinfo devices\n")) << "the command is the title" << markdown.output;
        expect(markdown.output.contains("\n## fakelibrary (receive and transmit)\n")) << "a section per family" << markdown.output;
        expect(markdown.output.contains("\n- **fakelibrary:serial=FAKE0001**: Fake unit A\n")) << "a unit by its selector and its label" << markdown.output;
        expect(markdown.output.contains("\n## fakethrowing (receive)\n\nnot listed: the fake bus does not answer\n")) << "a family's reason after its heading" << markdown.output;
        expect(markdown.output.contains("asked with --allow-open")) << markdown.output;

        const Result html = run(overDeviceListings({"devices", "--html"}), Streams::standardOutput);
        expect(eq(html.exitCode, 0)) << html.output;
        expect(html.output.starts_with("<!DOCTYPE html>\n")) << html.output;
        expect(html.output.contains("<h2 id=\"fakelibrary-receive-and-transmit\">fakelibrary (receive and transmit)</h2>")) << html.output;
        expect(html.output.contains("<li><strong>fakelibrary:serial=FAKE0001</strong>: Fake unit A</li>")) << html.output;
        expect(html.output.contains("<p>not listed: its listing opens a unit, and the call does not allow that</p>")) << html.output;
        for (const std::string_view reference : kExternalReferences) {
            expect(!html.output.contains(reference)) << reference << html.output;
        }
    };

    "a family that listed no unit carries its directions, and its section id is the same with --allow-open"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"devices", "--html"}, {"devices", "--html", "--allow-open"}}) {
            const Result html = run(overDeviceListings(arguments), Streams::standardOutput);
            expect(eq(html.exitCode, 0)) << html.output;
            expect(html.output.contains("<h2 id=\"fakeopening-receive-opens-a-unit\">fakeopening (receive, opens a unit)</h2>")) << html.output;
            expect(html.output.contains("<h2 id=\"fakeempty-transmit\">fakeempty (transmit)</h2>")) << "a family that found no unit" << html.output;
        }
    };

    "devices over plugins that register no listing names none of the test families"_test = [] {
        const EnvironmentVariable unset("GNURADIO4_PLUGIN_DIRECTORIES", std::nullopt);
        const Result              devices = run({"devices", "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS});
        expect(eq(devices.exitCode, 0)) << devices.output;
        for (const std::string_view family : {"fakelibrary", "fakeopening", "fakethrowing", "fakeempty", "fakeplugin"}) {
            expect(!devices.output.contains(family)) << family << devices.output;
        }
    };

    "no line of the device report is wider than a terminal"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"devices"}, {"devices", "--allow-open"}}) {
            const Result report = run(overDeviceListings(arguments));
            expect(eq(report.exitCode, 0)) << report.output;
            expect(le(widestLine(report.output), kWidth)) << report.output;
        }
    };
#endif

    "--help is no wider than a terminal either"_test = [] {
        const Result help = run({"--help"});
        expect(eq(help.exitCode, 0)) << help.output;
        expect(le(widestLine(help.output), kWidth)) << help.output;
    };

    "a name nothing is registered under is refused"_test = [] {
        const Result missing = run({"block", "NoSuchBlockIsRegistered"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("no block named NoSuchBlockIsRegistered is registered")) << missing.output;
    };
};

int main() { /* not needed for UT */ }
