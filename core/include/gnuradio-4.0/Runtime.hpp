#ifndef GNURADIO_RUNTIME_HPP
#define GNURADIO_RUNTIME_HPP

/**
 * @brief Build and run a GNU Radio 4 flow graph without naming a framework template.
 *
 * Blocks and schedulers are selected by a string. Parameters and ports are selected by name. This header therefore
 * instantiates no `Block<T>`, `CtxSettings<T>`, `Port<T, ...>` or `SchedulerBase<...>`. Everything it declares is
 * compiled once into `gnuradio-core`. The typed API remains the choice where compile-time port checking matters.
 *
 * A consumer is built with the same compiler family as the installed `gnuradio-core`.
 */

#include <gnuradio-4.0/Export.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Value.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gr {

/// Declared and not defined here. `RuntimeGraph::add` takes a block the caller built, which needs `BlockModel.hpp`.
class BlockModel;

/// Declared and not defined here. A program that builds its own loader includes `PluginLoader.hpp`.
class PluginLoader;

struct GNURADIO_EXPORT RuntimeError {
    std::string   message;
    std::string   where;    // the rendered source location of a framework error, or the method that refused
    std::uint64_t time = 0; // ns since epoch when the framework reported it, 0 otherwise
};

struct GNURADIO_EXPORT EdgeSpec {
    std::size_t minBufferSize = 0; // 0 selects the framework's default for the port's value type
};

/**
 * @brief One block of a graph built by name.
 *
 * A handle is copyable and exposes no framework type. It keeps its block and its graph's plugin loader alive. It stays
 * usable after its graph moves into a `Runtime` and after that Runtime is destroyed.
 */
class GNURADIO_EXPORT BlockHandle {
public:
    BlockHandle() noexcept = default;

    [[nodiscard]] bool        valid() const noexcept;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] std::string uniqueName() const;
    [[nodiscard]] std::string typeName() const;

    /// Stages parameters for the block to apply. A running block applies them at its next work call, and a block that
    /// is not running applies them when its run starts. A key the block does not declare is an error naming the key,
    /// and the call then stages nothing. A value that does not convert to its setting's type is an error carrying the
    /// block's reason. A value outside the setting's limits is staged, and the block drops it without a report when it
    /// applies it. The scheduler's handle refuses every call: a scheduler takes its settings in `Runtime::create()`.
    [[nodiscard]] std::expected<void, RuntimeError> setStaged(const property_map& parameters);

    /// The block's settings as of its last settings change, or of the end of its last run.
    [[nodiscard]] property_map              get(std::span<const std::string> keys = {}) const;
    [[nodiscard]] std::optional<pmt::Value> get(const std::string& key) const;

    /// The keys the block declares as settable. The scheduler's handle lists its own keys, which only `Runtime::create()`
    /// takes.
    [[nodiscard]] std::set<std::string> writableMembers() const;

    [[nodiscard]] property_map metaInformation() const;

    /// The names `RuntimeGraph::connect()` accepts for the block's ports. An element of a port collection is named
    /// `base#index`, such as `in#1`.
    [[nodiscard]] std::vector<std::string> inputPortNames() const;
    [[nodiscard]] std::vector<std::string> outputPortNames() const;

private:
    friend class RuntimeGraph;
    friend class Runtime;
    explicit BlockHandle(std::shared_ptr<void> model, std::shared_ptr<PluginLoader> loader, bool isScheduler = false) noexcept;

    std::shared_ptr<PluginLoader> _loader; // declared first to outlive a block built from one of its plugins
    std::shared_ptr<void>         _model;  // aliases the graph's shared_ptr<BlockModel>
    bool                          _isScheduler = false;
};

/**
 * @brief A flow graph built by name, before it runs.
 *
 * A graph takes its blocks and its scheduler from a plugin loader: the process's loader, `gr::globalPluginLoader()`,
 * or the loader `fromYaml()` is given. `Runtime::create()` takes the graph over.
 */
class GNURADIO_EXPORT RuntimeGraph {
public:
    enum class Recursive : bool { No = false, Yes = true };

    /// An empty graph over the process's plugin loader.
    RuntimeGraph();

    ~RuntimeGraph();
    RuntimeGraph(RuntimeGraph&&) noexcept;
    RuntimeGraph& operator=(RuntimeGraph&&) noexcept;
    RuntimeGraph(const RuntimeGraph&)            = delete;
    RuntimeGraph& operator=(const RuntimeGraph&) = delete;

    /// Builds a block of `type` through the graph's loader, names it and adds it. A type the loader does not know is an
    /// error naming the type. A known type that cannot be built is an error with the loader's reason. A parameter the
    /// block does not declare or refuses is an error carrying the block's refusal. A scheduler type is an error: a nested
    /// scheduler comes from a graph document through `fromYaml()`.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> emplace(std::string_view type, std::string_view name, property_map parameters = {});

    /// Adds a block the caller built, which can keep its own typed pointer. `name` renames the block when it is not
    /// empty. A block whose settings refuse a key or a value when the graph initializes it is an error carrying the
    /// reason.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> add(std::shared_ptr<BlockModel> block, std::string_view name = {});

    /// The blocks of the graph and of its nested graphs, or with `Recursive::No` those of the graph alone.
    [[nodiscard]] std::vector<BlockHandle> blocks(Recursive recursive = Recursive::Yes) const;

    /// The block whose unique name is `name`, else the one block whose name is `name`. A block `fromYaml()` read from a
    /// document entry that carries a `unique_name` also answers to that name. A name no block carries and a name two
    /// blocks share are errors.
    [[nodiscard]] std::expected<BlockHandle, RuntimeError> find(std::string_view name, Recursive recursive = Recursive::Yes) const;

    /// Connects two ports by the names `BlockHandle::inputPortNames()` and `outputPortNames()` list. A name the block
    /// does not have is an error that lists the names it has.
    [[nodiscard]] std::expected<void, RuntimeError> connect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
        const BlockHandle& destinationBlock, std::string_view destinationPort, EdgeSpec edge = {});

    /// Loads a graph document as `gr::loadGrc` does, through `loader`, the process's loader when it is null.
    ///
    /// The graph and a Runtime created from it share ownership of the loader. A document the reader refuses is an error
    /// carrying the reader's message, headed by the line and column for a document that does not parse. A block type
    /// the loader does not know is an error naming the type.
    ///
    /// `overrides` puts the caller's settings in place of the document's. Each key names a block at the document's top
    /// level by its `unique_name` or name. Its values replace those of the block's parameters before the block reads
    /// them. A name no block carries, a name two blocks share and a key a block does not declare are errors.
    [[nodiscard]] static std::expected<RuntimeGraph, RuntimeError> fromYaml(std::shared_ptr<PluginLoader> loader, std::string_view document, const std::map<std::string, property_map, std::less<>>& overrides = {});

private:
    friend class Runtime;
    struct Impl;
    explicit RuntimeGraph(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

/**
 * @brief A scheduler running a graph built by name.
 *
 * A Runtime holds one run of its graph at a time. `runAndWait()` makes the run on the calling thread, and `start()`
 * makes it on a thread of its own. When a run ends, each block's settings are refreshed from its members.
 * `BlockHandle::get()` then reads what the run left. The Runtime keeps the last run's result. The destructor stops the
 * run in progress and waits for it to end.
 *
 * The member functions other than the destructor and the move assignment may be called from several threads at once.
 * A thread of the run, such as a block's or a callback's, requests the stop with
 * `stopFor(std::chrono::nanoseconds::zero())`, which does not wait. On such a thread `stop()`, `waitFor()` and the
 * destructor wait for a run that cannot end, and `stopFor()` with a timeout returns false at that timeout.
 */
class GNURADIO_EXPORT Runtime {
public:
    enum class State : std::uint8_t { Idle, Initialized, Running, RequestedPause, Paused, RequestedStop, Stopped, Error };

    static constexpr std::string_view kDefaultScheduler = "gr::scheduler::Simple<singleThreaded>";

    /// Builds the scheduler `type` through the graph's loader with `schedulerParameters` and hands it the graph. A
    /// parameter the scheduler does not declare is an error. So is a value its constructor refuses. `graph` is consumed
    /// when a Runtime is returned and left as it was on an error. The shipped `Simple` schedulers are registered in
    /// `gr::globalSchedulerRegistry()` alone, and a loader over another scheduler registry does not find them.
    [[nodiscard]] static std::expected<Runtime, RuntimeError> create(RuntimeGraph&& graph, std::string_view type = kDefaultScheduler, property_map schedulerParameters = {});

    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Runs to completion on the calling thread and returns the run's result. Refused while a run is in progress.
    [[nodiscard]] std::expected<void, RuntimeError> runAndWait();

    /// Runs the graph as `runAndWait()` does, on a thread the Runtime owns. Returns once the scheduler is running or the
    /// run has ended. Refused while a run is in progress. `result()` reports how the run ended.
    [[nodiscard]] std::expected<void, RuntimeError> start();

    /// Requests the stop of the run in progress, by `start()` or by `runAndWait()` on another thread, and returns once
    /// the run has ended. Without a run in progress it does nothing.
    void stop();

    /// Requests the stop as `stop()` does and returns once the run has ended or `timeout` has passed. Returns whether
    /// the run has ended, true without a run in progress. The request stands after a return at the timeout, and a later
    /// call waits for the same stop.
    [[nodiscard]] bool stopFor(std::chrono::nanoseconds timeout);

    /// Returns once no run is in progress or `timeout` has passed, and whether no run is in progress. A timeout of
    /// `std::chrono::nanoseconds::max()` waits for the end of the run.
    [[nodiscard]] bool waitFor(std::chrono::nanoseconds timeout) const;

    /// The last run's result, as `runAndWait()` returns it. Success before the first run, and an error while a run is
    /// in progress.
    [[nodiscard]] std::expected<void, RuntimeError> result() const;

    [[nodiscard]] State state() const noexcept;

    /// The scheduler as a block handle. Its settings, such as `timeout_ms`, are read like any block's. The handle keeps
    /// the scheduler and the graph's plugin loader alive after the Runtime is destroyed, and it is invalid on a
    /// moved-from Runtime.
    [[nodiscard]] BlockHandle scheduler() const;

private:
    struct Impl;
    explicit Runtime(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace gr

#endif // GNURADIO_RUNTIME_HPP
