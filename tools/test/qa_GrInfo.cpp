#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
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
 * - a name nothing is registered under
 * - a command line that cannot be used
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
        for (const std::string_view policy : {"<singleThreaded>", "<multiThreaded>", "<singleThreadedBlocking>"}) {
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

    "no line of any report is wider than a terminal"_test = [] {
        for (const std::vector<std::string>& arguments : {std::vector<std::string>{"version"}, {"blocks"}, {"blocks", "--verbose"}, {"block", "convert"}, {"block", "LibraryDoubler", "--all-settings"}, {"schedulers"}, {"schedulers", "--all-settings"}}) {
            const Result report = run(overTestDirectories(arguments));
            expect(eq(report.exitCode, 0)) << report.output;
            expect(le(widestLine(report.output), kWidth)) << arguments.front() << report.output;
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
