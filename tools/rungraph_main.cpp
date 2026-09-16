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
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/formatter/ValueFormatter.hpp>

namespace {

constexpr std::string_view kProgram = "rungraph";

constexpr std::string_view kUsage = R"(rungraph - run a GNU Radio 4 graph file

Usage: rungraph --graph <file> [options]

  --graph <file>     the graph to run, in the GRC YAML dialect; - reads it from standard input
  --plugin-dir <dir> a directory to load plugins and block libraries from; repeatable
  --seconds <s>      stop the graph after <s> seconds; without it the run ends when the graph does
  --show <name>      print the settings of the block named <name> when the run ends; repeatable
  --verbose          list what each plugin directory loaded and the keys it brought
  --help, -h         this text

The blocks come from the directories named by --plugin-dir, from GNURADIO4_PLUGIN_DIRECTORIES,
the colon-separated list the framework's own plugin loader reads, and from the plugin directory
of this installation, which is always searched. A directory named twice is searched once.

A settings map holds what the last refresh put there, so the settings --show prints are read
after the run has ended and the block has been asked to refresh them: a counter a block keeps
as a readable member is then current as of the last sample it processed.

SIGINT and SIGTERM stop the graph as a --seconds bound does.

Exit status is 0 when the run stopped cleanly, 1 when the graph could not be read, loaded or
run, and 2 when the command line could not be used.
)";

std::atomic<bool> gStopRequested{false};

extern "C" void onSignal(int) { gStopRequested.store(true, std::memory_order_relaxed); }

struct Options {
    std::string              graph; // the graph file, or "-" for standard input
    std::vector<std::string> pluginDirectories;
    std::vector<std::string> show;          // the blocks whose settings are printed when the run ends
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
        if (argument != "--graph" && argument != "--plugin-dir" && argument != "--show" && argument != "--seconds") {
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

    gr::scheduler::Simple<> scheduler;
    if (!scheduler.exchange(std::move(**graph)).has_value()) {
        std::println(stderr, "{}: the scheduler refused the graph", kProgram);
        return 1;
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
