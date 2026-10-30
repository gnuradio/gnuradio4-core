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
 * every case here runs the built executable. The cases cover:
 * - a graph that would not end by itself, bounded by --seconds
 * - the settings --show prints when the run is over
 * - a scheduler setting and a block setting, each taken and each refused
 * - a refused setting's reason, printed without the source file and line of the library
 * - the program's own scheduler, a plugin's and a shared object's, each chosen by its registry key
 * - a shared object's scheduler refused at an earlier plugin ABI version
 * - a command line that cannot be used
 * - a graph file that cannot be read
 * - the names --show and --set take for a block, and a name two blocks share
 * - what --verbose reports, and that it reports the scheduler before the graph file is read
 * - a scheduler that cannot be built
 * - the errors a block and the scheduler report while the graph runs
 * - each refusal of a setting and each failure at start or while running of the fixture's blocks, each with its status and its reason
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

// how often `text` holds `part`
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view part) {
    std::size_t count = 0UZ;
    for (std::size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
constexpr std::string_view kGraphFile{GR_TOOLS_TEST_ASSETS "/source_to_sink.yaml"};
constexpr std::string_view kSettingsChainFile{GR_TOOLS_TEST_ASSETS "/settings_chain.yaml"};
constexpr std::string_view kStartChainFile{GR_TOOLS_TEST_ASSETS "/start_chain.yaml"};
constexpr std::string_view kUnnamedResourceFile{GR_TOOLS_TEST_ASSETS "/unnamed_resource_chain.yaml"};

// the graph, the plugins that supply its blocks, and a bound short enough for a test
[[nodiscard]] std::vector<std::string> boundedRun() { return {"--graph", std::string(kGraphFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over the chain whose two middle blocks open a resource when they start
[[nodiscard]] std::vector<std::string> startChainRun() { return {"--graph", std::string(kStartChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the same, over a chain whose middle block names no resource and fails to start
[[nodiscard]] std::vector<std::string> unnamedResourceRun() { return {"--graph", std::string(kUnnamedResourceFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

constexpr std::string_view kFaultFile{GR_TOOLS_TEST_ASSETS "/fault.yaml"};
constexpr std::string_view kSharedNameFile{GR_TOOLS_TEST_ASSETS "/shared_name.yaml"};

// the graph of the block that reports one error, over the fixture plugin; the bound is far beyond the report, and a
// run that reaches it was not stopped by the error
[[nodiscard]] std::vector<std::string> faultRun() { return {"--graph", std::string(kFaultFile), "--plugin-dir", GR_TOOLS_FIXTURE_PLUGINS, "--seconds", "10"}; }

// the graph of two sources that share a name, each with a unique_name
[[nodiscard]] std::vector<std::string> sharedNameRun() { return {"--graph", std::string(kSharedNameFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "5"}; }

// the same, over the four-block chain the settings cases set a value on
[[nodiscard]] std::vector<std::string> settingsChainRun() { return {"--graph", std::string(kSettingsChainFile), "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--seconds", "0.5"}; }

// the chain on the scheduler named by `key`. A count on its source bounds the chain, and the run ends by itself.
[[nodiscard]] std::vector<std::string> chainToItsEnd(std::string_view key) {
    std::vector<std::string>                arguments = settingsChainRun();
    const std::array<std::string_view, 7UZ> added{"--scheduler", key, "--set", "source.event_count=1000", "--show", "source", "--verbose"};
    arguments.insert(arguments.end(), added.begin(), added.end());
    return arguments;
}

// The first line of `output` that holds `marker`, without its newline; empty when no line holds it. A build without
// NDEBUG also prints a line of its own for every gr::exception it constructs. That line names the source file and line
// of the throw, and a case about rungraph's message reads rungraph's line alone.
[[nodiscard]] std::string_view lineHolding(std::string_view output, std::string_view marker) {
    const std::size_t found = output.find(marker);
    if (found == std::string_view::npos) {
        return {};
    }
    const std::size_t begin = output.rfind('\n', found);
    const std::size_t first = begin == std::string_view::npos ? 0UZ : begin + 1UZ;
    const std::size_t end   = output.find('\n', found);
    return output.substr(first, end == std::string_view::npos ? std::string_view::npos : end - first);
}
#endif

#ifdef GR_TOOLS_CORE_TEST_PLUGINS
constexpr std::string_view kRefusalFile{GR_TOOLS_TEST_ASSETS "/refusal.yaml"};
constexpr std::string_view kWrongTypeFile{GR_TOOLS_TEST_ASSETS "/refusal_wrong_type.yaml"};
constexpr std::string_view kOutOfLimitsFile{GR_TOOLS_TEST_ASSETS "/refusal_out_of_limits.yaml"};

// a graph of the fixture plugin; the bound is far beyond any failure, and a run that reaches it was not ended by one
[[nodiscard]] std::vector<std::string> fixtureRun(std::string_view file) { return {"--graph", std::string(file), "--plugin-dir", GR_TOOLS_FIXTURE_PLUGINS, "--seconds", "10"}; }

// the block that fails in the step `step` names
[[nodiscard]] std::vector<std::string> failingIn(std::string_view step) {
    std::vector<std::string> arguments = fixtureRun(kRefusalFile);
    arguments.insert(arguments.end(), {"--set", std::format("refusal.fails_in={}", step)});
    return arguments;
}

// The lines of `output` rungraph printed that hold `part`. A build without NDEBUG also prints a line of its own for
// every gr::exception it constructs, with the source file and line of the throw; that line is not rungraph's.
[[nodiscard]] std::vector<std::string_view> programLinesHolding(std::string_view output, std::string_view part) {
    std::vector<std::string_view> lines;
    for (std::size_t begin = 0UZ; begin < output.size();) {
        const std::size_t      end  = output.find('\n', begin);
        const std::string_view line = output.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (line.starts_with("rungraph: ") && line.contains(part)) {
            lines.push_back(line);
        }
        begin = end == std::string_view::npos ? output.size() : end + 1UZ;
    }
    return lines;
}
#endif

// the schedulers rungraph registers itself, one per execution policy of core's Simple scheduler
constexpr std::array<std::string_view, 4UZ> kOwnSchedulers{"gr::scheduler::Simple<singleThreaded>", "gr::scheduler::Simple<multiThreaded>", "gr::scheduler::Simple<singleThreadedBlocking>", "gr::scheduler::Simple<multiThreadedBlocking>"};

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

        const Result withoutValue = run({"--graph", "unread.yaml", "-s", "timeout_ms"});
        expect(eq(withoutValue.exitCode, 2)) << withoutValue.output;
        expect(withoutValue.output.contains("--set takes <key>=<value>")) << withoutValue.output;
    };

    "a --set whose value holds a second key is refused"_test = [] {
        const Result schedulerSetting = run({"--graph", "unread.yaml", "--set", "timeout_ms=10\nno_such_setting: 1"});
        expect(eq(schedulerSetting.exitCode, 2)) << schedulerSetting.output;
        expect(schedulerSetting.output.contains("the value of --set timeout_ms holds more than one key")) << schedulerSetting.output;
        expect(!schedulerSetting.output.contains("no_such_setting")) << "the second key never reaches the scheduler" << schedulerSetting.output;
        expect(!schedulerSetting.output.contains("unread.yaml")) << "the refusal comes before the graph is read" << schedulerSetting.output;

        const Result blockSetting = run({"--graph", "unread.yaml", "--set", "src.count=1\nother: 2"});
        expect(eq(blockSetting.exitCode, 2)) << blockSetting.output;
        expect(blockSetting.output.contains("the value of --set count holds more than one key")) << blockSetting.output;
        expect(!blockSetting.output.contains("unread.yaml")) << blockSetting.output;
    };

    "a scheduler key the registry does not hold is refused with the keys it holds"_test = [] {
        const Result refused = run({"--graph", "unread.yaml", "--scheduler", "no::such::Scheduler"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("no scheduler is registered as no::such::Scheduler")) << refused.output;
        for (const std::string_view key : kOwnSchedulers) {
            expect(refused.output.contains(key)) << "the list names" << key << refused.output;
        }
        expect(!refused.output.contains("unread.yaml")) << "the command line is judged before the graph is read" << refused.output;
    };

    "a scheduler setting is judged against the scheduler --scheduler chose"_test = [] {
        const Result refused = run({"--graph", "unread.yaml", "--scheduler", std::string(kOwnSchedulers[1]), "--set", "no_such_setting=1"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("the scheduler declares no setting named 'no_such_setting'")) << refused.output;
        expect(!refused.output.contains("unread.yaml")) << refused.output;
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

    "--set reaches a block's own setting, and --show reads it back"_test = [] {
        std::vector<std::string> arguments = settingsChainRun();
        arguments.emplace_back("--set");
        arguments.emplace_back("source.event_count=1000");
        arguments.emplace_back("--show");
        arguments.emplace_back("source");

        const Result set = run(arguments);
        expect(eq(set.exitCode, 0)) << set.output;
        expect(set.output.contains("source: event_count = 1000")) << set.output;
        expect(set.output.contains("the graph ended on its own")) << "the bound the setting carries stopped the run" << set.output;
    };

    "--set gives a block the value its start() reads"_test = [] {
        std::vector<std::string> arguments = startChainRun();
        arguments.insert(arguments.end(), {"--set", "first.resource=named-on-the-command-line", "--show", "first"});

        const Result started = run(arguments);
        expect(eq(started.exitCode, 0)) << started.output;
        expect(started.output.contains(R"(first: resource_at_start = "named-on-the-command-line")")) << "start() read the value the command line set, not the graph file's" << started.output;
        expect(started.output.contains("the graph was stopped before it ended")) << started.output;
    };

    "a block whose start fails holds every --set value, and --show reads each back"_test = [] {
        std::vector<std::string> arguments = unnamedResourceRun();
        arguments.insert(arguments.end(), {"--set", "first.sample_rate=2000", "--set", "first.gain=0.5", "--set", "source.event_count=1000", "--show", "first", "--show", "source"});

        const Result failed = run(arguments);
        expect(eq(failed.exitCode, 1)) << failed.output;
        expect(failed.output.contains("no resource to open")) << "start() failed on the empty resource the graph file gives the block" << failed.output;
        expect(failed.output.contains("first: sample_rate = 2000")) << failed.output;
        expect(failed.output.contains("first: gain = 0.5")) << failed.output;
        expect(failed.output.contains("source: event_count = 1000")) << "the value set on a second block is held as well" << failed.output;
    };

    // A --set value is refused as a graph file's value would be. A value outside the block's limits gets the
    // framework's message. A value the block's settingsChanged() throws on is refused when the scheduler initializes
    // the block, before start() runs.
    "a --set value the block refuses is refused as the graph file's value would be"_test = [] {
        std::vector<std::string> outOfLimits = startChainRun();
        outOfLimits.insert(outOfLimits.end(), {"--set", "first.gain=2", "--show", "first"});

        const Result refusedByLimits = run(outOfLimits);
        expect(refusedByLimits.output.contains("block 'first' refuses gain = 2: outside the limits [0, 1]")) << refusedByLimits.output;
        expect(!refusedByLimits.output.contains("first: gain = 2")) << "the block keeps a value inside its limits" << refusedByLimits.output;

        std::vector<std::string> refusedBySettingsChanged = startChainRun();
        refusedBySettingsChanged.insert(refusedBySettingsChanged.end(), {"--set", "first.sample_rate=-1"});

        const Result refusedByBlock = run(refusedBySettingsChanged);
        expect(refusedByBlock.output.contains("init() throws: sample_rate -1 is not positive")) << refusedByBlock.output;
    };

    "a --set value the block forwards reaches the block downstream in place of the graph file's"_test = [] {
        std::vector<std::string> arguments = startChainRun();
        arguments.insert(arguments.end(), {"--set", "first.sample_rate=2000", "--set", "source.event_count=1000", "--show", "second"});

        const Result forwarded = run(arguments);
        expect(eq(forwarded.exitCode, 0)) << forwarded.output;
        expect(forwarded.output.contains("second: sample_rate = 2000")) << "the graph file gives the upstream block 1000" << forwarded.output;
        expect(forwarded.output.contains("the graph ended on its own")) << "the run ended after every sample had passed the downstream block" << forwarded.output;
    };

    "a block, or a block setting, the graph does not hold is refused"_test = [] {
        std::vector<std::string> unknownKey = settingsChainRun();
        unknownKey.emplace_back("-s");
        unknownKey.emplace_back("source.no_such_key=1");

        const Result refusedKey = run(unknownKey);
        expect(eq(refusedKey.exitCode, 1)) << refusedKey.output;
        expect(refusedKey.output.contains("block 'source' of type 'good::fixed_source<float32>' declares no setting named 'no_such_key'; the nearest are")) << refusedKey.output;

        std::vector<std::string> unknownBlock = settingsChainRun();
        unknownBlock.emplace_back("-s");
        unknownBlock.emplace_back("no_such_block.event_count=1");

        const Result refusedBlock = run(unknownBlock);
        expect(eq(refusedBlock.exitCode, 1)) << refusedBlock.output;
        expect(refusedBlock.output.contains("settings are given for block 'no_such_block', and the graph holds no block of that name")) << refusedBlock.output;
    };

    "a value that does not convert is refused by its reason alone, without the source location of the refusal"_test = [] {
        const Result schedulerRefused = run({"--graph", "unread.yaml", "--set", "timeout_ms=many"});
        expect(eq(schedulerRefused.exitCode, 2)) << schedulerRefused.output;
        const std::string_view schedulerLine = lineHolding(schedulerRefused.output, "a scheduler setting could not be applied: ");
        expect(schedulerLine.contains("value \"many\" for key 'timeout_ms'")) << schedulerRefused.output;
        expect(!schedulerLine.contains(".cpp:") && !schedulerLine.contains(".hpp:")) << "a scheduler setting's refusal names no file and line of the library" << schedulerRefused.output;

        std::vector<std::string> member = settingsChainRun();
        member.emplace_back("-s");
        member.emplace_back("source.event_count=many");

        const Result memberRefused = run(member);
        expect(eq(memberRefused.exitCode, 1)) << memberRefused.output;
        const std::string_view memberLine = lineHolding(memberRefused.output, " did not load: ");
        expect(memberLine.contains("value \"many\" for key 'event_count'")) << memberRefused.output;
        expect(!memberLine.contains(".cpp:") && !memberLine.contains(".hpp:")) << "a member's refusal names no file and line of the library" << memberRefused.output;
    };

    "without --scheduler the graph runs on the single-threaded Simple scheduler"_test = [] {
        std::vector<std::string> arguments = settingsChainRun();
        arguments.emplace_back("--set");
        arguments.emplace_back("source.event_count=1000");
        arguments.emplace_back("--verbose");

        const Result ran = run(arguments);
        expect(eq(ran.exitCode, 0)) << ran.output;
        expect(ran.output.contains(std::format("rungraph: scheduler {}\n", kOwnSchedulers[0]))) << ran.output;
        expect(ran.output.contains("the graph ended on its own")) << ran.output;
    };

    "--scheduler runs the chain to its end on each of the program's own schedulers"_test = [] {
        for (const std::string_view key : kOwnSchedulers) {
            const Result ran = run(chainToItsEnd(key));
            expect(eq(ran.exitCode, 0)) << key << ran.output;
            expect(ran.output.contains(std::format("rungraph: scheduler {}\n", key))) << "the scheduler chosen is named" << ran.output;
            expect(ran.output.contains("the graph ended on its own")) << key << ran.output;
            expect(ran.output.contains("source: event_count = 1000")) << key << ran.output;
        }
    };

    "a bare --set key reaches the scheduler --scheduler chose"_test = [] {
        std::vector<std::string> arguments = chainToItsEnd(kOwnSchedulers[1]);
        arguments.emplace_back("--set");
        arguments.emplace_back("timeout_ms=37");

        const Result ran = run(arguments);
        expect(eq(ran.exitCode, 0)) << ran.output;
        expect(ran.output.contains("rungraph: scheduler setting timeout_ms = 37")) << "read back from the chosen scheduler" << ran.output;
        expect(ran.output.contains("the graph ended on its own")) << ran.output;
    };

    "a scheduler a plugin registers is chosen by its key and runs the chain to its end"_test = [] {
        std::vector<std::string> arguments = chainToItsEnd("good::GoodMathScheduler");
        arguments.emplace_back("--set");
        arguments.emplace_back("timeout_ms=37");

        const Result ran = run(arguments);
        expect(eq(ran.exitCode, 0)) << ran.output;
        expect(ran.output.contains("rungraph: scheduler good::GoodMathScheduler\n")) << ran.output;
        expect(ran.output.contains("rungraph: scheduler setting timeout_ms = 37")) << ran.output;
        expect(ran.output.contains("the graph ended on its own")) << ran.output;
        expect(ran.output.contains("source: event_count = 1000")) << ran.output;

        const Result refused = run({"--graph", "unread.yaml", "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--scheduler", "no::such::Scheduler"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("good::GoodMathScheduler")) << "the list of keys includes the plugin's" << refused.output;
    };

    "a shared object's scheduler of this ABI version runs the chain to its end, one of an earlier version or none is refused"_test = [] {
        std::vector<std::string> arguments = chainToItsEnd("test::library_scheduler");
        arguments.emplace_back("--plugin-dir");
        arguments.emplace_back(GR_TOOLS_TEST_SCHEDULER_LIBRARY);

        const Result ran = run(arguments);
        expect(eq(ran.exitCode, 0)) << ran.output;
        expect(ran.output.contains("rungraph: scheduler test::library_scheduler\n")) << ran.output;
        expect(ran.output.contains("the graph ended on its own")) << ran.output;
        expect(ran.output.contains("source: event_count = 1000")) << ran.output;
        expect(ran.output.contains("libscheduler_library_v1.so did not load: scheduler test::library_scheduler_v1 has plugin ABI version 1, which does not match the host's plugin ABI version")) << "the refusal is reported as a failed plugin is" << ran.output;
        expect(ran.output.contains("libscheduler_library_unversioned.so did not load: scheduler test::library_scheduler_unversioned carries no plugin ABI version")) << "the refusal is reported as a failed plugin is" << ran.output;

        const Result refused = run({"--graph", "unread.yaml", "--plugin-dir", GR_TOOLS_TEST_SCHEDULER_LIBRARY, "--scheduler", "test::library_scheduler_v1"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("no scheduler is registered as test::library_scheduler_v1")) << refused.output;
        expect(refused.output.contains("rungraph:   test::library_scheduler\n")) << "the list of keys includes the kept library's" << refused.output;
    };

    "a block name the graph does not hold is refused"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--show");
        arguments.emplace_back("no_such_block");

        const Result refused = run(arguments);
        expect(eq(refused.exitCode, 1)) << refused.output;
        expect(refused.output.contains("no block named no_such_block")) << refused.output;
    };

    "--verbose names each directory searched, the files that did not load and the block keys the load brought"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.emplace_back("--verbose");

        const Result verbose = run(arguments);
        expect(eq(verbose.exitCode, 0)) << verbose.output;
        expect(verbose.output.contains(std::format("rungraph: searching {}\n", GR_TOOLS_CORE_TEST_PLUGINS))) << verbose.output;
        expect(verbose.output.contains("libbad_plugin.so did not load: ")) << "the plugin that fails is reported with its reason" << verbose.output;
        expect(verbose.output.contains("rungraph:   good::fixed_source<float32>\n")) << "a key a plugin of the directory brought is listed" << verbose.output;
    };

    "--verbose reports the scheduler before the graph file is read"_test = [] {
        const Result unread = run({"--graph", "unread.yaml", "--plugin-dir", GR_TOOLS_CORE_TEST_PLUGINS, "--verbose", "--set", "timeout_ms=37"});
        expect(eq(unread.exitCode, 1)) << unread.output;
        expect(unread.output.contains("rungraph: scheduler gr::scheduler::Simple<singleThreaded>\n")) << unread.output;
        expect(unread.output.contains("rungraph: scheduler setting timeout_ms = 37")) << unread.output;
    };

    "a scheduler that cannot be built is refused without the usage text"_test = [] {
        const Result refused = run({"--graph", "unread.yaml", "--plugin-dir", GR_TOOLS_FIXTURE_PLUGINS, "--scheduler", "fixture::UnbuildableScheduler"});
        expect(eq(refused.exitCode, 2)) << refused.output;
        expect(refused.output.contains("could not be built: the fixture scheduler cannot be built")) << refused.output;
        expect(!refused.output.contains("Usage: rungraph")) << "a command line the program could not use was reported as unusable" << refused.output;
    };

    "--show and --set take the unique_name the graph file gives a block, and refuse a name two blocks share"_test = [] {
        std::vector<std::string> arguments = sharedNameRun();
        arguments.insert(arguments.end(), {"--set", "left.event_count=1000", "--set", "right.event_count=2000", "--show", "left", "--show", "right"});

        const Result byUniqueName = run(arguments);
        expect(eq(byUniqueName.exitCode, 0)) << byUniqueName.output;
        expect(byUniqueName.output.contains("left: event_count = 1000\n")) << byUniqueName.output;
        expect(byUniqueName.output.contains("right: event_count = 2000\n")) << "--set and --show reached different blocks under the two unique names" << byUniqueName.output;

        std::vector<std::string> shown = sharedNameRun();
        shown.insert(shown.end(), {"--show", "twin"});
        const Result sharedShow = run(shown);
        expect(eq(sharedShow.exitCode, 1)) << sharedShow.output;
        expect(sharedShow.output.contains("'twin' is the name of 2 blocks")) << sharedShow.output;

        std::vector<std::string> set = sharedNameRun();
        set.insert(set.end(), {"--set", "twin.event_count=1000"});
        const Result sharedSet = run(set);
        expect(eq(sharedSet.exitCode, 1)) << sharedSet.output;
        expect(sharedSet.output.contains("settings are given for block 'twin', and 2 blocks carry that unique_name or name")) << sharedSet.output;
    };

    "the first error a block reports ends the run, once, and without the source location"_test = [] {
        const Result reported = run(faultRun());
        expect(eq(reported.exitCode, 1)) << reported.output;
        expect(eq(occurrences(reported.output, "reports an error on 'processBulk': the fixture reports a fault"), 1UZ)) << reported.output;
        expect(reported.output.contains("rungraph: the graph was stopped on the first error reported")) << "the run went on to its bound" << reported.output;
        expect(!reported.output.contains(".hpp") && !reported.output.contains(".cpp")) << "the error names a file of the library" << reported.output;

        std::vector<std::string> failing = faultRun();
        failing.insert(failing.end(), {"--set", "fault.fails=true"});
        const Result failed = run(failing);
        expect(eq(failed.exitCode, 1)) << failed.output;
        expect(eq(occurrences(failed.output, "the fixture reports a fault"), 1UZ)) << "the run's result repeats the block's error" << failed.output;
        expect(eq(occurrences(failed.output, "rungraph: "), 2UZ)) << "the output holds the block's error once and the end of the run once" << failed.output;
    };

    "an error the scheduler reports reaches the output and fails the run"_test = [] {
        std::vector<std::string> arguments = boundedRun();
        arguments.insert(arguments.end(), {"--set", "poolName=nosuch"});

        const Result reported = run(arguments);
        expect(eq(reported.exitCode, 1)) << reported.output;
        expect(reported.output.contains("rungraph: the scheduler reports an error on ")) << reported.output;
        expect(reported.output.contains("'nosuch'")) << "the scheduler's reason is not printed" << reported.output;
    };

    "each refusal and failure of the fixture blocks ends the run with status 1 and prints the block's reason once, without a source path"_test = [] {
        struct Refusal {
            std::string_view         path;
            std::vector<std::string> arguments;
            std::string_view         reason;
        };
        const std::vector<Refusal> refusals{
            {"a graph file value of the wrong type", fixtureRun(kWrongTypeFile), R"(value "many" for key 'count' has wrong type)"},
            {"a graph file value outside the limits", fixtureRun(kOutOfLimitsFile), "outside the limits [0, 1]"},
            {"settingsChanged() throws", failingIn("settingsChanged"), "the fixture refuses its settings"},
            {"start() throws", failingIn("start"), "the fixture refuses to start"},
            {"processBulk() throws", failingIn("processBulk"), "the fixture fails in processBulk"},
            {"work() returns ERROR", failingIn("work"), "its work() returned ERROR"},
            {"an error on the message port", fixtureRun(kFaultFile), "the fixture reports a fault"},
        };
        for (const Refusal& refusal : refusals) {
            const Result                        failed = run(refusal.arguments);
            const std::vector<std::string_view> lines  = programLinesHolding(failed.output, refusal.reason);
            expect(eq(failed.exitCode, 1)) << refusal.path << failed.output;
            expect(eq(lines.size(), 1UZ)) << refusal.path << "prints its reason once" << failed.output;
            for (const std::string_view line : lines) {
                expect(!line.contains(".cpp:") && !line.contains(".hpp:")) << refusal.path << "names a source file" << failed.output;
            }
        }
    };
#endif
};

int main() { /* not needed for UT */ }
