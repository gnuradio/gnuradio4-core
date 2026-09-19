// rungraph - load a graph file, run it, and report what the run was asked to report.
//
// No block, setting or connection is named in this file: the graph file names the blocks, the plugin directories
// supply them, and the framework's own loader builds the graph, so a chain that changes needs no program rebuilt.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/FusedRun.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

namespace {

constexpr std::string_view kProgram = "rungraph";

constexpr std::string_view kUsage = R"(rungraph - run a GNU Radio 4 graph file

Usage: rungraph --graph <file> [options]

  --graph <file>     the graph to run, in the GRC YAML dialect; - reads it from standard input
  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --seconds <s>      stop the graph after <s> seconds; without it the run ends when the graph does
  --show <name>      print the settings of the block named <name> when the run ends; repeatable
  --set, -s <key>=<value>
                     set one setting before the run; repeatable
  --fuse[=<samples>] run fused, with <samples> per fused chunk where the count is given
  --verbose          list what each plugin directory loaded and the keys it brought, then the
                     fusion plan of the run
  --help, -h         this text

The blocks come from the directories named by --plugin-dir, from GNURADIO4_PLUGIN_DIRECTORIES,
the colon-separated list the framework's own plugin loader reads, and from the plugin directory
of this installation, which is always searched. A directory named twice is searched once.

A settings map holds what the last refresh put there, so the settings --show prints are read
after the run has ended and the block has been asked to refresh them: a counter a block keeps
as a readable member is then current as of the last sample it processed.

A bare key of --set names a setting of the scheduler, and a key of the form <block>.<key> names a
setting of the block --show matches by that name. The split is at the last dot before the '=', so
a block name may hold a dot and a setting key never does. rungraph reads the value the way a graph
file's parameter value is read, so a type tag applies: -s enable_fusion=true, -s
'shift.frequency_shift=!!float32 -100000'. The last --set of a key wins. --fuse sets
enable_fusion=true, and --fuse=<samples> sets fusion_chunk_samples to that count as well; without
the count the chunk size stays derived.

--verbose prints the fusion plan after the plugin listing: one line per fused run, naming its
member blocks in the order the run drives them, then the number of runs and the number of blocks
the scheduler still calls one at a time. Fusion runs only where enable_fusion asks for it.

SIGINT and SIGTERM stop the graph as a --seconds bound does.

Exit status is 0 when the run stopped cleanly, 1 when the graph could not be read, loaded or run
and when a --set or --show names a block or a block setting the graph does not hold, and 2 when
the command line could not be used, a scheduler setting the scheduler does not declare included.
)";

std::atomic<bool> gStopRequested{false};

extern "C" void onSignal(int) { gStopRequested.store(true, std::memory_order_relaxed); }

// one --set, or one of the pair --fuse=<samples> stands for
struct Setting {
    std::string block; // the block the setting belongs to; empty names the scheduler
    std::string key;
    std::string value; // the text after the '=', read as a graph file's parameter value is read
};

struct Options {
    std::string              graph; // the graph file, or "-" for standard input
    std::vector<std::string> pluginDirectories;
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
        // --fuse carries its count in the argument itself, so that the grammar holds one form and a bare --fuse
        // cannot swallow the next argument
        if (argument == "--fuse" || argument.starts_with("--fuse=")) {
            options.settings.emplace_back(std::string(), "enable_fusion", "true");
            if (argument != "--fuse") {
                const std::string_view samples = argument.substr(std::string_view("--fuse=").size());
                if (samples.empty() || std::ranges::any_of(samples, [](char c) { return c < '0' || c > '9'; })) {
                    std::println(stderr, "{}: --fuse takes a count of samples per chunk, not '{}'", kProgram, samples);
                    return std::nullopt;
                }
                options.settings.emplace_back(std::string(), "fusion_chunk_samples", std::string(samples));
            }
            ++index;
            continue;
        }
        // the option is recognized before its value is asked for, so that an unknown option in the last position is
        // reported as unknown rather than as one missing a value
        if (argument != "--graph" && argument != "--plugin-dir" && argument != "--show" && argument != "--seconds" && argument != "--set" && argument != "-s") {
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

// One block's readable settings, one `name: key = value` line each, in key order.
//
// A settings map holds what the last refresh put there, and the framework refreshes one when a settings change is
// applied, so a block whose readable members move while it runs reports what it started with until it is asked. The
// run is over by the time this is called, which is what makes such a member readable at all.
void showSettings(gr::BlockModel& block) {
    block.settings().updateActiveParameters();
    std::vector<std::pair<std::string, std::string>> lines;
    for (const auto& [key, value] : block.settings().get()) {
        lines.emplace_back(std::string(key.begin(), key.end()), std::format("{}", value));
    }
    std::ranges::sort(lines);
    for (const auto& [key, value] : lines) {
        std::println("{}: {} = {}", block.name(), key, value);
    }
    std::fflush(stdout);
}

// the scheduler's settings and one map per block named, each in the order the command line gave them
struct StagedSettings {
    gr::property_map                                      scheduler;
    std::vector<std::pair<std::string, gr::property_map>> blocks;
};

// The settings the --set and --fuse arguments stand for, or nothing when one of the values cannot be read.
//
// Each pair becomes a one-entry YAML document and the framework's own reader gives the value its type, so the text
// after the '=' means here what the same text means as a parameter of a graph file: a bare `true` is a boolean, a bare
// number an integer, and a tagged `!!float32 1.5` a float. A later setting of a key overwrites an earlier one.
[[nodiscard]] std::optional<StagedSettings> stagedSettingsOf(const std::vector<Setting>& settings) {
    StagedSettings staged;
    for (const Setting& setting : settings) {
        const auto parsed = gr::pmt::yaml::deserialize(std::format("{}: {}", setting.key, setting.value));
        if (!parsed.has_value()) {
            std::println(stderr, "{}: the value of --set {} could not be read: {}", kProgram, setting.key, parsed.error().message);
            return std::nullopt;
        }
        gr::property_map* target = std::addressof(staged.scheduler);
        if (!setting.block.empty()) {
            const auto found = std::ranges::find(staged.blocks, setting.block, &std::pair<std::string, gr::property_map>::first);
            target           = found != staged.blocks.end() ? std::addressof(found->second) : std::addressof(staged.blocks.emplace_back(setting.block, gr::property_map{}).second);
        }
        for (const auto& [parsedKey, parsedValue] : *parsed) {
            target->insert_or_assign(parsedKey, parsedValue);
        }
    }
    return staged;
}

// Applies the settings to the scheduler, before the graph reaches it and before the plan is built.
//
// The name is checked against the settings the scheduler declares first: a key outside that set is filed as meta
// information by the settings map itself, and the run would then proceed as if the caller had asked for nothing.
[[nodiscard]] bool applySchedulerSettings(gr::scheduler::Simple<>& scheduler, const gr::property_map& settings) {
    const std::set<std::string>& declared = scheduler.settings().writableMembers();
    for (const auto& [key, value] : settings) {
        if (!declared.contains(std::string(key.begin(), key.end()))) {
            std::println(stderr, "{}: the scheduler declares no setting named '{}'", kProgram, std::string_view(key.data(), key.size()));
            return false;
        }
    }
    try {
        if (const gr::property_map refused = scheduler.settings().set(settings); !refused.empty()) {
            std::println(stderr, "{}: the scheduler refused {}", kProgram, refused);
            return false;
        }
    } catch (const std::exception& error) {
        std::println(stderr, "{}: a scheduler setting could not be applied: {}", kProgram, error.what());
        return false;
    }
    std::ignore = scheduler.settings().activateContext();
    std::ignore = scheduler.settings().applyStagedParameters();
    return true;
}

// Whether a scheduler takes these settings, asked of one built for the question alone.
//
// The scheduler that runs the graph holds the graph's blocks and has to be destroyed before the libraries those blocks
// come from are unloaded, so it is built after the plugin loader and cannot answer this. A setting the scheduler will
// not take is a command line problem, and the answer is wanted before the graph file is read.
[[nodiscard]] bool schedulerTakesSettings(const gr::property_map& settings) {
    gr::scheduler::Simple<> probe;
    return applySchedulerSettings(probe, settings);
}

// Applies each block's settings by the call the graph file's own parameters take, so a value set here is in force for
// the first sample and is the value --show reports at the end. The block and the key are both looked up first: an
// unknown key would otherwise be filed as meta information and the run would proceed as if nothing had been asked.
[[nodiscard]] bool applyBlockSettings(gr::Graph& graph, const std::vector<std::pair<std::string, gr::property_map>>& blocks) {
    for (const auto& [name, settings] : blocks) {
        std::shared_ptr<gr::BlockModel> found;
        gr::graph::forEachBlock<gr::block::Category::NormalBlock>(graph, [&found, &name](const std::shared_ptr<gr::BlockModel>& block) {
            if (block->name() == name) {
                found = block;
            }
        });
        if (found == nullptr) {
            std::println(stderr, "{}: --set names {}, and the graph holds no block named {}", kProgram, name, name);
            return false;
        }
        const std::set<std::string>& declared = found->settings().writableMembers();
        for (const auto& [key, value] : settings) {
            if (!declared.contains(std::string(key.begin(), key.end()))) {
                std::println(stderr, "{}: the block {} declares no setting named '{}'", kProgram, name, std::string_view(key.data(), key.size()));
                return false;
            }
        }
        try {
            found->settings().loadParametersFromPropertyMap(settings);
        } catch (const std::exception& error) {
            std::println(stderr, "{}: the settings of block {} could not be applied: {}", kProgram, name, error.what());
            return false;
        }
        if (found->settings().activateContext() == std::nullopt) {
            std::println(stderr, "{}: the settings of block {} could not be activated", kProgram, name);
            return false;
        }
    }
    return true;
}

// The fused runs the scheduler planned: one line each, naming the members in the order the run drives them, then the
// two totals. Each run holds one entry of the job list and its members hold none, so every other entry of a normal
// block is a block the scheduler calls on its own.
void reportFusionPlan(gr::scheduler::Simple<>& scheduler) {
    if (!scheduler.enable_fusion.value) {
        std::println(stderr, "{}: fusion is off", kProgram);
        return;
    }
    std::size_t nRuns = 0UZ;
    for (const std::vector<gr::fusion::RunPlan>& job : scheduler.fusionPlan()) {
        for (const gr::fusion::RunPlan& run : job) {
            std::string members;
            for (const std::shared_ptr<gr::BlockModel>& member : run.members) {
                members += members.empty() ? std::string(member->name()) : std::format(" -> {}", member->name());
            }
            ++nRuns;
            std::println(stderr, "{}: fused run {}: {} ({} samples per chunk)", kProgram, nRuns, members, run.chunkSamples);
        }
    }
    std::size_t nUnfused = 0UZ;
    for (const std::vector<std::shared_ptr<gr::BlockModel>>& job : *scheduler.jobs()) {
        for (const std::shared_ptr<gr::BlockModel>& entry : job) {
            if (entry->blockCategory() == gr::block::Category::NormalBlock && dynamic_cast<const gr::fusion::FusedRun*>(entry.get()) == nullptr) {
                ++nUnfused;
            }
        }
    }
    std::println(stderr, "{}: {} fused run(s), {} block(s) called one at a time", kProgram, nRuns, nUnfused);
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

    // the scheduler's settings are settled before the graph is read, so that one it will not take is reported as the
    // command line problem it is rather than after a file has been loaded
    const std::optional<StagedSettings> staged = stagedSettingsOf(options.settings);
    if (!staged.has_value() || !schedulerTakesSettings(staged->scheduler)) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }

    const std::optional<std::string> document = readGraph(options.graph);
    if (!document.has_value()) {
        return 1;
    }

    const std::vector<std::string> directories = searchDirectories(options.pluginDirectories);
    std::vector<std::string>       keysBefore  = gr::globalBlockRegistry().keys();
    std::ranges::sort(keysBefore);
    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
    if (options.verbose) {
        reportPlugins(loader, directories, keysBefore);
    }

    // the loader refuses a graph file for a key nothing supplies, a setting a block does not declare, a value of the
    // wrong type or a port that is not there, and its message is printed as it arrives
    std::optional<gr::meta::indirect<gr::Graph>> graph;
    try {
        graph.emplace(gr::loadGrc(loader, *document));
    } catch (const std::exception& error) {
        std::println(stderr, "{}: {} did not load: {}", kProgram, options.graph, error.what());
        return 1;
    }

    if (!applyBlockSettings(**graph, staged->blocks)) {
        return 1;
    }

    // the blocks --show names are found before the graph is handed to the scheduler, because the handles stay valid
    // over the move and the graph itself does not
    std::vector<std::shared_ptr<gr::BlockModel>> shown;
    bool                                         allFound = true;
    for (const std::string& wanted : options.show) {
        std::shared_ptr<gr::BlockModel> found;
        gr::graph::forEachBlock<gr::block::Category::NormalBlock>(**graph, [&found, &wanted](const std::shared_ptr<gr::BlockModel>& block) {
            if (block->name() == wanted) {
                found = block;
            }
        });
        if (found == nullptr) {
            std::println(stderr, "{}: the graph holds no block named {}", kProgram, wanted);
            allFound = false;
            continue;
        }
        shown.push_back(std::move(found));
    }
    if (!allFound) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // the scheduler is built after the loader, so that it and the blocks it holds are destroyed while the libraries
    // they came from are still open
    gr::scheduler::Simple<> scheduler;
    if (!applySchedulerSettings(scheduler, staged->scheduler)) {
        return 1;
    }
    if (!scheduler.exchange(std::move(**graph)).has_value()) {
        std::println(stderr, "{}: the scheduler refused the graph", kProgram);
        return 1;
    }

    // the job lists and the fusion plan are built on the way into the initialized state, so the run is planned here
    // and runAndWait() below finds the scheduler ready and starts it
    if (const auto prepared = scheduler.changeStateTo(gr::lifecycle::State::INITIALISED); !prepared.has_value()) {
        std::println(stderr, "{}: the graph could not be prepared: {}", kProgram, prepared.error().message);
        return 1;
    }
    if (options.verbose) {
        reportFusionPlan(scheduler);
    }

    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::thread       runner([&scheduler, &finished, &failed] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "{}: the graph stopped: {}", kProgram, result.error().message);
            failed.store(true, std::memory_order_relaxed);
        }
        finished.store(true, std::memory_order_relaxed);
    });

    const auto start = std::chrono::steady_clock::now();
    while (!finished.load(std::memory_order_relaxed) && !gStopRequested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= options.seconds) {
            break;
        }
    }
    const bool endedItself = finished.load(std::memory_order_relaxed);
    if (!endedItself) {
        scheduler.requestStop();
    }
    runner.join();

    for (const std::shared_ptr<gr::BlockModel>& block : shown) {
        showSettings(*block);
    }
    std::println(stderr, "{}: {}", kProgram, endedItself ? "the graph ended on its own" : "the graph was stopped before it ended");
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
