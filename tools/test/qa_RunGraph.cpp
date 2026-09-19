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
 * rungraph, driven as the program a caller runs.
 *
 * The tool's contract is its exit status and what it prints, and neither is visible from inside the process, so
 * every case here runs the built executable: a graph that would not end by itself, bounded by --seconds; the
 * settings --show prints when the run is over; a scheduler setting and a block setting taken and each refused, with
 * the fusion plan --fuse produces; a command line that cannot be used; and a graph file that cannot be read.
 */
namespace qa_rungraph {

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
    std::string command = std::format("\"{}\"", GR_TOOLS_RUNGRAPH);
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

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
constexpr std::string_view kGraphFile{GR_TOOLS_TEST_ASSETS "/source_to_sink.yaml"};
constexpr std::string_view kFusedChainFile{GR_TOOLS_TEST_ASSETS "/fused_chain.yaml"};

// the graph, the plugins that supply its blocks, and a bound short enough for a test
[[nodiscard]] std::vector<std::string> boundedRun() { return {"--graph", std::string(kGraphFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over the graph whose two middle blocks a fused run can take
[[nodiscard]] std::vector<std::string> fusedChainRun() { return {"--graph", std::string(kFusedChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5", "--verbose"}; }
#endif

} // namespace qa_rungraph

const boost::ut::suite<"RunGraph"> runGraphTests = [] {
    using namespace boost::ut;
    using namespace qa_rungraph;

    "a command line that cannot be used is refused with the usage text"_test = [] {
        const Result unknown = run({"--no-such-option"});
        expect(eq(unknown.exitCode, 2)) << unknown.output;
        expect(unknown.output.contains("unknown option '--no-such-option'")) << unknown.output;
        expect(unknown.output.contains("Usage: rungraph")) << unknown.output;

        const Result withoutArguments = run({});
        expect(eq(withoutArguments.exitCode, 2)) << withoutArguments.output;
        expect(withoutArguments.output.contains("Usage: rungraph")) << withoutArguments.output;

        const Result withoutValue = run({"--graph"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--graph needs a value")) << withoutValue.output;
    };

    "a scheduler setting the scheduler does not declare is refused"_test = [] {
        const Result refused = run({"--graph", "unread.yaml", "--set", "no_such_setting=1"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("the scheduler declares no setting named 'no_such_setting'")) << refused.output;
        expect(!refused.output.contains("unread.yaml")) << "the command line is judged before the graph is read" << refused.output;

        const Result withoutValue = run({"--graph", "unread.yaml", "-s", "enable_fusion"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--set takes <key>=<value>")) << withoutValue.output;

        const Result badCount = run({"--graph", "unread.yaml", "--fuse=many"});
        expect(eq(badCount.exitCode, 2)) << badCount.output;
        expect(badCount.output.contains("--fuse takes a count of samples per chunk")) << badCount.output;
    };

    "a graph file that cannot be read ends the run before anything is loaded"_test = [] {
        const Result missing = run({"--graph", "/gnuradio4-graph-file-that-does-not-exist.yaml"});
        expect(eq(missing.exitCode, 1)) << missing.output;
        expect(missing.output.contains("could not be read")) << missing.output;
    };

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
    "a run of a graph that does not end by itself is bounded by --seconds and stops cleanly"_test = [] {
        const Result bounded = run(boundedRun());
        expect(eq(bounded.exitCode, 0)) << bounded.output;
        expect(bounded.output.contains("the graph was stopped before it ended")) << bounded.output;
    };

    "--show prints the settings of the block it names, and of no other"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--show");
        arguments.emplace_back("source");

        const Result shown = run(arguments);
        expect(eq(shown.exitCode, 0)) << shown.output;
        expect(shown.output.contains("source: event_count = ")) << "the block's own setting is reported" << shown.output;
        expect(!shown.output.contains("sink:")) << "only the block --show named is reported" << shown.output;
    };

    "--verbose reports fusion off unless a scheduler setting asks for it"_test = [] {
        const Result off = run(fusedChainRun());
        expect(eq(off.exitCode, 0)) << off.output;
        expect(off.output.contains("rungraph: fusion is off")) << off.output;
    };

    "--fuse turns fusion on, and the plan names the run it produced"_test = [] {
        std::vector<std::string> arguments = fusedChainRun();
        arguments.emplace_back("--fuse");

        const Result fused = run(arguments);
        expect(eq(fused.exitCode, 0)) << fused.output;
        expect(fused.output.contains("fused run 1: first -> second")) << "the members in the order the run drives them" << fused.output;
        expect(fused.output.contains("1 fused run(s), 2 block(s) called one at a time")) << "the source and the sink stay outside the run" << fused.output;
    };

    "--fuse=<samples> is the chunk the plan reports"_test = [] {
        std::vector<std::string> arguments = fusedChainRun();
        arguments.emplace_back("--fuse=256");

        const Result fused = run(arguments);
        expect(eq(fused.exitCode, 0)) << fused.output;
        expect(fused.output.contains("fused run 1: first -> second (256 samples per chunk)")) << fused.output;
    };

    "--set reaches a block's own setting, and --show reads it back"_test = [] {
        std::vector<std::string> arguments = fusedChainRun();
        arguments.emplace_back("--set");
        arguments.emplace_back("source.event_count=1000");
        arguments.emplace_back("--show");
        arguments.emplace_back("source");

        const Result set = run(arguments);
        expect(eq(set.exitCode, 0)) << set.output;
        expect(set.output.contains("source: event_count = 1000")) << set.output;
        expect(set.output.contains("the graph ended on its own")) << "the bound the setting carries stopped the run" << set.output;
    };

    "a block, or a block setting, the graph does not hold is refused"_test = [] {
        std::vector<std::string> unknownKey = fusedChainRun();
        unknownKey.emplace_back("-s");
        unknownKey.emplace_back("source.no_such_key=1");

        const Result refusedKey = run(unknownKey);
        expect(eq(refusedKey.exitCode, 1)) << refusedKey.output;
        expect(refusedKey.output.contains("the block source declares no setting named 'no_such_key'")) << refusedKey.output;

        std::vector<std::string> unknownBlock = fusedChainRun();
        unknownBlock.emplace_back("-s");
        unknownBlock.emplace_back("no_such_block.event_count=1");

        const Result refusedBlock = run(unknownBlock);
        expect(eq(refusedBlock.exitCode, 1)) << refusedBlock.output;
        expect(refusedBlock.output.contains("no block named no_such_block")) << refusedBlock.output;
    };

    "a block name the graph does not hold is refused"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--show");
        arguments.emplace_back("no_such_block");

        const Result refused = run(arguments);
        expect(eq(refused.exitCode, 1)) << refused.output;
        expect(refused.output.contains("no block named no_such_block")) << refused.output;
    };
#endif
};

int main() { /* not needed for UT */ }
