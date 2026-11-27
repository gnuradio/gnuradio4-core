#include <gnuradio-4.0/Runtime.hpp>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/RecipeInstantiation.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>

#include "ShippedSchedulers.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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

static_assert(static_cast<std::uint8_t>(RuntimeCommand::Invalid) == static_cast<std::uint8_t>(message::Command::Invalid));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Get) == static_cast<std::uint8_t>(message::Command::Get));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Set) == static_cast<std::uint8_t>(message::Command::Set));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Partial) == static_cast<std::uint8_t>(message::Command::Partial));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Final) == static_cast<std::uint8_t>(message::Command::Final));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Ready) == static_cast<std::uint8_t>(message::Command::Ready));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Disconnect) == static_cast<std::uint8_t>(message::Command::Disconnect));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Subscribe) == static_cast<std::uint8_t>(message::Command::Subscribe));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Unsubscribe) == static_cast<std::uint8_t>(message::Command::Unsubscribe));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Notify) == static_cast<std::uint8_t>(message::Command::Notify));
static_assert(static_cast<std::uint8_t>(RuntimeCommand::Heartbeat) == static_cast<std::uint8_t>(message::Command::Heartbeat));

namespace {

BlockModel* modelOf(const std::shared_ptr<void>& handle) noexcept { return static_cast<BlockModel*>(handle.get()); }

// a refusal that names no key: by a handle itself, or of a run
[[nodiscard]] settings::Rejected refusal(std::string reason) { return settings::Rejected{settings::Rejection{.key = {}, .reason = std::move(reason)}}; }

// the block a handle's `set` and `check` reach; the scheduler's settings are given to `Runtime::create()` alone
[[nodiscard]] std::expected<BlockModel*, settings::Rejected> blockToWrite(const std::shared_ptr<void>& model, bool isScheduler) {
    if (!model) {
        return std::unexpected(refusal("empty block handle"));
    }
    if (isScheduler) {
        return std::unexpected(refusal("scheduler settings only through Runtime::create()"));
    }
    return modelOf(model);
}

std::shared_ptr<BlockModel> modelPtrOf(const std::shared_ptr<void>& handle) noexcept { return std::static_pointer_cast<BlockModel>(handle); }

std::uint64_t toNanoseconds(std::chrono::system_clock::time_point when) { //
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

RuntimeError toRuntimeError(const Error& error) { return RuntimeError{.message = error.message, .where = error.srcLoc(), .time = toNanoseconds(error.errorTime)}; }

RuntimeError localError(std::string message, std::string_view where) { return RuntimeError{.message = std::move(message), .where = std::string(where), .time = 0ULL}; }

// an error that settings refused, which carries the refused entries. An entry's key reads `<block>.<key>`, and an
// entry that names no key reads `<block>`, the name the caller gave the block
RuntimeError refusedError(std::string message, settings::Rejected rejected, std::string_view block, std::string_view where) {
    RuntimeError error = localError(std::move(message), where);
    if (!block.empty()) {
        for (settings::Rejection& entry : rejected) {
            entry.key = entry.key.empty() ? std::string(block) : std::format("{}.{}", block, entry.key);
        }
    }
    error.rejected = std::move(rejected);
    return error;
}

// the name an entry of a start or a resume gives each block of `root`, `root` included: the name the caller gave it, and
// for a block inside a subgraph or a definition `<outer>.<inner>`, as the entries of its construction read. The blocks
// of a nested scheduler read their own names, as at their construction. A name two blocks share gives each its unique
// name
std::map<const BlockModel*, std::string> entryNames(const BlockModel& root) {
    const auto                                             given = [](const BlockModel& block) { return std::string(block.name().empty() ? block.uniqueName() : block.name()); };
    std::vector<std::pair<const BlockModel*, std::string>> named{{&root, given(root)}};
    const auto                                             walk = [&named, &given](const BlockModel& group, const std::string& prefix, const auto& self) -> void {
        for (const std::shared_ptr<BlockModel>& block : group.blocks()) {
            std::string name = prefix.empty() ? given(*block) : std::format("{}.{}", prefix, given(*block));
            if (block->blockCategory() == block::Category::TransparentBlockGroup) {
                self(*block, name, self);
            } else if (block->blockCategory() == block::Category::ScheduledBlockGroup) {
                self(*block, std::string{}, self);
            }
            named.emplace_back(block.get(), std::move(name));
        }
    };
    walk(root, std::string{}, walk);
    std::map<std::string, std::size_t, std::less<>> nUses;
    for (const auto& [block, name] : named) {
        ++nUses[name];
    }
    std::map<const BlockModel*, std::string> names;
    for (auto& [block, name] : named) {
        names.emplace(block, nUses.at(name) > 1UZ ? std::string(block->uniqueName()) : std::move(name));
    }
    return names;
}

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

RuntimeGraph RuntimeGraph::adopt(std::unique_ptr<Graph> graph, std::shared_ptr<PluginLoader> loader) {
    if (!graph) {
        throw std::invalid_argument("cannot adopt an empty native graph");
    }
    auto impl    = std::make_unique<Impl>();
    impl->loader = loaderOrGlobal(std::move(loader));
    impl->graph  = std::move(graph);
    return RuntimeGraph(std::move(impl));
}

struct Runtime::Impl {
    std::shared_ptr<PluginLoader>   loader;      // the graph's share, declared first to outlive the scheduler and the blocks
    MsgPortIn                       events;      // declared before the scheduler to outlive the scheduler writing to it
    MsgPortOut                      commands;    // feeds the scheduler's msgIn
    std::mutex                      eventsMutex; // guards `events` and `pending`
    std::deque<RuntimeEvent>        pending;
    std::mutex                      commandsMutex; // one sender at a time on `commands`
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
    std::uint64_t                     runningRun     = 0ULL;  // the last run whose scheduler moved its settings to running
    bool                              requestingStop = false; // a stop request has not returned
    std::expected<void, RuntimeError> lastResult;             // of the last run that ended, success before the first

    static constexpr std::size_t               kMaxPendingEvents = 1024UZ;
    static constexpr std::chrono::milliseconds kStopPollInterval{10}; // requestStop() reads the scheduler's state this often

    Impl()                       = default;
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        std::ignore = stopRun(std::nullopt);
        if (scheduler) {
            settings::detail::FrameworkAccess::onSettingsRunning(*scheduler, {}); // a handle may keep the scheduler past this runtime
        }
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

    void drain() {
        std::lock_guard guard(eventsMutex);
        drainLocked();
    }

    // Moves the scheduler's messages into `pending` as events.
    void drainLocked() {
        ReaderSpanLike auto messages = events.streamReader().get();
        for (const Message& message : messages) {
            RuntimeEvent event{.source = message.serviceName, .endpoint = message.endpoint, .isError = !message.data.has_value(), .text = {}, .where = {}, .data = {}, .time = 0ULL, .command = static_cast<RuntimeCommand>(message.cmd), .clientRequestID = message.clientRequestID};
            if (message.data.has_value()) {
                event.data = message.data.value();
            } else {
                event.text  = message.data.error().message;
                event.where = message.data.error().srcLoc();
                event.time  = toNanoseconds(message.data.error().errorTime);
            }
            pending.push_back(std::move(event));
        }
        std::ignore = messages.consume(messages.size());
        while (pending.size() > kMaxPendingEvents) {
            pending.pop_front();
        }
    }

    // each key of the latest start or resume of every block the scheduler runs, those of its subgraphs and of the
    // schedulers nested in it included, as `block.key`, the block named as a refusal of the start names it
    [[nodiscard]] settings::Values runValues() const {
        const std::map<const BlockModel*, std::string> names = entryNames(*schedulerBlock);
        settings::Values                               values;
        graph::forEachBlock<block::Category::All>(*schedulerBlock, [&values, &names](const std::shared_ptr<BlockModel>& block) {
            const auto        named = names.find(block.get());
            const std::string name  = named != names.end() ? named->second : std::string(block->uniqueName());
            for (auto& [key, entry] : settings::detail::FrameworkAccess::runOutcome(settings::detail::settingsOf(*block))) {
                values.insert_or_assign(std::format("{}.{}", name, key), std::move(entry));
            }
        });
        return values;
    }
};

BlockHandle::BlockHandle(std::shared_ptr<void> model, std::shared_ptr<PluginLoader> loader, bool isScheduler) noexcept : _loader(std::move(loader)), _model(std::move(model)), _isScheduler(isScheduler) {}

bool BlockHandle::valid() const noexcept { return _model != nullptr; }

// The settings read takes the lock under which a running block applies a staged `name`.
std::string BlockHandle::name() const {
    if (!_model) {
        return {};
    }
    if (const std::expected<std::string, settings::Rejected> named = modelOf(_model)->get<std::string>("name"); named.has_value()) {
        return *named;
    }
    return std::string(modelOf(_model)->name());
}

std::string BlockHandle::uniqueName() const { return _model ? std::string(modelOf(_model)->uniqueName()) : std::string{}; }

std::string BlockHandle::typeName() const { return _model ? std::string(modelOf(_model)->typeName()) : std::string{}; }

std::expected<settings::Values, settings::Rejected> BlockHandle::set(const property_map& changes, settings::Timeout timeout) {
    return blockToWrite(_model, _isScheduler).and_then([&](BlockModel* block) { return block->set(changes, timeout); });
}

std::expected<settings::Values, settings::Rejected> BlockHandle::set(std::string_view key, pmt::Value value, settings::Timeout timeout) {
    return blockToWrite(_model, _isScheduler).and_then([&](BlockModel* block) { return block->set(key, std::move(value), timeout); });
}

std::expected<void, settings::Rejected> BlockHandle::check(const property_map& changes) {
    return blockToWrite(_model, _isScheduler).and_then([&](BlockModel* block) { return block->check(changes); });
}

std::expected<settings::Value, settings::Rejected> BlockHandle::get(std::string_view key) const {
    if (!_model) {
        return std::unexpected(refusal("empty block handle"));
    }
    return modelOf(_model)->get(key);
}

std::expected<settings::Values, settings::Rejected> BlockHandle::get(std::span<const std::string> keys) const {
    if (!_model) {
        return std::unexpected(refusal("empty block handle"));
    }
    return modelOf(_model)->get(keys);
}

std::set<std::string> BlockHandle::writableMembers() const { return _model ? modelOf(_model)->writableMembers() : std::set<std::string>{}; }

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
        // a recipe is built from its exported parameters and loads the rest as settings, as Graph::emplaceBlock() does
        const detail::RecipeParameterSplit                    split = detail::splitRecipeParameters(loader, type, std::move(parameters));
        std::expected<std::shared_ptr<BlockModel>, gr::Error> built = split.fromDefinition ? detail::instantiateRecipe(loader, type, name, split) : loader.instantiateOrError(type, split.remaining);
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
        std::ignore = model->setName(std::string(name)); // before addBlock, for init() to see the final name
        _impl->graph->addBlock(model);
    } catch (const settings::RefusedSettings& refused) { // a constructor or init() refuses a parameter
        return std::unexpected(refusedError(refused.message, refused.rejected, refused.block, where));
    } catch (const gr::exception& error) {
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
    if (!name.empty()) { // before addBlock, for init() to see the final name
        const std::expected<settings::Values, settings::Rejected> renamed = block->setName(std::string(name));
        if (const settings::Rejected refused = renamed ? settings::detail::failedKeys(*renamed) : renamed.error(); !refused.empty()) {
            return std::unexpected(refusedError(std::format("block '{}': name '{}': {}", block->uniqueName(), name, refused.front().reason), refused, block->name(), where));
        }
    }
    try {
        _impl->graph->addBlock(block);
    } catch (const settings::RefusedSettings& refused) { // init() refuses a parameter
        return std::unexpected(refusedError(refused.message, refused.rejected, refused.block, where));
    } catch (const gr::exception& error) {
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
    } catch (const settings::RefusedSettings& refused) {
        return std::unexpected(refusedError(refused.message, refused.rejected, refused.block, where));
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
    } catch (const settings::RefusedSettings& refused) { // the scheduler's constructor refuses a value
        return std::unexpected(refusedError(refused.message, refused.rejected, refused.block, where));
    } catch (const gr::exception& error) {
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
    settings::detail::FrameworkAccess::onSettingsRunning(*impl->scheduler, [state = impl.get()] {
        std::lock_guard lock(state->runMutex);
        state->runningRun = state->nRunsStarted;
        state->runEnded.notify_all();
    });

    // the scheduler's constructor files a key it does not declare as meta-information and keeps the default
    const std::set<std::string>& declared = impl->schedulerBlock->writableMembers();
    for (const auto& [key, value] : schedulerParameters) {
        if (const std::string keyName{std::string_view(key)}; !declared.contains(keyName)) {
            return std::unexpected(refusedError(std::format("scheduler type '{}' declares no setting '{}'", type, keyName), settings::Rejected{settings::Rejection{.key = keyName, .reason = "unknown setting"}}, impl->schedulerBlock->name(), where));
        }
    }

    // A msgOut with no reader turns a child's error into an exception thrown on a worker thread, and the reason is lost.
    // The scheduler reports there a pool name it does not know, which the parameters below may give.
    if (auto connected = impl->schedulerBlock->msgOut->connect(impl->events); !connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }
    if (auto connected = impl->commands.connect(*impl->schedulerBlock->msgIn); !connected) {
        return std::unexpected(toRuntimeError(connected.error()));
    }

    // A plugin's scheduler may ignore its constructor's parameters. The parameters are applied to the built scheduler as a
    // settings change.
    if (!schedulerParameters.empty()) {
        // a stopped scheduler applies its settings at the call
        const std::expected<settings::Values, settings::Rejected> applied = impl->schedulerBlock->set(schedulerParameters);
        if (const settings::Rejected refused = applied ? settings::detail::failedKeys(*applied) : applied.error(); !refused.empty()) {
            return std::unexpected(refusedError(std::format("scheduler type '{}': {}", type, settings::describeRejected(refused)), refused, impl->schedulerBlock->name(), where));
        }
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
        settings::detail::FrameworkAccess::returnStartValues(*_impl->scheduler, false);
    }
    std::expected<void, RuntimeError> result = _impl->runToEnd(where);
    _impl->endRun(result);
    return result;
}

std::expected<settings::Values, settings::Rejected> Runtime::start() {
    using settings::detail::FrameworkAccess;
    if (!_impl) {
        return std::unexpected(refusal("runtime handle is empty"));
    }
    Impl&            impl = *_impl;
    std::unique_lock lock(impl.runMutex);
    if (impl.runInProgress()) {
        return std::unexpected(refusal("a run is in progress"));
    }
    if (impl.runThread.joinable()) { // the thread of an ended run, which does not take runMutex again
        impl.runThread.join();
    }
    const std::uint64_t run = ++impl.nRunsStarted;
    settings::detail::FrameworkAccess::returnStartValues(*impl.scheduler, true);
    try {
        impl.runThread = std::thread([&impl] { impl.endRun(impl.runToEnd("Runtime::start")); });
    } catch (const std::system_error& error) {
        impl.lastResult = std::unexpected(localError(std::format("the run's thread did not start: {}", error.what()), "Runtime::start"));
        ++impl.nRunsReturned;
        ++impl.nRunsEnded;
        return std::unexpected(refusal(impl.lastResult.error().message));
    }
    // the scheduler's settings run once every block has started and its workers are released
    impl.runEnded.wait(lock, [&impl, run] { return impl.runningRun >= run || impl.nRunsEnded >= run; });
    // a run that ended on an error after its start returns the values, and result() reports the error. A start that
    // failed returns the refusal. A stop asked during the start ends the run with no error and before the release, and
    // the start returns no values for a graph that does not run
    if (impl.runningRun < run && impl.lastResult.has_value()) {
        return std::unexpected(settings::Rejected{settings::Rejection{.key = entryNames(*impl.schedulerBlock).at(impl.schedulerBlock.get()), .reason = "stopped during the start"}});
    }
    if (impl.runningRun < run && impl.scheduler->startError().has_value()) {
        const std::map<const BlockModel*, std::string> names = entryNames(*impl.schedulerBlock);
        settings::Rejected                             refused;
        graph::forEachBlock<block::Category::All>(*impl.schedulerBlock, [&refused, &names](const std::shared_ptr<BlockModel>& block) {
            const auto        named = names.find(block.get());
            const std::string name  = named != names.end() ? named->second : std::string(block->uniqueName());
            for (settings::Rejection entry : FrameworkAccess::runRefusals(settings::detail::settingsOf(*block))) {
                entry.key = entry.key.empty() ? name : std::format("{}.{}", name, entry.key);
                refused.push_back(std::move(entry));
            }
        });
        refused.push_back(settings::Rejection{.key = names.at(impl.schedulerBlock.get()), .reason = impl.lastResult.error().message});
        return std::unexpected(std::move(refused));
    }
    lock.unlock();
    return impl.runValues();
}

std::expected<void, RuntimeError> Runtime::pause() {
    constexpr std::string_view where = "Runtime::pause";
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", where));
    }
    BlockModel& scheduler = *_impl->schedulerBlock;
    if (scheduler.state() != lifecycle::State::RUNNING) {
        return std::unexpected(localError(std::format("the scheduler is {}, not running", gr::meta::enumName(scheduler.state()).value_or("?")), where));
    }
    if (std::expected<void, Error> paused = scheduler.changeStateTo(lifecycle::State::REQUESTED_PAUSE); !paused) {
        return std::unexpected(toRuntimeError(paused.error()));
    }
    if (scheduler.state() != lifecycle::State::PAUSED) { // a stop overtook the pause
        return std::unexpected(localError(std::format("the scheduler is {}, not paused", gr::meta::enumName(scheduler.state()).value_or("?")), where));
    }
    return {};
}

std::expected<settings::Values, settings::Rejected> Runtime::resume() {
    if (!_impl) {
        return std::unexpected(refusal("runtime handle is empty"));
    }
    BlockModel& scheduler = *_impl->schedulerBlock;
    const auto  refusal   = [&scheduler](std::string reason) { return settings::Rejected{settings::Rejection{.key = entryNames(scheduler).at(&scheduler), .reason = std::move(reason)}}; };
    if (scheduler.state() != lifecycle::State::PAUSED) {
        return std::unexpected(refusal(std::format("the scheduler is {}, not paused", gr::meta::enumName(scheduler.state()).value_or("?"))));
    }
    if (std::expected<void, Error> resumed = scheduler.changeStateTo(lifecycle::State::RUNNING); !resumed) {
        return std::unexpected(refusal(resumed.error().message));
    }
    if (scheduler.state() != lifecycle::State::RUNNING) { // the resume could not connect an edge, or a stop overtook it
        const std::optional<Error> failed = _impl->scheduler->startError();
        return std::unexpected(refusal(failed ? failed->message : std::format("the scheduler is {} after the resume", gr::meta::enumName(scheduler.state()).value_or("?"))));
    }
    return _impl->runValues();
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

std::vector<RuntimeEvent> Runtime::pollEvents(std::size_t maxEvents) {
    std::vector<RuntimeEvent> events;
    if (!_impl) {
        return events;
    }
    std::lock_guard guard(_impl->eventsMutex);
    _impl->drainLocked();

    const std::size_t nEvents = std::min(maxEvents, _impl->pending.size());
    events.reserve(nEvents);
    for (std::size_t i = 0UZ; i < nEvents; ++i) {
        events.push_back(std::move(_impl->pending.front()));
        _impl->pending.pop_front();
    }
    return events;
}

std::expected<void, RuntimeError> Runtime::send(RuntimeCommand command, std::string_view serviceName, std::string_view endpoint, property_map payload, std::string_view clientRequestID) {
    constexpr std::string_view where = "Runtime::send";
    if (!_impl) {
        return std::unexpected(localError("runtime handle is empty", where));
    }
    std::lock_guard guard(_impl->commandsMutex);
    MsgPortOut&     port = _impl->commands;

    using enum message::Command;
    bool published = false;
    switch (command) {
    case RuntimeCommand::Get: published = detail::sendMessage<Get>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Set: published = detail::sendMessage<Set>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Partial: published = detail::sendMessage<Partial>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Final: published = detail::sendMessage<Final>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Ready: published = detail::sendMessage<Ready>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Disconnect: published = detail::sendMessage<Disconnect>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Subscribe: published = detail::sendMessage<Subscribe>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Unsubscribe: published = detail::sendMessage<Unsubscribe>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Notify: published = detail::sendMessage<Notify>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Heartbeat: published = detail::sendMessage<Heartbeat>(port, serviceName, endpoint, std::move(payload), clientRequestID); break;
    case RuntimeCommand::Invalid: return std::unexpected(localError("RuntimeCommand::Invalid is not sendable", where));
    }
    if (!published) {
        return std::unexpected(localError(std::format("the scheduler's message input is full -- the message to '{}' endpoint '{}' is dropped", serviceName, endpoint), where));
    }
    return {};
}

std::vector<BlockHandle> Runtime::blocks() const {
    std::vector<BlockHandle> handles;
    if (_impl) {
        if (auto* graph = _impl->schedulerBlock->graph()) {
            gr::graph::forEachBlock<block::Category::All>(*graph, [this, &handles](const std::shared_ptr<BlockModel>& block) { handles.push_back(BlockHandle(std::shared_ptr<void>(block), _impl->loader)); });
        }
    }
    return handles;
}

void Runtime::withScheduler(const std::function<void(SchedulerModel&)>& operation) {
    if (!_impl) {
        throw std::logic_error("runtime has been moved from");
    }
    operation(*_impl->scheduler);
}

BlockHandle Runtime::scheduler() const { return _impl ? BlockHandle(std::shared_ptr<void>(_impl->schedulerBlock), _impl->loader, true) : BlockHandle{}; }

} // namespace gr
