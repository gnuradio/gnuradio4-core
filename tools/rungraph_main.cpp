// rungraph - load a graph file, run it, and report what the run was asked to report.
//
// No block, setting or connection is named in this file. The graph file names the blocks, and the plugin directories
// supply them through the program's own loader. gr::RuntimeGraph builds the graph and gr::Runtime runs it. A changed
// chain needs no rebuilt program.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

#include "ShippedSchedulers.hpp"

namespace {

constexpr std::string_view kProgram = "rungraph";

constexpr std::string_view kUsage = R"(rungraph - run a GNU Radio 4 graph file

Usage: rungraph --graph <file> [options]

  --graph <file>     the graph to run, in the GRC YAML dialect; - reads it from standard input
  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --scheduler <key>  the registry key of the scheduler that runs the graph;
                     gr::scheduler::Simple<singleThreaded> without it
  --seconds <s>      stop the graph after <s> seconds; without it the run ends when the graph does
  --show <name>      print the settings of the block named <name> when the run ends; repeatable
  --set, -s <key>=<value>
                     set one setting before the run; repeatable
  --verbose          list what each plugin directory loaded, the keys it brought and the scheduler
  --help, -h         this text

The blocks come from the directories named by --plugin-dir, from GNURADIO4_PLUGIN_DIRECTORIES,
the colon-separated list the framework's own plugin loader reads, and from the plugin directory
of this installation, which is always searched. A directory named twice is searched once.

rungraph registers core's Simple scheduler under one key per execution policy:
gr::scheduler::Simple<singleThreaded> runs every block on one thread,
gr::scheduler::Simple<multiThreaded> spreads the blocks over the threads of the processing pool,
gr::scheduler::Simple<singleThreadedBlocking> runs on one thread and sleeps while no block
makes progress, and gr::scheduler::Simple<multiThreadedBlocking> spreads the blocks over the
pool's threads, each of which sleeps while its own blocks make no progress. A plugin or a block
library in the searched directories may register more, and grinfo schedulers lists them all. A
key the registry does not hold is refused with the list of the keys it holds.

A settings map holds what the last refresh put there, so the settings --show prints are read
after the run has ended and the block has been asked to refresh them: a counter a block keeps
as a readable member is then current as of the last sample it processed.

A bare key of --set names a setting of the scheduler --scheduler chose, and a key of the form
<block>.<key> names a setting of the one top-level block whose unique_name or name is <block>;
a <block> two blocks carry is refused. The split is at the last dot before the '=', so a block
name may hold a dot and a setting key never does. rungraph reads the value the way a graph file's
parameter value is read, so a type tag applies: -s timeout_ms=50,
-s 'shift.frequency_shift=!!float32 -100000'. One --set carries one setting, and a value that
holds a second key is refused. The last --set of a key wins.

SIGINT and SIGTERM stop the graph as a --seconds bound does.

Exit status is 0 when the run stopped cleanly, 1 when the graph could not be read, loaded or run
and when a --set or --show names a block or a block setting the graph does not hold, and 2 when
the command line could not be used, a scheduler the registry does not hold or cannot build and
a scheduler setting the scheduler does not declare included.
)";

std::atomic<bool> gStopRequested{false};

extern "C" void onSignal(int) { gStopRequested.store(true, std::memory_order_relaxed); }

// one --set
struct Setting {
    std::string block; // the block the setting belongs to; empty names the scheduler
    std::string key;
    std::string value; // the text after the '=', read as a graph file's parameter value is read
};

struct Options {
    std::string              graph; // the graph file, or "-" for standard input
    std::vector<std::string> pluginDirectories;
    std::string              scheduler{gr::Runtime::kDefaultScheduler};
    std::vector<std::string> show;          // the blocks whose settings are printed when the run ends
    std::vector<Setting>     settings;      // in command-line order, so that the last of a key wins
    double                   seconds = 0.0; // 0 runs until the graph ends or a signal arrives
    bool                     verbose = false;
    bool                     help    = false;
};

// a run bound, or nothing when the text is not one positive number
[[nodiscard]] std::optional<double> secondsOf(std::string_view text) {
    double     value  = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !(value > 0.0)) {
        return std::nullopt;
    }
    return value;
}

// one <key>=<value>, or nothing when the text is not one; a key carrying a dot names a block and the key under it
[[nodiscard]] std::optional<Setting> settingOf(std::string_view text) {
    const std::size_t assignment = text.find('=');
    if (assignment == std::string_view::npos || assignment == 0UZ) {
        return std::nullopt;
    }
    const std::string_view target = text.substr(0UZ, assignment);
    const std::string_view value  = text.substr(assignment + 1UZ);
    const std::size_t      dot    = target.rfind('.');
    if (dot == std::string_view::npos) {
        return Setting{std::string(), std::string(target), std::string(value)};
    }
    if (dot == 0UZ || dot + 1UZ == target.size()) {
        return std::nullopt;
    }
    return Setting{std::string(target.substr(0UZ, dot)), std::string(target.substr(dot + 1UZ)), std::string(value)};
}

// the command line, or nothing when it cannot be used; every refusal is reported as it is found
[[nodiscard]] std::optional<Options> parse(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t index = 0UZ; index < arguments.size();) {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            ++index;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            ++index;
            continue;
        }
        // the option is recognized before its value is asked for, so that an unknown option in the last position is
        // reported as unknown rather than as one missing a value
        if (argument != "--graph" && argument != "--plugin-dir" && argument != "--scheduler" && argument != "--show" && argument != "--seconds" && argument != "--set" && argument != "-s") {
            std::println(stderr, "{}: unknown option '{}'", kProgram, argument);
            return std::nullopt;
        }
        if (index + 1UZ >= arguments.size()) {
            std::println(stderr, "{}: {} needs a value", kProgram, argument);
            return std::nullopt;
        }
        const std::string_view value = arguments[index + 1UZ];
        index += 2UZ;
        if (argument == "--graph") {
            options.graph.assign(value);
        } else if (argument == "--plugin-dir") {
            options.pluginDirectories.emplace_back(value);
        } else if (argument == "--scheduler") {
            options.scheduler.assign(value);
        } else if (argument == "--show") {
            options.show.emplace_back(value);
        } else if (argument == "--set" || argument == "-s") {
            std::optional<Setting> setting = settingOf(value);
            if (!setting.has_value()) {
                std::println(stderr, "{}: --set takes <key>=<value> or <block>.<key>=<value>, not '{}'", kProgram, value);
                return std::nullopt;
            }
            options.settings.push_back(std::move(*setting));
        } else {
            const std::optional<double> seconds = secondsOf(value);
            if (!seconds.has_value()) {
                std::println(stderr, "{}: --seconds takes one positive number of seconds, not '{}'", kProgram, value);
                return std::nullopt;
            }
            options.seconds = *seconds;
        }
    }
    if (!options.help && options.graph.empty()) {
        std::println(stderr, "{}: --graph names the graph file to run", kProgram);
        return std::nullopt;
    }
    return options;
}

// the graph file's text, or nothing when it could not be read
[[nodiscard]] std::optional<std::string> readGraph(const std::string& path) {
    std::ostringstream text;
    if (path == "-") {
        text << std::cin.rdbuf();
        return text.str();
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::println(stderr, "{}: {} could not be read", kProgram, path);
        return std::nullopt;
    }
    text << file.rdbuf();
    return text.str();
}

// where the blocks are looked for, in the order the directories are searched
[[nodiscard]] std::vector<std::string> searchDirectories(const std::vector<std::string>& fromCommandLine) {
    std::vector<std::string> directories;
    auto                     add = [&directories](std::string_view directory) {
        if (!directory.empty() && std::ranges::find(directories, directory) == directories.end()) {
            directories.emplace_back(directory);
        }
    };
    for (const std::string& directory : fromCommandLine) {
        add(directory);
    }
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        const std::string_view list(environment);
        for (std::size_t start = 0UZ; start < list.size();) {
            const std::size_t separator = list.find(':', start);
            const std::size_t end       = separator == std::string_view::npos ? list.size() : separator;
            add(list.substr(start, end - start));
            start = end + 1UZ;
        }
    }
    add(GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY);
    return directories;
}

// what the directories held: the files that loaded, the files that did not, and the block keys they brought
void reportPlugins(const gr::PluginLoader& loader, const std::vector<std::string>& directories, const std::vector<std::string>& keysBefore) {
    for (const std::string& directory : directories) {
        std::println(stderr, "{}: searching {}", kProgram, directory);
    }
    for (const gr::PluginLoader::BlockLibrary& library : loader.blockLibraries()) {
        std::println(stderr, "{}: loaded {} ({} block registration(s))", kProgram, library.file, library.nBlockRegistrations);
    }
    for (const auto& [file, reason] : loader.failedPlugins()) {
        std::println(stderr, "{}: {} did not load: {}", kProgram, file, reason);
    }
    for (const std::string& file : loader.skippedFiles()) {
        std::println(stderr, "{}: {} was not opened; its name only reads as a shared object", kProgram, file);
    }
    std::vector<std::string> available = loader.availableBlocks();
    std::ranges::sort(available);
    std::vector<std::string> added;
    std::ranges::set_difference(available, keysBefore, std::back_inserter(added));
    std::println(stderr, "{}: the load brought {} block key(s):", kProgram, added.size());
    for (const std::string& key : added) {
        std::println(stderr, "{}:   {}", kProgram, key);
    }
}

// One block's readable settings, one `label: key = value` line each, in key order, where `label` is the name --show
// gave.
//
// A block's settings are those of its last settings change, and the run refreshes them when it ends. The run is over by
// the time this is called, so a member that moved while the block ran is read as the block left it.
void showSettings(std::string_view label, const gr::BlockHandle& block) {
    std::vector<std::pair<std::string, std::string>> lines;
    for (const auto& [key, value] : block.get()) {
        lines.emplace_back(std::string(key.begin(), key.end()), std::format("{}", value));
    }
    std::ranges::sort(lines);
    for (const auto& [key, value] : lines) {
        std::println("{}: {} = {}", label, key, value);
    }
    std::fflush(stdout);
}

// the scheduler's settings, and the settings of each block named, which the graph file is read with
struct StagedSettings {
    gr::property_map                                     scheduler;
    std::map<std::string, gr::property_map, std::less<>> blocks;
};

// The settings the --set arguments stand for, or nothing when one of the values cannot be read or stands for more
// than the key it was given for.
//
// Each pair becomes a one-entry YAML document and the framework's own reader gives the value its type, so the text
// after the '=' means here what the same text means as a parameter of a graph file: a bare `true` is a boolean, a bare
// number an integer, and a tagged `!!float32 1.5` a float. The document has to come back holding that one key alone:
// text carrying a second line would otherwise reach the run as a setting the caller never wrote. A later setting of a
// key overwrites an earlier one.
[[nodiscard]] std::optional<StagedSettings> stagedSettingsOf(const std::vector<Setting>& settings) {
    StagedSettings staged;
    for (const Setting& setting : settings) {
        const auto parsed = gr::pmt::yaml::deserialize(std::format("{}: {}", setting.key, setting.value));
        if (!parsed.has_value()) {
            std::println(stderr, "{}: the value of --set {} could not be read: {}", kProgram, setting.key, parsed.error().message);
            return std::nullopt;
        }
        if (parsed->size() != 1UZ || std::string_view(parsed->begin()->first) != setting.key) {
            std::println(stderr, "{}: --set takes one value for one key, and the value of --set {} holds more than one key", kProgram, setting.key);
            return std::nullopt;
        }
        gr::property_map* target = setting.block.empty() ? std::addressof(staged.scheduler) : std::addressof(staged.blocks[setting.block]);
        for (const auto& [parsedKey, parsedValue] : *parsed) {
            target->insert_or_assign(parsedKey, parsedValue);
        }
    }
    return staged;
}

// Whether the registry holds a scheduler under `key`. A key it does not hold is reported with the keys it holds.
[[nodiscard]] bool schedulerIsRegistered(const gr::PluginLoader& loader, std::string_view key) {
    const std::vector<std::string> registered = loader.availableSchedulers();
    if (std::ranges::contains(registered, key)) {
        return true;
    }
    std::println(stderr, "{}: no scheduler is registered as {}; the registered schedulers are:", kProgram, key);
    for (const std::string& available : registered) {
        std::println(stderr, "{}:   {}", kProgram, available);
    }
    return false;
}

// A scheduler is unusable when it cannot be built at all or when it refuses the settings --set gave it.
enum class SchedulerProblem { cannotBuild, refusesSettings };

// The scheduler registered under `key` built over an empty graph with these settings, as a handle, or the problem
// that stops it. The probe's runtime is discarded when the function returns.
//
// A setting the scheduler will not take is a command-line problem, and the answer is wanted before the graph file is
// read. The scheduler is built without the settings first, to report a name it does not declare in the words of
// `--help`. It is then built with the settings, which refuses a value that does not convert or falls outside its
// limits. Without settings the first build is the only one.
[[nodiscard]] std::expected<gr::BlockHandle, SchedulerProblem> probeScheduler(const std::shared_ptr<gr::PluginLoader>& loader, std::string_view key, const gr::property_map& settings) {
    const auto probeOf = [&loader, key](const gr::property_map& parameters) -> std::expected<gr::Runtime, gr::RuntimeError> {
        std::expected<gr::RuntimeGraph, gr::RuntimeError> empty = gr::RuntimeGraph::fromYaml(loader, "blocks: []\n");
        if (!empty.has_value()) {
            return std::unexpected(empty.error());
        }
        return gr::Runtime::create(std::move(*empty), key, parameters);
    };

    const std::expected<gr::Runtime, gr::RuntimeError> bare = probeOf({});
    if (!bare.has_value()) {
        std::println(stderr, "{}: the scheduler {} could not be built: {}", kProgram, key, bare.error().message);
        return std::unexpected(SchedulerProblem::cannotBuild);
    }
    if (settings.empty()) {
        return bare->scheduler();
    }
    const std::set<std::string> declared = bare->scheduler().writableMembers();
    for (const auto& [name, value] : settings) {
        if (!declared.contains(std::string(name.begin(), name.end()))) {
            std::println(stderr, "{}: the scheduler declares no setting named '{}'", kProgram, std::string_view(name.data(), name.size()));
            return std::unexpected(SchedulerProblem::refusesSettings);
        }
    }
    const std::expected<gr::Runtime, gr::RuntimeError> taken = probeOf(settings);
    if (!taken.has_value()) {
        std::println(stderr, "{}: a scheduler setting could not be applied: {}", kProgram, taken.error().message);
        return std::unexpected(SchedulerProblem::refusesSettings);
    }
    return taken->scheduler();
}

// the scheduler chosen and each setting --set gave it, read back from the scheduler
void reportScheduler(const gr::BlockHandle& scheduler, std::string_view key, const gr::property_map& settings) {
    std::println(stderr, "{}: scheduler {}", kProgram, key);
    for (const auto& [name, _] : settings) {
        if (const std::optional<gr::pmt::Value> value = scheduler.get(std::string(name.begin(), name.end())); value.has_value()) {
            std::println(stderr, "{}: scheduler setting {} = {}", kProgram, std::string_view(name.data(), name.size()), *value);
        }
    }
}

// The blocks --show names, or nothing when one of them is not in the graph; each missing name is reported. A handle
// keeps its block when the graph moves into the runtime.
[[nodiscard]] std::optional<std::vector<gr::BlockHandle>> shownBlocks(const gr::RuntimeGraph& graph, const std::vector<std::string>& names) {
    std::vector<gr::BlockHandle> shown;
    bool                         allFound = true;
    for (const std::string& wanted : names) {
        std::expected<gr::BlockHandle, gr::RuntimeError> found = graph.find(wanted);
        if (found.has_value()) {
            shown.push_back(std::move(*found));
            continue;
        }
        allFound                 = false;
        const bool nameIsInGraph = std::ranges::any_of(graph.blocks(), [&wanted](const gr::BlockHandle& block) { return block.name() == wanted || block.uniqueName() == wanted; });
        if (nameIsInGraph) {
            std::println(stderr, "{}: {}", kProgram, found.error().message);
        } else {
            std::println(stderr, "{}: the graph holds no block named {}", kProgram, wanted);
        }
    }
    if (!allFound) {
        return std::nullopt;
    }
    return shown;
}

// the errors a run reported: how many, and the first
struct ReportedErrors {
    std::size_t count = 0UZ;
    // the first error's line, worded as the run's result words it, so a result that repeats it is not printed again
    std::string first;
};

// The endpoint of the scheduler's report that a block ended the run with an error status. The block's own report
// carries the reason, and the run's result holds it.
constexpr std::string_view kRunEndedByBlockEndpoint = "error in the run";

// Takes the errors reported since the last call into `errors` and prints the first of the run on one line, without the
// location the report carries. The later ones are counted. The scheduler's notice that a block ended the run is not an
// error of its own.
void takeReportedErrors(gr::Runtime& runtime, ReportedErrors& errors) {
    const std::string scheduler = runtime.scheduler().uniqueName();
    for (std::vector<gr::RuntimeEvent> events = runtime.pollEvents(); !events.empty(); events = runtime.pollEvents()) {
        for (const gr::RuntimeEvent& event : events) {
            if (!event.isError || (event.source == scheduler && event.endpoint == kRunEndedByBlockEndpoint)) {
                continue;
            }
            if (errors.count++ > 0UZ) {
                continue;
            }
            if (event.source == scheduler) {
                std::println(stderr, "{}: the scheduler reports an error on '{}': {}", kProgram, event.endpoint, event.text);
                errors.first = event.text;
            } else {
                errors.first = std::format("block '{}' reports an error on '{}': {}", event.source, event.endpoint, event.text);
                std::println(stderr, "{}: {}", kProgram, errors.first);
            }
        }
    }
}

constexpr std::chrono::milliseconds kPollInterval{20};

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

    const std::optional<StagedSettings> staged = stagedSettingsOf(options.settings);
    if (!staged.has_value()) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }

    // registers the program's own schedulers before the loader runs. A library the loader opens can then replace one
    // under the same key.
    gr::detail::registerShippedSchedulers();
    const std::vector<std::string> directories = searchDirectories(options.pluginDirectories);
    std::vector<std::string>       keysBefore  = gr::globalBlockRegistry().keys();
    std::ranges::sort(keysBefore);
    const auto loader = std::make_shared<gr::PluginLoader>(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
    if (options.verbose) {
        reportPlugins(*loader, directories, keysBefore);
    }

    // The scheduler key and its settings are checked before the graph file is read. A key the registry does not hold or
    // a setting the scheduler refuses ends the program with status 2.
    if (!schedulerIsRegistered(*loader, options.scheduler)) {
        return 2;
    }
    const std::expected<gr::BlockHandle, SchedulerProblem> probed = probeScheduler(loader, options.scheduler, staged->scheduler);
    if (!probed.has_value()) {
        if (probed.error() == SchedulerProblem::refusesSettings) {
            std::print(stderr, "{}", kUsage);
        }
        return 2;
    }
    if (options.verbose) {
        reportScheduler(*probed, options.scheduler, staged->scheduler);
    }

    const std::optional<std::string> document = readGraph(options.graph);
    if (!document.has_value()) {
        return 1;
    }

    // Each block reads its --set values with the graph file's own. Its start() sees them. The reader refuses a graph
    // file that names a key nothing supplies or a port that is not there. It also refuses a --set key the block does not
    // declare, a value of the wrong type, and a --set for a block the file does not hold. Its message is printed as it
    // arrives.
    std::expected<gr::RuntimeGraph, gr::RuntimeError> graph = gr::RuntimeGraph::fromYaml(loader, *document, staged->blocks);
    if (!graph.has_value()) {
        std::println(stderr, "{}: {} did not load: {}", kProgram, options.graph, graph.error().message);
        return 1;
    }

    std::optional<std::vector<gr::BlockHandle>> shown = shownBlocks(*graph, options.show);
    if (!shown.has_value()) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::expected<gr::Runtime, gr::RuntimeError> runtime = gr::Runtime::create(std::move(*graph), options.scheduler, staged->scheduler);
    if (!runtime.has_value()) {
        std::println(stderr, "{}: the scheduler {} could not be built: {}", kProgram, options.scheduler, runtime.error().message);
        return 2;
    }
    if (const std::expected<void, gr::RuntimeError> started = runtime->start(); !started.has_value()) {
        std::println(stderr, "{}: the graph stopped: {}", kProgram, started.error().message);
        return 1;
    }

    const auto     start        = std::chrono::steady_clock::now();
    const auto     boundReached = [&options, start] { return options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.seconds; };
    ReportedErrors errors;
    bool           endedItself = false;
    while (!endedItself && errors.count == 0UZ && !gStopRequested.load(std::memory_order_relaxed) && !boundReached()) {
        endedItself = runtime->waitFor(kPollInterval);
        takeReportedErrors(*runtime, errors);
    }
    const bool stoppedOnError = errors.count > 0UZ && !endedItself;
    if (!endedItself) {
        // the stop is requested once, and each later call waits for that same stop
        while (!runtime->stopFor(kPollInterval)) {
            takeReportedErrors(*runtime, errors);
        }
    }
    takeReportedErrors(*runtime, errors);
    if (errors.count > 1UZ) {
        std::println(stderr, "{}: {} more error(s) followed the first", kProgram, errors.count - 1UZ);
    }

    // a run the first error failed returns that error as its result, and the line is printed once
    const std::expected<void, gr::RuntimeError> result = runtime->result();
    if (!result.has_value() && result.error().message != errors.first) {
        std::println(stderr, "{}: the graph stopped: {}", kProgram, result.error().message);
    }
    for (std::size_t i = 0UZ; i < shown->size(); ++i) {
        showSettings(options.show[i], (*shown)[i]);
    }
    std::println(stderr, "{}: {}", kProgram, endedItself ? "the graph ended on its own" : (stoppedOnError ? "the graph was stopped on the first error reported" : "the graph was stopped before it ended"));
    return result.has_value() && errors.count == 0UZ ? 0 : 1;
}
