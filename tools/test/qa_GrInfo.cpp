#include <boost/ut.hpp>

#include <array>
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
 * every case here runs the built executable over core's own test plugins and test block libraries: the framework
 * report, the block listing, one block in detail, a name nothing is registered under, and a command line that
 * cannot be used.
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
    std::string output; // standard output and standard error together, in the order the run wrote them
};

// runs the tool with `arguments` and collects what it wrote and the status it exited with
[[nodiscard]] Result run(const std::vector<std::string>& arguments) {
    std::string command = std::format("\"{}\"", GR_TOOLS_GRINFO);
    for (const std::string& argument : arguments) {
        command += std::format(" \"{}\"", argument);
    }
    command += " 2>&1";

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

// one JSON document: every bracket closed in order outside a string, and nothing left open at the end
[[nodiscard]] bool isOneJsonDocument(std::string_view text) {
    std::size_t depth    = 0UZ;
    bool        inString = false;
    bool        escaped  = false;
    bool        opened   = false;
    for (const char character : text) {
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
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "version names the directories searched and what they held"_test = [] {
        const Result version = run(overTestDirectories({"version"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(version.output.contains(GR_TOOLS_CORE_TEST_PLUGINS)) << "the directory it was given" << version.output;
        expect(version.output.contains(GR_TOOLS_TEST_BLOCK_LIBRARY)) << "the directory it was given" << version.output;
        expect(version.output.contains("block library")) << "a shared object that registers without being a plugin" << version.output;
        expect(version.output.contains("block_library")) << "and the file it is" << version.output;
        expect(version.output.contains("not loaded")) << "the plugin whose ABI version does not match" << version.output;
        expect(version.output.contains("Good Math Plugin")) << "a plugin that did load" << version.output;
        expect(version.output.contains("block keys")) << version.output;
    };

    "version --json is one document carrying the shape a reader relies on"_test = [] {
        const Result version = run(overTestDirectories({"version", "--json"}));
        expect(eq(version.exitCode, 0)) << version.output;
        expect(isOneJsonDocument(version.output)) << version.output;
        expect(version.output.contains("\"schema\": 1")) << version.output;
        for (const std::string_view key : {"\"framework\"", "\"directories\"", "\"libraries\"", "\"plugins\"", "\"schedulers\"", "\"totals\"", "\"blockKeys\""}) {
            expect(version.output.contains(key)) << key << version.output;
        }
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
        const Result blocks = run(overTestDirectories({"blocks", "--json"}));
        expect(eq(blocks.exitCode, 0)) << blocks.output;
        expect(isOneJsonDocument(blocks.output)) << blocks.output;
        expect(blocks.output.contains("\"schema\": 1")) << blocks.output;
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

    "block --json carries the settings with their defaults typed"_test = [] {
        const Result block = run(overTestDirectories({"block", "LibraryDoubler", "--json"}));
        expect(eq(block.exitCode, 0)) << block.output;
        expect(isOneJsonDocument(block.output)) << block.output;
        expect(block.output.contains("\"command\": \"block\"")) << block.output;
        expect(block.output.contains("\"name\": \"extra_gain\"")) << block.output;
        expect(block.output.contains("\"default\": 1")) << "a number is a number, not a string" << block.output;
        expect(block.output.contains("\"unit\": \"dB\"")) << block.output;
    };
#endif

    "a name nothing is registered under is refused"_test = [] {
        const Result missing = run({"block", "NoSuchBlockIsRegistered"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("no block named NoSuchBlockIsRegistered is registered")) << missing.output;
    };
};

int main() { /* not needed for UT */ }
