#include <gnuradio-4.0/Runtime.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#include "ShippedSchedulers.hpp"

#include <algorithm>
#include <condition_variable>
#include <format>
#include <mutex>
#include <ranges>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace gr {

static_assert(Runtime::kDefaultScheduler == detail::kDefaultSchedulerKey);

static_assert(static_cast<std::uint8_t>(Runtime::State::Idle) == static_cast<std::uint8_t>(lifecycle::State::IDLE));
static_assert(static_cast<std::uint8_t>(Runtime::State::Initialized) == static_cast<std::uint8_t>(lifecycle::State::INITIALISED));
static_assert(static_cast<std::uint8_t>(Runtime::State::Running) == static_cast<std::uint8_t>(lifecycle::State::RUNNING));
static_assert(static_cast<std::uint8_t>(Runtime::State::RequestedPause) == static_cast<std::uint8_t>(lifecycle::State::REQUESTED_PAUSE));
static_assert(static_cast<std::uint8_t>(Runtime::State::Paused) == static_cast<std::uint8_t>(lifecycle::State::PAUSED));
static_assert(static_cast<std::uint8_t>(Runtime::State::RequestedStop) == static_cast<std::uint8_t>(lifecycle::State::REQUESTED_STOP));
static_assert(static_cast<std::uint8_t>(Runtime::State::Stopped) == static_cast<std::uint8_t>(lifecycle::State::STOPPED));
static_assert(static_cast<std::uint8_t>(Runtime::State::Error) == static_cast<std::uint8_t>(lifecycle::State::ERROR));

namespace {

BlockModel* modelOf(const std::shared_ptr<void>& handle) noexcept { return static_cast<BlockModel*>(handle.get()); }

std::shared_ptr<BlockModel> modelPtrOf(const std::shared_ptr<void>& handle) noexcept { return std::static_pointer_cast<BlockModel>(handle); }

std::uint64_t toNanoseconds(std::chrono::system_clock::time_point when) { //
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

RuntimeError toRuntimeError(const Error& error) { return RuntimeError{.message = error.message, .where = error.srcLoc(), .time = toNanoseconds(error.errorTime)}; }

RuntimeError localError(std::string message, std::string_view where) { return RuntimeError{.message = std::move(message), .where = std::string(where), .time = 0ULL}; }

// the loader given, or the process's own, which lives as long as the process and is held without ownership
std::shared_ptr<PluginLoader> loaderOrGlobal(std::shared_ptr<PluginLoader> loader) {
    if (loader != nullptr) {
        return loader;
    }
    return std::shared_ptr<PluginLoader>(std::shared_ptr<void>{}, std::addressof(gr::globalPluginLoader()));
}

// Waits on `condition` until `ended` holds or `timeout` has passed. A timeout too long to add to the clock's present
// time waits without a deadline.
template<typename TPredicate>
bool waitWithTimeout(std::condition_variable& condition, std::unique_lock<std::mutex>& lock, std::chrono::nanoseconds timeout, TPredicate ended) {
    using Clock                          = std::chrono::steady_clock;
    const Clock::time_point now          = Clock::now();
    const Clock::duration   clockTimeout = std::chrono::duration_cast<Clock::duration>(std::max(timeout, std::chrono::nanoseconds::zero()));
    if (clockTimeout >= Clock::time_point::max() - now) {
        condition.wait(lock, ended);
        return true;
    }
    return condition.wait_until(lock, now + clockTimeout, ended);
}

// A collection is listed element by element, as "base#index", the one spelling an edge to a collection can carry.
std::vector<std::string> portNamesOf(BlockModel& model, bool isInput) {
    model.initDynamicPorts();
    const BlockModel::DynamicPorts& ports = isInput ? model.dynamicInputPorts() : model.dynamicOutputPorts();

    std::vector<std::string> names;
    names.reserve(ports.size());
    for (const BlockModel::DynamicPortOrCollection& entry : ports) {
        if (const auto* collection = std::get_if<BlockModel::NamedPortCollection>(&entry); collection != nullptr) {
            for (std::size_t index = 0UZ; index < collection->ports.size(); ++index) {
                names.push_back(std::format("{}#{}", collection->name, index));
            }
        } else {
            names.push_back(BlockModel::portName(entry));
        }
    }
    return names;
}

std::string commaSeparated(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

// Resolves a port as the graph's edge resolution does. A name refused here would fail when the graph starts.
std::expected<void, RuntimeError> checkPortExists(BlockModel& model, bool isInput, std::string_view portName, std::string_view where) {
    model.initDynamicPorts();
    const PortDefinition definition{std::string(portName)};
    const auto           port = isInput ? model.dynamicInputPort(definition) : model.dynamicOutputPort(definition);
    if (port.has_value()) {
        return {};
    }
    return std::unexpected(localError(std::format("block '{}' has no {} port '{}' -- it has [{}]", model.uniqueName(), isInput ? "input" : "output", portName, //
                                          commaSeparated(portNamesOf(model, isInput))),
        where));
}

} // namespace

struct RuntimeGraph::Impl {
    std::shared_ptr<PluginLoader> loader; // declared first to outlive the blocks it made
    std::unique_ptr<Graph>        graph;

    std::map<std::string, std::weak_ptr<BlockModel>, std::less<>> documentUniqueNames; // the `unique_name` of each top-level block fromYaml() read
};

struct Runtime::Impl {
    std::shared_ptr<PluginLoader>   loader; // the graph's share, declared first to outlive the scheduler and the blocks
    MsgPortIn                       events; // declared before the scheduler to outlive the scheduler writing to it
    std::shared_ptr<SchedulerModel> scheduler;
    std::shared_ptr<BlockModel>     schedulerBlock;

    std::mutex                        runMutex; // guards the members below
    std::condition_variable           runEnded;
    std::thread                       runThread;  // start()'s run, joined by the next start() and the destructor
    std::thread                       stopThread; // a run's stop request, joined by the next request and the destructor
    std::uint64_t                     nRunsStarted   = 0ULL;
    std::uint64_t                     nRunsReturned  = 0ULL; // runs whose scheduler call has returned
    std::uint64_t                     nRunsEnded     = 0ULL;
    std::uint64_t                     stoppedRun     = 0ULL;  // the last run whose stop was requested, 0 for none
    bool                              requestingStop = false; // a stop request has not returned
    std::expected<void, RuntimeError> lastResult;             // of the last run that ended, success before the first

    static constexpr std::chrono::milliseconds kStopPollInterval{10}; // requestStop() reads the scheduler's state this often
    static constexpr std::chrono::milliseconds kStartPollInterval{1}; // start() reads the scheduler's state this often

    Impl()                       = default;
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        std::ignore = stopRun(std::nullopt);
        if (runThread.joinable()) {
            runThread.join();
        }
        if (stopThread.joinable()) {
            stopThread.join();
        }
    }

    [[nodiscard]] bool runInProgress() const noexcept { return nRunsEnded != nRunsStarted; } // runMutex held

    // Runs the graph to its end on the calling thread, then refreshes each block's settings from its members. A failed
    // run returns the scheduler's error, and an exception the run throws becomes a RuntimeError.
    std::expected<void, RuntimeError> runToEnd(std::string_view where) {
        std::expected<void, RuntimeError> result;
        try {
            if (std::expected<void, Error> ran = scheduler->runAndWait(); !ran) {
                result = std::unexpected(toRuntimeError(ran.error()));
            }
        } catch (const gr::exception& error) {
            result = std::unexpected(localError(error.message, where));
        } catch (const std::exception& error) {
            result = std::unexpected(localError(error.what(), where));
        } catch (...) {
            result = std::unexpected(localError("the run threw an exception of unknown type", where));
        }
        if (Graph* graph = schedulerBlock->graph(); graph != nullptr) {
            gr::graph::forEachBlock<block::Category::All>(*graph, [](const std::shared_ptr<BlockModel>& block) { block->settings().updateActiveParameters(); });
        }
        drain();
        return result;
    }

    // A run ends once its scheduler call has returned and a stop request made of it has returned. No block of the run
    // is then still inside its stop() when the next run starts.
    void endRun(std::expected<void, RuntimeError> result) {
        std::unique_lock lock(runMutex);
        ++nRunsReturned;
        runEnded.notify_all();
        runEnded.wait(lock, [this] { return !requestingStop; });
        lastResult = std::move(result);
        ++nRunsEnded;
        runEnded.notify_all();
    }

    // Requests the stop of the run in progress, once for each run. Waits until that run has ended, for at most
    // `timeout` when one is given. Returns whether the run has ended. The request runs on stopThread. A block's slow
    // stop() then holds up that thread and not the caller. When the thread cannot start, the caller makes the request.
    [[nodiscard]] bool stopRun(std::optional<std::chrono::nanoseconds> timeout) {
        std::unique_lock    lock(runMutex);
        const std::uint64_t run   = nRunsStarted;
        const auto          ended = [this, run] { return nRunsEnded >= run; };
        if (!ended() && stoppedRun != run) {
            stoppedRun     = run;
            requestingStop = true;
            if (stopThread.joinable()) { // the thread of an earlier run's request, which does not take runMutex again
                stopThread.join();
            }
            try {
                stopThread = std::thread([this, run] {
                    std::unique_lock threadLock(runMutex);
                    requestStop(threadLock, run);
                });
            } catch (const std::system_error&) {
                requestStop(lock, run);
            }
        }
        if (!timeout.has_value()) {
            runEnded.wait(lock, ended);
            return true;
        }
        return waitWithTimeout(runEnded, lock, *timeout, ended);
    }

    // Requests the stop of run `run` once the scheduler is active, unless the run's scheduler call returns first. Until
    // then it reads the state the run before it left, or a state runAndWait() passes on its way to RUNNING. A
    // transition to REQUESTED_STOP from STOPPED has no effect, and from a state before RUNNING it can collide with the
    // run's own transition. The request makes the transition alone and does not call `SchedulerModel::stop()`, which
    // moves a scheduler that refuses the transition to ERROR. `lock` holds runMutex and releases it during the transition.
    void requestStop(std::unique_lock<std::mutex>& lock, std::uint64_t run) {
        const auto returned = [this, run] { return nRunsReturned >= run; };
        while (!returned()) {
            if (lifecycle::isActive(schedulerBlock->state())) {
                lock.unlock();
                std::ignore = schedulerBlock->changeStateTo(lifecycle::State::REQUESTED_STOP);
                lock.lock();
                break;
            }
            std::ignore = runEnded.wait_for(lock, kStopPollInterval, returned);
        }
        requestingStop = false;
        runEnded.notify_all();
    }

    // Consumes the scheduler's messages and drops them.
    void drain() {
        ReaderSpanLike auto messages = events.streamReader().get();
        std::ignore                  = messages.consume(messages.size());
    }
};

BlockHandle::BlockHandle(std::shared_ptr<void> model, std::shared_ptr<PluginLoader> loader, bool isScheduler) noexcept : _loader(std::move(loader)), _model(std::move(model)), _isScheduler(isScheduler) {}

bool BlockHandle::valid() const noexcept { return _model != nullptr; }

// The settings read takes the lock under which a running block applies a staged `name`.
std::string BlockHandle::name() const {
    if (!_model) {
        return {};
    }
    if (const std::optional<pmt::Value> named = modelOf(_model)->settings().get(std::string("name")); named.has_value()) {
        if (const std::string_view value = named->value_or(std::string_view{}); value.data() != nullptr) {
            return std::string(value);
        }
    }
    return std::string(modelOf(_model)->name());
}

std::string BlockHandle::uniqueName() const { return _model ? std::string(modelOf(_model)->uniqueName()) : std::string{}; }

std::string BlockHandle::typeName() const { return _model ? std::string(modelOf(_model)->typeName()) : std::string{}; }

std::expected<void, RuntimeError> BlockHandle::setStaged(const property_map& parameters) {
    constexpr std::string_view where = "BlockHandle::setStaged";
    if (!_model) {
        return std::unexpected(localError("block handle is empty", where));
    }
    if (_isScheduler) {
        return std::unexpected(localError("a scheduler applies no staged setting -- give its settings to Runtime::create() as schedulerParameters", where));
    }
    SettingsBase&                settings = modelOf(_model)->settings();
    const std::set<std::string>& declared = settings.writableMembers();

    std::vector<std::string> undeclared;
    for (const auto& [key, value] : parameters) {
        if (std::string keyName{std::string_view(key)}; !declared.contains(keyName)) {
            undeclared.push_back(std::move(keyName));
        }
    }
    if (!undeclared.empty()) {
        return std::unexpected(localError(std::format("block '{}' declares no setting [{}]", uniqueName(), commaSeparated(undeclared)), where));
    }
    try {
        std::ignore = settings.setStaged(parameters);
    } catch (const gr::exception& error) { // a value that does not convert to its setting's type
        return std::unexpected(localError(error.message, where));
    } catch (const std::exception& error) {
        return std::unexpected(localError(error.what(), where));
    }
    return {};
}

property_map BlockHandle::get(std::span<const std::string> keys) const { return _model ? modelOf(_model)->settings().get(keys) : property_map{}; }

std::optional<pmt::Value> BlockHandle::get(const std::string& key) const { return _model ? modelOf(_model)->settings().get(key) : std::nullopt; }

std::set<std::string> BlockHandle::writableMembers() const { return _model ? modelOf(_model)->settings().writableMembers() : std::set<std::string>{}; }

property_map BlockHandle::metaInformation() const { return _model ? modelOf(_model)->metaInformation() : property_map{}; }

std::vector<std::string> BlockHandle::inputPortNames() const { return _model ? portNamesOf(*modelOf(_model), true) : std::vector<std::string>{}; }

std::vector<std::string> BlockHandle::outputPortNames() const { return _model ? portNamesOf(*modelOf(_model), false) : std::vector<std::string>{}; }

RuntimeGraph::RuntimeGraph() : _impl(std::make_unique<Impl>()) {
    _impl->loader = loaderOrGlobal(nullptr);
    _impl->graph  = std::make_unique<Graph>(*_impl->loader);
}

RuntimeGraph::RuntimeGraph(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

RuntimeGraph::~RuntimeGraph() = default;

RuntimeGraph::RuntimeGraph(RuntimeGraph&&) noexcept = default;

RuntimeGraph& RuntimeGraph::operator=(RuntimeGraph&&) noexcept = default;

std::expected<BlockHandle, RuntimeError> RuntimeGraph::emplace(std::string_view type, std::string_view name, property_map parameters) {
    constexpr std::string_view where = "RuntimeGraph::emplace";
    if (!_impl) {
        return std::unexpected(localError("graph handle is empty", where));
    }
    PluginLoader& loader = *_impl->loader;

    std::shared_ptr<BlockModel> model;
    try {
        std::expected<std::shared_ptr<BlockModel>, gr::Error> built = loader.instantiateOrError(type, parameters);
        if (!built.has_value()) {
            return std::unexpected(localError(std::format("block type '{}' could not be built: {}", type, built.error().message), where));
        }
        model = std::move(*built);
        if (!model) {
            detail::registerShippedSchedulers();
            if (std::ranges::contains(loader.availableSchedulers(), type)) {
                return std::unexpected(localError(std::format("'{}' is a scheduler type -- a nested scheduler comes from a graph document through RuntimeGraph::fromYaml()", type), where));
            }
            return std::unexpected(localError(std::format("unknown block type '{}' -- the loader holds {} block types", type, loader.availableBlocks().size()), where));
        }
        model->setName(std::string(name)); // before addBlock, for init() to see the final name
        _impl->graph->addBlock(model);
    } catch (const gr::exception& error) { // a constructor or init() refuses a parameter
        return std::unexpected(localError(error.message, where));
    } catch (const std::exception& error) {
        return std::unexpected(localError(error.what(), where));
    }
    return BlockHandle(std::shared_ptr<void>(std::move(model)), _impl->loader);
}

std::expected<BlockHandle, RuntimeError> RuntimeGraph::add(std::shared_ptr<BlockModel> block, std::string_view name) {
    constexpr std::string_view where = "RuntimeGraph::add";
    if (!_impl) {
        return std::unexpected(localError("graph handle is empty", where));
    }
    if (!block) {
        return std::unexpected(localError("block is null", where));
    }
    if (!name.empty()) {
        block->setName(std::string(name)); // before addBlock, for init() to see the final name
    }
    try {
        _impl->graph->addBlock(block);
    } catch (const gr::exception& error) { // init() refuses a parameter
        return std::unexpected(localError(error.message, where));
    } catch (const std::exception& error) {
        return std::unexpected(localError(error.what(), where));
    }
    return BlockHandle(std::shared_ptr<void>(std::move(block)), _impl->loader);
}

std::vector<BlockHandle> RuntimeGraph::blocks(Recursive recursive) const {
    std::vector<BlockHandle> handles;
    if (!_impl) {
        return handles;
    }
    if (recursive == Recursive::No) {
        handles.reserve(_impl->graph->blocks().size());
        for (const std::shared_ptr<BlockModel>& block : _impl->graph->blocks()) {
            handles.push_back(BlockHandle(std::shared_ptr<void>(block), _impl->loader));
        }
        return handles;
    }
    gr::graph::forEachBlock<block::Category::All>(*_impl->graph, [this, &handles](const std::shared_ptr<BlockModel>& block) { handles.push_back(BlockHandle(std::shared_ptr<void>(block), _impl->loader)); });
    return handles;
}

std::expected<BlockHandle, RuntimeError> RuntimeGraph::find(std::string_view name, Recursive recursive) const {
    constexpr std::string_view     where      = "RuntimeGraph::find";
    const std::vector<BlockHandle> candidates = blocks(recursive);
    if (const auto byUniqueName = std::ranges::find(candidates, name, &BlockHandle::uniqueName); byUniqueName != candidates.end()) {
        return *byUniqueName;
    }
    if (_impl) { // a document name answers only while its block is in the graph
        if (const auto named = _impl->documentUniqueNames.find(name); named != _impl->documentUniqueNames.end()) {
            const std::shared_ptr<void> block = named->second.lock();
            if (const auto held = std::ranges::find(candidates, block, [](const BlockHandle& handle) { return handle._model; }); block != nullptr && held != candidates.end()) {
                return *held;
            }
        }
    }
    const std::size_t nNamed = static_cast<std::size_t>(std::ranges::count(candidates, name, &BlockHandle::name));
    if (nNamed > 1UZ) {
        return std::unexpected(localError(std::format("'{}' is the name of {} blocks in this graph; address one by its unique name", name, nNamed), where));
    }
    if (const auto byName = std::ranges::find(candidates, name, &BlockHandle::name); byName != candidates.end()) {
        return *byName;
    }
    return std::unexpected(localError(std::format("no block named '{}' in this graph", name), where));
}

std::expected<void, RuntimeError> RuntimeGraph::connect(const BlockHandle& sourceBlock, std::string_view sourcePort, //
    const BlockHandle& destinationBlock, std::string_view destinationPort, EdgeSpec edge) {
    constexpr std::string_view where = "RuntimeGraph::connect";
    if (!_impl || !sourceBlock.valid() || !destinationBlock.valid()) {
        return std::unexpected(localError("graph handle or block handle is empty", where));
    }
    // Graph::connect only records the edge. A name that resolves to nothing would surface when the graph starts.
    if (auto found = checkPortExists(*modelOf(sourceBlock._model), false, sourcePort, where); !found) {
        return found;
    }
    if (auto found = checkPortExists(*modelOf(destinationBlock._model), true, destinationPort, where); !found) {
        return found;
    }

    EdgeParameters parameters;
    parameters.minBufferSize = edge.minBufferSize == 0UZ ? undefined_size : edge.minBufferSize;
    auto connected           = _impl->graph->connect(modelPtrOf(sourceBlock._model), PortDefinition(std::string(sourcePort)), //
                  modelPtrOf(destinationBlock._model), PortDefinition(std::string(destinationPort)), std::move(parameters));
    if (!connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }
    return {};
}

std::expected<RuntimeGraph, RuntimeError> RuntimeGraph::fromYaml(std::shared_ptr<PluginLoader> loader, std::string_view document, const std::map<std::string, property_map, std::less<>>& overrides) {
    constexpr std::string_view where = "RuntimeGraph::fromYaml";
    try {
        const auto parsed = pmt::yaml::deserialize(document);
        if (!parsed.has_value()) {
            return std::unexpected(localError(std::format("line {}, column {}: {}", parsed.error().line, parsed.error().column, parsed.error().message), where));
        }

        detail::registerShippedSchedulers(); // a nested scheduler names one of these keys
        auto impl    = std::make_unique<Impl>();
        impl->loader = loaderOrGlobal(std::move(loader));
        impl->graph  = std::make_unique<Graph>(*impl->loader);

        const detail::LoadedBlocks loaded = detail::loadGraphFromMap(*impl->loader, *impl->graph, *parsed, overrides);
        for (const auto& [uniqueName, block] : loaded.byUniqueName) {
            impl->documentUniqueNames.emplace(uniqueName, block);
        }
        return RuntimeGraph(std::move(impl));
    } catch (const gr::exception& error) {
        return std::unexpected(localError(error.message, where));
    } catch (const gr::Error& error) {
        return std::unexpected(localError(error.message, where));
    } catch (const std::exception& error) {
        return std::unexpected(localError(error.what(), where));
    } catch (...) {
        return std::unexpected(localError("the graph-file reader threw an exception of unknown type", where));
    }
}

Runtime::Runtime(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

Runtime::Runtime(Runtime&&) noexcept = default;

Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Runtime::~Runtime() = default;

std::expected<Runtime, RuntimeError> Runtime::create(RuntimeGraph&& graph, std::string_view type, property_map schedulerParameters) {
    constexpr std::string_view where = "Runtime::create";
    if (!graph._impl) {
        return std::unexpected(localError("graph handle is empty", where));
    }
    detail::registerShippedSchedulers();
    PluginLoader& loader = *graph._impl->loader;

    std::shared_ptr<SchedulerModel> scheduler;
    try {
        scheduler = loader.instantiateScheduler(type, schedulerParameters);
    } catch (const gr::exception& error) { // the scheduler's constructor refuses a value
        return std::unexpected(localError(error.message, where));
    } catch (const std::exception& error) {
        return std::unexpected(localError(error.what(), where));
    }
    if (!scheduler) {
        return std::unexpected(localError(std::format("unknown scheduler type '{}' -- the loader holds {} scheduler types", type, loader.availableSchedulers().size()), where));
    }

    auto impl            = std::make_unique<Runtime::Impl>();
    impl->scheduler      = std::move(scheduler);
    impl->schedulerBlock = SchedulerModel::asBlockModelPtr(impl->scheduler);

    // the scheduler's constructor files a key it does not declare as meta-information and keeps the default
    const std::set<std::string>& declared = impl->schedulerBlock->settings().writableMembers();
    for (const auto& [key, value] : schedulerParameters) {
        if (const std::string keyName{std::string_view(key)}; !declared.contains(keyName)) {
            return std::unexpected(localError(std::format("scheduler type '{}' declares no setting '{}'", type, keyName), where));
        }
    }

    // A msgOut with no reader turns a child's error into an exception thrown on a worker thread, and the reason is lost.
    if (auto connected = impl->schedulerBlock->msgOut->connect(impl->events); !connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }

    impl->scheduler->setGraph(std::move(*graph._impl->graph));
    impl->loader = std::move(graph._impl->loader);
    graph._impl.reset();
    return Runtime(std::move(impl));
}

std::expected<void, RuntimeError> Runtime::runAndWait() {
    constexpr std::string_view where = "Runtime::runAndWait";
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", where));
    }
    {
        std::lock_guard lock(_impl->runMutex);
        if (_impl->runInProgress()) {
            return std::unexpected(localError("a run is in progress", where));
        }
        ++_impl->nRunsStarted;
    }
    std::expected<void, RuntimeError> result = _impl->runToEnd(where);
    _impl->endRun(result);
    return result;
}

std::expected<void, RuntimeError> Runtime::start() {
    constexpr std::string_view where = "Runtime::start";
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", where));
    }
    Impl&            impl = *_impl;
    std::unique_lock lock(impl.runMutex);
    if (impl.runInProgress()) {
        return std::unexpected(localError("a run is in progress", where));
    }
    if (impl.runThread.joinable()) { // the thread of an ended run, which does not take runMutex again
        impl.runThread.join();
    }
    const std::uint64_t run = ++impl.nRunsStarted;
    try {
        impl.runThread = std::thread([&impl] { impl.endRun(impl.runToEnd("Runtime::start")); });
    } catch (const std::system_error& error) {
        impl.lastResult = std::unexpected(localError(std::format("the run's thread did not start: {}", error.what()), where));
        ++impl.nRunsReturned;
        ++impl.nRunsEnded;
        return std::unexpected(impl.lastResult.error());
    }
    // the previous run left the scheduler in a state that is not active. An active state is this run's.
    const auto runningOrReturned = [&impl, run] { return lifecycle::isActive(impl.schedulerBlock->state()) || impl.nRunsReturned >= run; };
    while (!runningOrReturned()) {
        std::ignore = impl.runEnded.wait_for(lock, Impl::kStartPollInterval);
    }
    return {};
}

void Runtime::stop() {
    if (_impl) {
        std::ignore = _impl->stopRun(std::nullopt);
    }
}

bool Runtime::stopFor(std::chrono::nanoseconds timeout) { return _impl ? _impl->stopRun(timeout) : true; }

bool Runtime::waitFor(std::chrono::nanoseconds timeout) const {
    if (!_impl) {
        return true;
    }
    std::unique_lock lock(_impl->runMutex);
    return waitWithTimeout(_impl->runEnded, lock, timeout, [this] { return !_impl->runInProgress(); });
}

std::expected<void, RuntimeError> Runtime::result() const {
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", "Runtime::result"));
    }
    std::lock_guard lock(_impl->runMutex);
    if (_impl->runInProgress()) {
        return std::unexpected(localError("the run has not ended", "Runtime::result"));
    }
    return _impl->lastResult;
}

Runtime::State Runtime::state() const noexcept { return _impl ? static_cast<State>(_impl->schedulerBlock->state()) : State::Idle; }

BlockHandle Runtime::scheduler() const { return _impl ? BlockHandle(std::shared_ptr<void>(_impl->schedulerBlock), _impl->loader, true) : BlockHandle{}; }

} // namespace gr
