#ifndef GNURADIO_SCHEDULER_HPP
#define GNURADIO_SCHEDULER_HPP

#include <algorithm>
#include <bit>
#include <chrono>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>

#include <thread>
#include <utility>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Profiler.hpp>
#include <gnuradio-4.0/SharedState.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

// Under Windows windows.h defines ERROR as 0.  This messes the ERROR function work::status::ERROR.
#ifdef _WIN32
#ifdef ERROR
#undef ERROR
#endif // #ifdef ERROR
#endif // #ifdef _WIN32

// Returns when the sequence leaves oldValue or after timeout_ms, whichever comes first. The producer's notify_all() on
// the sequence ends the wait. The timeout is a wall-clock bound: the caller may be the only thread of its runtime, and a
// sequence that nothing else advances or notifies must not hold it.
template<typename T>
inline void waitUntilChanged(gr::Sequence& sequence, T oldValue, unsigned int timeout_ms = 1U) {
    sequence.waitUntil(oldValue, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
}

// Returns when the sequence leaves oldValue, `wake` leaves wakeOldValue, or after timeout_ms, whichever comes first.
// Advancing `wake` ends the wait once the advancing thread calls notify_all() on the sequence.
template<typename T>
inline void waitUntilChanged(gr::Sequence& sequence, T oldValue, const gr::Sequence& wake, std::size_t wakeOldValue, unsigned int timeout_ms) {
    sequence.waitUntil(oldValue, wake, wakeOldValue, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
}

namespace gr::scheduler {
using namespace gr::message;

namespace property {

inline static const char* const kEmplaceBlock = "EmplaceBlock";
inline static const char* const kRemoveBlock  = "RemoveBlock";
inline static const char* const kReplaceBlock = "ReplaceBlock";
inline static const char* const kEmplaceEdge  = "EmplaceEdge";
inline static const char* const kRemoveEdge   = "RemoveEdge";

inline static const char* const kBlockEmplaced = "BlockEmplaced";
inline static const char* const kBlockRemoved  = "BlockRemoved";
inline static const char* const kBlockReplaced = "BlockReplaced";
inline static const char* const kEdgeEmplaced  = "EdgeEmplaced";
inline static const char* const kEdgeRemoved   = "EdgeRemoved";

inline static const char* const kGraphGRC           = "GraphGRC";
inline static const char* const kSchedulerInspect   = "SchedulerInspect";
inline static const char* const kSchedulerInspected = "SchedulerInspected";
} // namespace property

enum class ExecutionPolicy {
    singleThreaded,        ///
    multiThreaded,         ///
    singleThreadedBlocking /// blocks with a time-out if none of the blocks in the graph made progress (N.B. a CPU/battery power-saving measures)
};

using JobLists = std::vector<std::vector<std::shared_ptr<BlockModel>>>;

// identifies the scheduler whose poolWorker() is running on this thread, so a handler invoked from
// that worker can tell that waiting for the job count to reach zero would include itself
inline thread_local const void*& tActiveSchedulerWorker = activeSchedulerWorker();

template<typename Derived, ExecutionPolicy execution = ExecutionPolicy::singleThreaded, profiling::ProfilerLike TProfiler = profiling::null::Profiler>
struct SchedulerBase : Block<Derived> {
    friend class lifecycle::StateMachine<Derived>;
    using TaskExecutor = gr::thread_pool::TaskExecutor;
    using enum block::Category;

private:
    static consteval void _forbid_reserved_overrides() {
        using Base = SchedulerBase<Derived, execution, TProfiler>;
        // Lifecycle callback functions init/start/stop/pause/resume/reset need to remain reserved to SchedulerBase<Derived, ...>.
        // Do NOT re-implement them in the Derived custom user-defined scheduler.
        static_assert(std::same_as<decltype(&Derived::init), decltype(&Base::init)>, "Derived defines 'init()' (reserved). Use 'customInit()' instead.");
        static_assert(std::same_as<decltype(&Derived::start), decltype(&Base::start)>, "Derived defines 'start()' (reserved). Use 'customStart()' instead.");
        static_assert(std::same_as<decltype(&Derived::stop), decltype(&Base::stop)>, "Derived defines 'stop()' (reserved). Use 'customStop()' instead.");
        static_assert(std::same_as<decltype(&Derived::pause), decltype(&Base::pause)>, "Derived defines 'pause()' (reserved). Use 'customPause()' instead.");
        static_assert(std::same_as<decltype(&Derived::resume), decltype(&Base::resume)>, "Derived defines 'resume()' (reserved). Use 'customResume()' instead.");
        static_assert(std::same_as<decltype(&Derived::reset), decltype(&Base::reset)>, "Derived defines 'reset()' (reserved). Use 'customReset()' instead.");
    }

    gr::Graph* findTargetSubGraph(const gr::property_map& data) {
        auto it = data.find("_targetGraph");
        if (it == data.end()) {
            return std::addressof(*_graph);
        } else if (it->second.value_or(std::string()) == _graph->unique_name || it->second.value_or(std::string()) == this->unique_name) {
            return std::addressof(*_graph);
        } else {
            const auto targetGraphName = it->second.value_or(std::string_view{});
            if (targetGraphName.empty()) {
                return nullptr;
            }
            auto result = graph::findBlock(*_graph, std::string_view(targetGraphName));
            if (!result) {
                return nullptr;
            }

            if (result.value()->typeName() != "gr::Graph") {
                return nullptr;
            }

            return static_cast<gr::Graph*>(result.value()->raw());
        }
    }

protected:
    using ProfileHandle = decltype(std::declval<TProfiler&>().forThisThread());

    std::size_t                   _nWatchdogsRunning{0};
    gr::Sequence                  _watchdogGeneration{}; // a watchdog runs only while it matches this. An advance wakes it.
    meta::indirect<gr::Graph>     _graph{};
    TProfiler                     _profiler{};
    ProfileHandle                 _profilerHandler{_profiler.forThisThread()};
    std::shared_ptr<TaskExecutor> _pool{gr::thread_pool::Manager::instance().defaultCpuPool()};
    const TaskExecutor*           _jobListsPool = nullptr; // the pool the job lists were last sized on
    std::shared_ptr<gr::Sequence> _nRunningJobs = std::make_shared<gr::Sequence>();
    std::recursive_mutex          _executionOrderMutex; // only used when modifying and copying the graph->local job list
    std::shared_ptr<JobLists>     _executionOrder = std::make_shared<JobLists>();
    std::mutex                    _childLifecycleMutex; // serializes start()'s and stop()'s sweeps; a worker never takes it to count itself
    std::mutex                    _workersInLoopMutex;  // guards _nWorkersInLoop; no block's stop() hook runs under it
    std::size_t                   _nWorkersInLoop{0UZ}; // workers inside poolWorker(); only these workers call a block's work()
    // advanced by everything that ends a park and is not work (see wakeWorkers() and registerWake()). The rings in
    // _wakeRings are changed under _workersInLoopMutex
    std::shared_ptr<gr::Sequence>      _wake = std::make_shared<gr::Sequence>();
    std::vector<MsgPortIn::BufferType> _wakeRings;

    std::mutex                               _zombieBlocksMutex;
    std::vector<std::shared_ptr<BlockModel>> _zombieBlocks;

    // the stage of the latest run. awaiting: no run has begun since construction or since the scheduler was last
    // initialized. starting: RUNNING is published and start() has not returned. started: start() has returned, or an
    // awaited run ended without a start.
    enum class RunPhase { awaiting, starting, started };

    // The run record. Every field is written under _runMutex. A worker reads the generation without the lock.
    struct RunRecord {
        std::size_t                generation{0UZ}; // advanced by start(), by stop(), and by a stop request that the state already satisfies
        RunPhase                   phase{RunPhase::awaiting};
        bool                       destroying{false}; // set by the destructor. A start that reads it set runs nothing.
        std::optional<std::size_t> restoredFrom;      // the generation of the scheduler's own stop that restoreRun() last restarted from
    };

    // a graph swap or a restart that a worker of this scheduler requested. The worker whose message handler made the
    // latest request claims it when it releases its count. No other worker claims it. The claimed request stays here
    // until every other worker has left, and a start or a swap that another worker requests in that time joins it. The
    // claiming worker then takes the request and applies it. A restart requested by start() carries no graph.
    struct PendingExchange {
        std::optional<meta::indirect<gr::Graph>> graph;
        profiling::Options                       option;
        bool                                     restart{false};
        std::size_t                              generation{0UZ}; // a start or a stop that changes it cancels the restart
        std::thread::id                          requester;       // the worker that claims the request
        bool                                     claimed{false};
    };
    std::mutex                     _runMutex; // guards _run, _pendingExchange and the claim of a pending swap
    RunRecord                      _run;
    std::optional<PendingExchange> _pendingExchange;
    std::size_t                    _nDeferredExchanges{0UZ}; // claimed swaps and restarts still running outside the job count
    bool                           _exchangeClaimed{false};  // held by exchange() while it swaps the graph of an inactive scheduler
    std::optional<Error>           _startError;              // written by failStart(), cleared when a start begins

    // for blocks that were added while scheduler was running. They need to be adopted by a thread
    std::mutex _adoptionBlocksMutex;
    // fixed-sized vector indexed by runnerId. Cheaper than a map.
    std::vector<std::vector<std::shared_ptr<BlockModel>>> _adoptionBlocks;
    std::vector<bool>                                     _adoptionListClosed; // the job list's worker has left its loop

    MsgPortOutForChildren                     _toChildMessagePort;
    MsgPortInFromChildren                     _fromChildMessagePort;
    std::vector<gr::Message>                  _pendingMessagesToChildren;
    std::vector<gr::Message>                  _unforwardedReplies; // children's replies that found msgOut full, oldest first
    bool                                      _messagePortsConnected = false;
    std::optional<Error>                      _firstErrorFromChildren; // the first error a child sent since the latest start, named for the child
    std::map<std::string, Error, std::less<>> _latestErrorByChild;     // each child's latest error since the latest start, keyed by its unique name
    std::mutex                                _runEndingBlockMutex;
    std::optional<std::string>                _runEndingBlock; // the first block that ended the run since the latest start (see failRun())

    std::atomic_flag _processingScheduledMessages;
    // separate cache lines: every worker reads the flag and updates the counter on each iteration
    alignas(gr::kCacheLine) bool _workQuiescenceRequested{false};
    alignas(gr::kCacheLine) std::size_t _nWorkersInWork{0};
    // counted by the dispatch, immediately before the call into poolWorker(), so that a scheduler which supplies its
    // own worker is counted as well; a worker the pool has only queued is not counted
    std::size_t _nWorkersStarted{0};

    // A graph that holds this scheduler as a block connects the ports that this scheduler's graph exports. Each start
    // records the blocks that own those ports and the progress sequences that their work advances: the sequence that
    // this scheduler holds, followed by the outer sequences its holder handed it. A port exported while the scheduler
    // runs takes effect at the next start.
    std::vector<const BlockModel*>             _exportedBlocks;
    std::vector<std::shared_ptr<gr::Sequence>> _outerProgress;    // handed by the holder; empty unless it exports this scheduler
    std::vector<std::shared_ptr<gr::Sequence>> _exportedProgress; // read by the workers of one run

    void recordExportedBlocks() {
        _exportedProgress.assign(1UZ, this->progress);
        std::ranges::copy(_outerProgress, std::back_inserter(_exportedProgress));
        _exportedBlocks.clear();
        graph::forEachBlock<block::Category::TransparentBlockGroup>(*_graph, [this](auto& block) {
            if (std::ranges::any_of(_graph->_exportedPorts, [&block](const auto& entry) { return entry.blockName == block->uniqueName(); })) {
                _exportedBlocks.push_back(block.get());
            }
        });
    }

    [[nodiscard]] bool isExported(const BlockModel& block) const { return std::ranges::find(_exportedBlocks, std::addressof(block)) != _exportedBlocks.end(); }

    // A nested scheduler holds the progress sequence of this scheduler's graph. When this scheduler exports the nested
    // scheduler's ports in turn, the graphs beyond this one read or write those rings as well, and the nested scheduler
    // receives their sequences as its outer sequences. Each start hands both before the nested scheduler starts.
    void handProgressToNestedSchedulers() {
        graph::forEachBlock<block::Category::TransparentBlockGroup>(*_graph, [this](auto& block) {
            if (block->blockCategory() != block::Category::ScheduledBlockGroup) {
                return;
            }
            block->init(_graph->_progress, this->compute_domain);
            if (auto* receiver = dynamic_cast<OuterProgressReceiver*>(block.get()); receiver != nullptr) {
                receiver->setOuterProgress(isExported(*block) ? _exportedProgress : std::vector<std::shared_ptr<gr::Sequence>>{});
            }
        });
    }

    // A worker of a graph that connects the exported ports parks on that graph's progress sequence. A pass in which an
    // exported block moves samples can publish into or free a ring that the graph reads or writes. The pass advances
    // each recorded sequence after its work calls return and wakes those workers. The pass that ends the run advances
    // them as well. Work that stays inside this scheduler's graph leaves them alone.
    void advanceEnclosingProgress(bool exportedBlockMoved, bool runEnds) {
        if (!exportedBlockMoved && !(runEnds && !_exportedBlocks.empty())) {
            return;
        }
        for (const std::shared_ptr<gr::Sequence>& sequence : _exportedProgress) {
            sequence->incrementAndGet();
            sequence->notify_all();
        }
    }

    // a watchdog only leaves on its own once the run's jobs are gone, which a restart inside its check
    // interval undoes, so every start retires the previous generation explicitly
    void stopWatchdogs() {
        _watchdogGeneration.incrementAndGet();
        _watchdogGeneration.notify_all();
    }

    // retires the watchdogs and waits until each has returned. A watchdog wakes at once on its retirement.
    void retireWatchdogs() {
        stopWatchdogs();
        for (std::size_t n = gr::atomic_ref(_nWatchdogsRunning).load_acquire(); n != 0UZ; n = gr::atomic_ref(_nWatchdogsRunning).load_acquire()) {
            gr::atomic_ref(_nWatchdogsRunning).wait(n);
        }
    }

    [[nodiscard]] std::size_t runGeneration() {
        std::lock_guard guard(_runMutex);
        return _run.generation;
    }

    void advanceRunGeneration() {
        std::lock_guard guard(_runMutex);
        gr::atomic_ref(_run.generation).fetch_add(1UZ);
    }

    // a run that was awaited and ended without one: the next runAndWait() runs the graph
    void endAwaitedRun() {
        std::lock_guard guard(_runMutex);
        if (_run.phase == RunPhase::awaiting) {
            _run.phase = RunPhase::started;
        }
    }

    // The run executes on the caller's thread inside start(). The scheduler refuses a swap from another thread from the
    // moment the state reads RUNNING until start() returns. The job count rises only partway through start(). After
    // start() has returned, the swap proceeds. The thread that applies a swap requested on the scheduler's worker
    // swaps as the worker would: a start in the same message batch has published RUNNING without running a worker.
    [[nodiscard]] std::expected<void, Error> swapAllowedFromThisThread() {
        if constexpr (executionPolicy() == ExecutionPolicy::singleThreaded || executionPolicy() == ExecutionPolicy::singleThreadedBlocking) {
            const auto state = this->state();
            if (!isOnOwnWorkerThread() && applyingScheduler() != static_cast<const void*>(this) && lifecycle::isActive(state)) {
                std::lock_guard guard(_runMutex);
                if (_run.phase != RunPhase::started) {
                    return std::unexpected(Error(std::format("exchange(): the single-threaded scheduler '{}' is {} on another thread; stop it before exchanging its graph", this->unique_name, gr::meta::enumName(state).value_or(""))));
                }
            }
        }
        return {};
    }

    // Restarts a run that this scheduler stopped on its own, for a graph swap. ownStopGeneration is the generation that
    // the swap's own stop advanced to. The scheduler leaves STOPPED first and reads the generation after that. An
    // unchanged generation restores RUNNING and the pause the run had. A changed one means a start or a stop came
    // since. The scheduler then requests a stop in place of RUNNING. A stop request that the state satisfied before the
    // read has advanced the generation. One that finds the state already left STOPPED requests its own stop.
    std::expected<void, Error> restoreRun(std::size_t ownStopGeneration, lifecycle::State runState) {
        using enum lifecycle::State;
        {
            std::lock_guard guard(_runMutex);
            _run.restoredFrom = ownStopGeneration;
        }
        if (auto result = this->changeStateTo(INITIALISED); !result) {
            return std::unexpected(result.error());
        }
        if (runGeneration() != ownStopGeneration) {
            this->emitErrorMessageIfAny("restoreRun() -> REQUESTED_STOP", this->changeStateTo(REQUESTED_STOP));
            endAwaitedRun();
            return {};
        }
        if (auto result = this->changeStateTo(RUNNING); !result) {
            if (lifecycle::isShuttingDown(this->state())) { // a stop claimed the transition first
                endAwaitedRun();
                return {};
            }
            return std::unexpected(result.error());
        }
        if (runState == REQUESTED_PAUSE || runState == PAUSED) {
            if (auto result = this->changeStateTo(REQUESTED_PAUSE); !result) {
                return std::unexpected(result.error());
            }
        }
        if (runState == PAUSED) {
            if (auto result = this->changeStateTo(PAUSED); !result) {
                return std::unexpected(result.error());
            }
        }
        return {};
    }

    // exchange() on an inactive scheduler holds a claim from its state check to the end of the swap. It takes the claim
    // and then confirms the state it read with a compare-exchange. A thread that changed the state before that
    // compare-exchange makes the swap fail. A thread that changes it afterwards waits in init(), reset() or start()
    // until the claim is released, and then prepares or runs the new graph. The compare-exchange orders the two
    // threads: a state change that follows it also sees the claim.
    void claimExchange() {
        for (bool expected = false; !gr::atomic_ref(_exchangeClaimed).compare_exchange(expected, true); expected = false) {
            gr::atomic_ref(_exchangeClaimed).wait(true);
        }
    }

    void releaseExchange() {
        gr::atomic_ref(_exchangeClaimed).store_release(false);
        gr::atomic_ref(_exchangeClaimed).notify_all();
    }

    // a worker of this scheduler does not wait: a swap in progress can be waiting for the worker's job count
    void awaitExchange() {
        if (exchangingScheduler() == static_cast<const void*>(this) || isOnOwnWorkerThread()) {
            return;
        }
        for (bool claimed = gr::atomic_ref(_exchangeClaimed).load_acquire(); claimed; claimed = gr::atomic_ref(_exchangeClaimed).load_acquire()) {
            gr::atomic_ref(_exchangeClaimed).wait(true);
        }
    }

    // a worker occupies its pool thread for the scheduler's lifetime, so a job list that never gets one
    // never runs its blocks and back-pressure stalls the whole graph -- claim only the free threads.
    // A worker that runs a reset from a message, and a thread that applies a deferred swap or restart, hold their
    // thread only until the next run is dispatched. That thread counts as free. A task the pool has queued and no
    // thread has taken yet counts as a held thread, whatever its length: a worker of a scheduler that started a moment
    // earlier holds its thread for the run, and a short task of another component counts the same.
    [[nodiscard]] std::size_t nJobLists(std::size_t nBlocks) const {
        const std::size_t nThreads = static_cast<std::size_t>(_pool->maxThreads());
        const std::size_t nOwn     = isOnOwnWorkerThread() || applyingScheduler() == static_cast<const void*>(this) ? 1UZ : 0UZ;
        const std::size_t nRunning = _pool->numTasksRunning() + _pool->numTasksQueued();
        const std::size_t nBusy    = std::min(nRunning - std::min(nRunning, nOwn), nThreads);
        return std::min(std::max(nThreads - nBusy, 1UZ), nBlocks);
    }

    // waits until every worker count except the caller's own is released. A lifecycle command sent as a message can
    // run on one of the scheduler's workers. That worker holds its own count until it leaves poolWorker() at its next
    // generation check.
    void waitForOtherWorkers() {
        const std::size_t nOwn = isOnOwnWorkerThread() ? 1UZ : 0UZ;
        for (std::size_t nRunning = _nRunningJobs->value(); nRunning > nOwn; nRunning = _nRunningJobs->value()) {
            _nRunningJobs->wait(nRunning);
        }
    }

    // waits until no worker and no claimed swap or restart remain. A worker that claims a swap or a restart releases its
    // count before it applies the claim, and the next run's workers are counted only while the claim is applied.
    void waitForRunAndClaims() {
        auto nDeferred = [this] { return gr::atomic_ref(_nDeferredExchanges).load_acquire(); };
        do {
            waitDone();
            for (std::size_t n = nDeferred(); n != 0UZ; n = nDeferred()) {
                gr::atomic_ref(_nDeferredExchanges).wait(n);
            }
        } while (_nRunningJobs->value() != 0UZ);
    }

    void rebuildProfiler(const profiling::Options& opt) {
        std::destroy_at(std::addressof(_profiler));
        std::construct_at(std::addressof(_profiler), opt);
        _profilerHandler = _profiler.forThisThread();
    }

    void registerPropertyCallbacks() noexcept {
        _forbid_reserved_overrides();
        using PropertyCallback                            = BlockBase::PropertyCallback;
        auto& callbacks                                   = this->propertyCallbacks;
        callbacks[scheduler::property::kEmplaceBlock]     = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackEmplaceBlock);
        callbacks[scheduler::property::kRemoveBlock]      = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackRemoveBlock);
        callbacks[scheduler::property::kRemoveEdge]       = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackRemoveEdge);
        callbacks[scheduler::property::kEmplaceEdge]      = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackEmplaceEdge);
        callbacks[scheduler::property::kReplaceBlock]     = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackReplaceBlock);
        callbacks[scheduler::property::kGraphGRC]         = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackGraphGRC);
        callbacks[scheduler::property::kSchedulerInspect] = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackSchedulerInspect);
        callbacks[graph::property::kInspectBlock]         = static_cast<PropertyCallback>(&SchedulerBase::propertyCallbackInspectBlock);
        this->settings().updateActiveParameters();
    }

public:
    using base_t = Block<Derived>;

    Annotated<gr::Size_t, "timeout", Unit<"ms">, Doc<"longest wait of an idle or paused worker">>                                          timeout_ms                      = 100U;
    Annotated<gr::Size_t, "watchdog_timeout", Unit<"ms">, Doc<"sleep timeout for watchdog">>                                               watchdog_timeout                = 1000U;
    Annotated<gr::Size_t, "timeout_inactivity_count", Doc<"inactive traversals before a park, or watchdog periods before a stall report">> timeout_inactivity_count        = 5U;
    Annotated<gr::Size_t, "process_stream_to_message_ratio", Doc<"number of stream to msg processing">>                                    process_stream_to_message_ratio = 16U;
    Annotated<std::string, "pool name", Doc<"default pool name">>                                                                          poolName                        = std::string(gr::thread_pool::kDefaultCpuPoolId);
    Annotated<std::size_t, "max_work_items", Doc<"number of work items per work scheduling interval (controls latency)">>                  max_work_items                  = std::numeric_limits<std::size_t>::max(); // TODO: check whether we can keep this std::size_t or more consistently to gr::Size_t
    Annotated<property_map, "sched_settings", Doc<"scheduler implementation specific settings">>                                           sched_settings{};

    GR_MAKE_REFLECTABLE(SchedulerBase, timeout_ms, watchdog_timeout, timeout_inactivity_count, process_stream_to_message_ratio, max_work_items, poolName, sched_settings);

    constexpr static block::Category blockCategory = block::Category::ScheduledBlockGroup;

    [[nodiscard]] static constexpr auto executionPolicy() { return execution; }

    // whether an idle worker parks on the graph's progress sequence until work or a wake ends the park
    [[nodiscard]] static constexpr bool parksIdleWorkers() { return executionPolicy() == ExecutionPolicy::singleThreadedBlocking; }

    // seq_cst: this store and the worker's _nWorkersInWork increment must not sink below the load that follows them
    void requestWorkQuiescence() {
        gr::atomic_ref(_workQuiescenceRequested).store_seq_cst(true);
        while (gr::atomic_ref(_nWorkersInWork).load_acquire() > 0) {
            std::this_thread::yield();
        }
    }

    void releaseWorkQuiescence() {
        gr::atomic_ref(_workQuiescenceRequested).store_release(false);
        wakeParkedWorkers();
    }

    [[nodiscard]] bool workerStarted() noexcept { return gr::atomic_ref(_nWorkersStarted).load_acquire() > 0UZ; }

    // the progress sequences of the graphs beyond the holder that can read or write this scheduler's exported rings
    void setOuterProgress(std::vector<std::shared_ptr<gr::Sequence>> outerProgress) { _outerProgress = std::move(outerProgress); }

    // why the latest start or resume could not complete and left the scheduler in ERROR. The next start clears it. The thread
    // that runs the start writes it before it publishes ERROR. Read it once the state reads ERROR or while no start is
    // in progress. A read during a start races with that write.
    [[nodiscard]] std::optional<Error> startError() const { return _startError; }

    // applies the settings staged since they were last applied. A setting that refuses its staged value keeps its
    // value, and the error names the scheduler and each refused key.
    [[nodiscard]] std::expected<void, Error> applyStagedSettings() {
        if (!this->settings().changed()) {
            return {};
        }
        const ApplyStagedParametersResult applied = this->settings().applyStagedParameters();
        if (applied.failedParameters.empty()) {
            return {};
        }
        std::string refusedKeys;
        for (const auto& [key, value] : applied.failedParameters) {
            refusedKeys += refusedKeys.empty() ? "" : ", ";
            refusedKeys += key;
        }
        return std::unexpected(Error(std::format("scheduler '{}' refused the staged settings {}", this->unique_name, refusedKeys)));
    }

    // ends a start whose move to RUNNING returned an error on a thread that is not one of this scheduler's workers. The
    // error goes out on msgOut through a writer of the caller's own and is kept for startError(). The scheduler then
    // stops the children that started and enters ERROR.
    void failStartAndReport(Error reason) {
        Message report;
        report.cmd         = message::Command::Notify;
        report.serviceName = this->unique_name;
        report.endpoint    = "start()";
        report.data        = std::unexpected(reason);
        publishOnOwnWriter(std::move(report));
        failStart(std::move(reason));
    }

    // a worker holds its pool thread for the run's lifetime, so a start into a pool whose threads are all held
    // queues that worker behind them for as long as the holders live. A queued task counts as held
    [[nodiscard]] std::expected<void, Error> checkWorkerCapacity() const {
        if constexpr (executionPolicy() == ExecutionPolicy::multiThreaded) {
            const std::size_t nThreads = static_cast<std::size_t>(_pool->maxThreads());
            const std::size_t nBusy    = std::min(_pool->numTasksRunning() + _pool->numTasksQueued(), nThreads);
            if (nBusy >= nThreads) {
                return std::unexpected(Error(std::format("thread pool '{}' runs {} of its {} threads and has none free for a worker of '{}'", _pool->name(), nBusy, nThreads, this->unique_name)));
            }
        }
        return {};
    }

    void requestWorkQuiescenceAll() {
        requestWorkQuiescence();
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [](auto& block) {
            if (block->blockCategory() == block::Category::ScheduledBlockGroup) {
                if (auto* sm = dynamic_cast<SchedulerModel*>(block.get())) {
                    sm->requestWorkQuiescence();
                }
            }
        });
    }

    void releaseWorkQuiescenceAll() {
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [](auto& block) {
            if (block->blockCategory() == block::Category::ScheduledBlockGroup) {
                if (auto* sm = dynamic_cast<SchedulerModel*>(block.get())) {
                    sm->releaseWorkQuiescence();
                }
            }
        });
        releaseWorkQuiescence();
    }

    struct WorkQuiescenceGuard {
        SchedulerBase* _scheduler;
        explicit WorkQuiescenceGuard(SchedulerBase* s) : _scheduler(s) { _scheduler->requestWorkQuiescenceAll(); }
        ~WorkQuiescenceGuard() { _scheduler->releaseWorkQuiescenceAll(); }
        WorkQuiescenceGuard(const WorkQuiescenceGuard&)            = delete;
        WorkQuiescenceGuard& operator=(const WorkQuiescenceGuard&) = delete;
    };

    SchedulerBase() : base_t(gr::property_map()) { registerPropertyCallbacks(); }

    SchedulerBase(std::initializer_list<std::pair<const std::pmr::string, pmt::Value>> initParameter) noexcept(false) : base_t(initParameter) {
        registerPropertyCallbacks();
        std::ignore = this->settings().set(initParameter);
        std::ignore = this->settings().activateContext();
        std::ignore = this->settings().applyStagedParameters();
    }

    explicit SchedulerBase(property_map initParameters) noexcept(false) : base_t(initParameters) {
        registerPropertyCallbacks();
        std::ignore = this->settings().set(initParameters);
        std::ignore = this->settings().activateContext();
        std::ignore = this->settings().applyStagedParameters();
    }

    ~SchedulerBase() {
        {
            // start() reads the flag under this lock. A start that read it unset has published RUNNING already, and
            // the loop below stops that run. A pending swap is dropped and does not restart the scheduler.
            std::lock_guard guard(_runMutex);
            gr::atomic_ref(_run.destroying).store_release(true);
            _pendingExchange.reset();
        }

        // A swap or a restart claimed before the flag was set runs outside the job count. It can restart the run after
        // a pass has read the state, and that restart spawns a watchdog. Each pass stops an active run, waits for the
        // workers and the claims, then retires the watchdogs and waits for them. The watchdog dereferences
        // SchedulerBase. The passes repeat until no run, worker, claim or watchdog remains.
        auto nDeferred  = [this] { return gr::atomic_ref(_nDeferredExchanges).load_acquire(); };
        auto nWatchdogs = [this] { return gr::atomic_ref(_nWatchdogsRunning).load_acquire(); };
        do {
            if (lifecycle::isActive(this->state())) { // RUNNING, REQUESTED_PAUSE or PAUSED -- workers are parked, not gone
                if (auto e = this->changeStateTo(lifecycle::REQUESTED_STOP); !e) {
                    std::println(std::cerr, "Failed to stop execution at destruction of scheduler: {} ({})", e.error().message, e.error().srcLoc());
                    std::abort();
                }
            }
            waitForRunAndClaims();
            retireWatchdogs();
        } while (lifecycle::isActive(this->state()) || _nRunningJobs->value() != 0UZ || nDeferred() != 0UZ || nWatchdogs() != 0UZ);

        _executionOrder.reset(); // force earlier crashes if this is accessed after destruction (e.g. from thread that was kept running)
    }

    [[nodiscard]] bool isOnOwnWorkerThread() const noexcept { return activeSchedulerWorker() == static_cast<const void*>(this); }

    // A stop requested while the scheduler is stopping or stopped changes no state. satisfiedBy is the state that the
    // request read. While the state still satisfies a stop, the request advances the run generation. A swap that
    // restarts the run reads the generation once it has left STOPPED. The state can leave STOPPED between the
    // request's read and this call. A restart by restoreRun() that saw no start or stop since its own stop, other than
    // the start it runs itself, belongs to the swap: the request advances the generation and stops the run that the
    // swap restores. Any other start or reset in that interval follows the stop, and the request changes nothing.
    void transitionSatisfied(lifecycle::State requested, lifecycle::State satisfiedBy) {
        using enum lifecycle::State;
        if (requested != REQUESTED_STOP) {
            return;
        }
        const bool stopRestoredRun = [this, satisfiedBy] {
            std::lock_guard        guard(_runMutex);
            const lifecycle::State current = this->state();
            if (current == satisfiedBy || lifecycle::isShuttingDown(current)) {
                gr::atomic_ref(_run.generation).fetch_add(1UZ);
                return false;
            }
            const std::size_t generation = _run.generation;
            // restoreRun()'s own start advances the generation once
            const bool restoring = _run.restoredFrom.has_value() && (generation == *_run.restoredFrom || generation == *_run.restoredFrom + 1UZ);
            if (restoring) {
                gr::atomic_ref(_run.generation).fetch_add(1UZ);
            }
            return restoring;
        }();
        if (stopRestoredRun) {
            this->emitErrorMessageIfAny("transitionSatisfied() -> REQUESTED_STOP", this->changeStateTo(REQUESTED_STOP));
        }
    }

    [[nodiscard]] std::expected<meta::indirect<Graph>, Error> exchange(meta::indirect<Graph>&& newGraph, const profiling::Options& option = {}) {
        using enum lifecycle::State;
        const auto oldState = this->state();

        if (isOnOwnWorkerThread()) {
            // a message handler on a worker of this scheduler cannot wait for the job count to reach zero. Its own count
            // is part of it. Request the stop and record the graph. The worker that claims the record applies the swap
            // once every other worker has left.
            const bool  restart       = lifecycle::isActive(oldState);
            std::size_t ownGeneration = 0UZ;
            if (restart) {
                ownGeneration = runGeneration() + 1UZ; // the stop below advances the generation once
                if (auto result = this->changeStateTo(REQUESTED_STOP); !result) {
                    return std::unexpected(result.error());
                }
            }
            {
                std::lock_guard guard(_runMutex);
                const bool      joinsClaim = _pendingExchange.has_value() && _pendingExchange->claimed;
                _pendingExchange           = PendingExchange{std::move(newGraph), option, restart, ownGeneration, std::this_thread::get_id(), joinsClaim};
            }
            return meta::indirect<Graph>{};
        }

        if (auto allowed = swapAllowedFromThisThread(); !allowed) {
            return std::unexpected(allowed.error());
        }

        if constexpr (requires(Derived& d) { d.customExchange(); }) { // runs before the swap is claimed, with the state still free to change
            static_cast<Derived*>(this)->customExchange();
        }

        // an active scheduler is stopped below through the state transitions. An inactive one is claimed instead:
        // another thread may start it after oldState was read
        const bool  claimed                     = !lifecycle::isActive(oldState);
        const void* previousExchangingScheduler = exchangingScheduler();
        if (claimed) {
            claimExchange();
            exchangingScheduler() = this;
        }
        on_scope_exit releaseClaim = [this, claimed, previousExchangingScheduler] {
            if (claimed) {
                exchangingScheduler() = previousExchangingScheduler;
                releaseExchange();
            }
        };
        if (lifecycle::State observed = oldState; claimed && !gr::atomic_ref(this->_state).compare_exchange(observed, oldState)) {
            return std::unexpected(Error(std::format("exchange(): the scheduler '{}' changed from {} to {} before the swap; the graph is unchanged", this->unique_name, gr::meta::enumName(oldState).value_or(""), gr::meta::enumName(observed).value_or(""))));
        }

        std::size_t ownGeneration = 0UZ;
        if (lifecycle::isActive(oldState)) {       // need to stop running scheduler
            ownGeneration = runGeneration() + 1UZ; // the stop below advances the generation once
            if (auto result = this->changeStateTo(REQUESTED_STOP); !result) {
                return std::unexpected(result.error());
            }
            waitDone(); // wait for all jobs to complete

            if (this->state() != ERROR) { // a block that failed before the stop leaves ERROR, and reset() below leaves it
                if (auto result = this->changeStateTo(STOPPED); !result) {
                    return std::unexpected(result.error());
                }
            }
        }

        if (this->state() == ERROR || this->state() == STOPPED) {
            reset(); // reset internal states
        }

        retireWatchdogs(); // the watchdog reads _graph
        auto oldGraph = std::exchange(_graph, std::move(newGraph));

        if ((option != profiling::Options{})) { // need to update profiler
            rebuildProfiler(option);
        }

        if (_pool->name() != std::string_view(poolName.value)) { // sync pool with (possibly updated) poolName setting
            const std::string_view requested{poolName.value};
            try {
                _pool = gr::thread_pool::Manager::instance().get(requested);
            } catch (const std::exception& e) {
                this->emitErrorMessage("exchange(poolName)", std::format("unknown thread pool '{}': {}; keeping existing pool '{}'", requested, e.what(), _pool->name()));
            }
        }

        if (lifecycle::isActive(oldState)) {
            if (auto result = restoreRun(ownGeneration, oldState); !result) {
                return std::unexpected(result.error());
            }
        }
        return oldGraph;
    }

    [[nodiscard]] const gr::Graph& graph() const noexcept { return *_graph; }
    [[nodiscard]] gr::Graph&       graph() noexcept { return *_graph; }

    [[nodiscard]] const TProfiler& profiler() const noexcept { return _profiler; }

    [[nodiscard]] bool isProcessing() const
    requires(executionPolicy() == ExecutionPolicy::multiThreaded)
    {
        return _nRunningJobs->value() > 0UZ;
    }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings) noexcept {
        if (!newSettings.contains("poolName")) {
            return;
        }
        const std::string_view requested{poolName.value};
        if (!_pool || _pool->name() == requested) {
            return;
        }
        try {
            _pool = gr::thread_pool::Manager::instance().get(requested);
        } catch (const std::exception& e) {
            this->emitErrorMessage("settingsChanged(poolName)", std::format("unknown thread pool '{}': {}; keeping existing pool '{}'", requested, e.what(), _pool->name()));
        }
    }

    void stateChanged(lifecycle::State newState) {
        if (newState == lifecycle::State::INITIALISED || newState == lifecycle::State::RUNNING) {
            std::lock_guard guard(_runMutex);
            _run.phase = newState == lifecycle::State::INITIALISED ? RunPhase::awaiting : RunPhase::starting;
        }
        this->notifyListeners(block::property::kLifeCycleState, {{"state", std::string(gr::meta::enumName(newState).value_or(""))}});
    }

    [[nodiscard]] std::span<std::shared_ptr<BlockModel>>       blocks() noexcept { return _graph->blocks(); }
    [[nodiscard]] std::span<const std::shared_ptr<BlockModel>> blocks() const noexcept { return _graph->blocks(); }
    [[nodiscard]] std::span<Edge>                              edges() noexcept { return _graph->edges(); }
    [[nodiscard]] std::span<const Edge>                        edges() const noexcept { return _graph->edges(); }

    void connectBlockMessagePorts() {
        const auto available = _graph->msgIn.available();
        if (available != 0UZ) {
            ReaderSpanLike auto msgInSpan = _graph->msgIn.streamReader().get<SpanReleasePolicy::ProcessAll>(available);
            _pendingMessagesToChildren.insert(_pendingMessagesToChildren.end(), msgInSpan.begin(), msgInSpan.end());
        }

        auto toSchedulerBuffer = _fromChildMessagePort.buffer();
        if (!_toChildMessagePort.connect(_graph->msgIn)) {
            this->emitErrorMessage("connectBlockMessagePorts()", "Failed to connect scheduler input message port to graph msgIn");
        }
        _graph->msgOut.setBuffer(toSchedulerBuffer.streamBuffer, toSchedulerBuffer.tagBuffer);

        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this, &toSchedulerBuffer](auto& block) {
            if (!_toChildMessagePort.connect(*block->msgIn)) {
                this->emitErrorMessage("connectBlockMessagePorts()", std::format("Failed to connect scheduler input message port to child '{}'", block->uniqueName()));
            }

            block->msgOut->setBuffer(toSchedulerBuffer.streamBuffer, toSchedulerBuffer.tagBuffer);
        });

        // Forward any messages to children that were received before the scheduler was initialised
        _messagePortsConnected = true;

        WriterSpanLike auto msgSpan = _toChildMessagePort.streamWriter().template reserve<SpanReleasePolicy::ProcessAll>(_pendingMessagesToChildren.size());
        std::ranges::move(_pendingMessagesToChildren, msgSpan.begin());
        _pendingMessagesToChildren.clear();
    }

    void processMessages(gr::MsgPortInBuiltin& port, std::span<const gr::Message> messages) {
        base_t::processMessages(port, messages); // filters messages and calls own property handler

        bool forwarded = false;
        for (const gr::Message& msg : messages) {
            if (msg.serviceName != this->unique_name && msg.serviceName != this->name && msg.endpoint != block::property::kLifeCycleState) {
                // only forward wildcard, non-scheduler messages, and non-lifecycle messages (N.B. the latter is exclusively handled by the scheduler)
                if (_messagePortsConnected) {
                    WriterSpanLike auto msgSpan = _toChildMessagePort.streamWriter().template reserve<SpanReleasePolicy::ProcessAll>(1UZ);
                    msgSpan[0]                  = msg;
                    forwarded                   = true;
                } else {
                    // if not yet connected, keep messages to children in cache and forward when connecting
                    _pendingMessagesToChildren.push_back(msg);
                }
            }
        }
        if (forwarded) {
            wakeParkedWorkers();
        }
    }

    // callable from any thread. One caller at a time runs the scheduler's and the graph's handlers, and a caller that
    // finds them running returns at once
    void processScheduledMessages() {
        if (std::atomic_flag_test_and_set_explicit(&_processingScheduledMessages, std::memory_order_acquire)) {
            return;
        }

        on_scope_exit _ = [&] { std::atomic_flag_clear_explicit(&_processingScheduledMessages, std::memory_order_release); };

        base_t::processScheduledMessages(); // filters messages and calls own property handler

        // Process messages in the graph
        _graph->processScheduledMessages();
        if (_nRunningJobs->value() == 0UZ) {
            graph::forEachBlock<TransparentBlockGroup>(*_graph, [](auto& block) { block->processScheduledMessages(); });
        }

        forwardMessagesFromChildren();
    }

    // drains _fromChildMessagePort on every path: the ring is shared by all blocks, so an unconsumed span
    // both wedges the next emitter and latches the workers' message-poll gate permanently open
    void forwardMessagesFromChildren() {
        ReaderSpanLike auto messagesFromChildren = _fromChildMessagePort.streamReader().get();
        const std::size_t   nFromChildren        = messagesFromChildren.size();
        if (nFromChildren == 0UZ) {
            if (!_unforwardedReplies.empty() && this->msgOut.nReaders() != 0) {
                forwardToMsgOut(std::span<const gr::Message>{});
            }
            return;
        }

        std::optional<Error> firstError;
        std::string          firstErrorChild;
        for (const gr::Message& message : messagesFromChildren) {
            if (message.data.has_value()) {
                continue;
            }
            const Error& reason = message.data.error();
            Error        named{std::format("block '{}' reports an error on '{}': {}", message.serviceName, message.endpoint, reason.message), reason.sourceLocation, reason.errorTime};
            if (!firstError.has_value()) {
                firstError      = named;
                firstErrorChild = message.serviceName;
            }
            if (!_firstErrorFromChildren.has_value()) {
                _firstErrorFromChildren = named;
            }
            _latestErrorByChild.insert_or_assign(message.serviceName, std::move(named));
        }

        if (this->msgOut.nReaders() == 0) {
            // nobody is listening on messages -> convert errors to exceptions
            if (!messagesFromChildren.consume(nFromChildren)) {
                this->emitErrorMessage("process child return messages", "Failed to consume messages from child message port");
            }
            if (firstError.has_value()) {
                if (isOnOwnWorkerThread()) { // the worker's pool would drop the exception
                    failRun(firstErrorChild, activeWorkerGeneration(), "reported an error");
                    return;
                }
                throw gr::exception(firstError->message, firstError->sourceLocation);
            }
            return;
        }

        forwardToMsgOut(std::span<const gr::Message>(messagesFromChildren.begin(), messagesFromChildren.end()));
        if (!messagesFromChildren.consume(nFromChildren)) {
            this->emitErrorMessage("process child return messages", "Failed to consume messages from child message port");
        }
    }

    // forwards what fits in msgOut and never blocks: a subscriber that stops consuming must not block a worker. The
    // replies kept from earlier calls go first, in order. A notification that does not fit is dropped and counted. Any
    // other message that does not fit is kept for a later call. When the kept replies outnumber the slots of msgOut,
    // the oldest is dropped and counted.
    void forwardToMsgOut(std::span<const gr::Message> messages) {
        auto&             msgWriter = this->msgOut.streamWriter();
        const std::size_t nPending  = _unforwardedReplies.size() + messages.size();
        const std::size_t nToSend   = std::min(nPending, msgWriter.available());
        std::size_t       nSent     = 0UZ;
        if (nToSend > 0UZ) {
            WriterSpanLike auto msgSpan = msgWriter.template tryReserve<SpanReleasePolicy::ProcessAll>(nToSend);
            nSent                       = msgSpan.size();
            const std::size_t nKept     = std::min(nSent, _unforwardedReplies.size());
            std::ranges::move(_unforwardedReplies.begin(), _unforwardedReplies.begin() + static_cast<std::ptrdiff_t>(nKept), msgSpan.begin());
            std::ranges::copy_n(messages.begin(), static_cast<std::ptrdiff_t>(nSent - nKept), msgSpan.begin() + static_cast<std::ptrdiff_t>(nKept));
            msgSpan.publish(nSent);
            _unforwardedReplies.erase(_unforwardedReplies.begin(), _unforwardedReplies.begin() + static_cast<std::ptrdiff_t>(nKept));
            nSent -= nKept;
        }
        std::size_t nDropped = 0UZ;
        for (const gr::Message& message : messages.subspan(nSent)) {
            if (message.cmd == message::Command::Notify) {
                ++nDropped;
            } else {
                _unforwardedReplies.push_back(message);
            }
        }
        if (const std::size_t capacity = this->msgOut.bufferSize(); _unforwardedReplies.size() > capacity) {
            const std::size_t nOverflow = _unforwardedReplies.size() - capacity;
            _unforwardedReplies.erase(_unforwardedReplies.begin(), _unforwardedReplies.begin() + static_cast<std::ptrdiff_t>(nOverflow));
            nDropped += nOverflow;
        }
        if (nDropped > 0UZ) {
            message::droppedMessageCount().fetch_add(nDropped, std::memory_order_relaxed);
        }
    }

    std::expected<void, Error> runAndWait() {
        using enum lifecycle::State;
        [[maybe_unused]] const auto pe = this->_profilerHandler->startCompleteEvent("scheduler_base.runAndWait");

        // a stop that arrives while no run has begun belongs to the run this call would start. No run loop observes
        // it, and the reinitialization below would erase the STOPPED it produced. This call honors it in place of the
        // run and counts as that run on the way out
        on_scope_exit endAwaited = [this] { endAwaitedRun(); };

        auto settleStopped = [this]() -> std::expected<void, Error> {
            if (this->state() == RUNNING) {
                if (auto e = this->changeStateTo(REQUESTED_STOP); !e) {
                    this->emitErrorMessage("runAndWait() -> LifecycleState", e.error());
                    return std::unexpected(e.error());
                }
            }
            if (this->state() == REQUESTED_STOP) {
                if (auto e = this->changeStateTo(STOPPED); !e) {
                    this->emitErrorMessage("runAndWait() -> LifecycleState", e.error());
                }
            }
            processScheduledMessages();
            return {};
        };

        processScheduledMessages(); // make sure initial subscriptions are processed
        const bool runAwaited = [this] {
            std::lock_guard guard(_runMutex);
            return _run.phase == RunPhase::awaiting;
        }();
        if (lifecycle::isShuttingDown(this->state()) && runAwaited) {
            return settleStopped();
        }
        if (this->state() == STOPPED || this->state() == ERROR) {
            if (auto e = this->changeStateTo(INITIALISED); !e) {
                this->emitErrorMessage("runAndWait() -> LifecycleState", e.error());
                return std::unexpected(e.error());
            }
        }
        if (this->state() == IDLE) {
            if (auto e = this->changeStateTo(INITIALISED); !e) {
                this->emitErrorMessage("runAndWait() -> LifecycleState", e.error());
                return std::unexpected(e.error());
            }
        }
        if (auto e = this->changeStateTo(RUNNING); !e) {
            if (!lifecycle::isShuttingDown(this->state())) {
                this->emitErrorMessage("runAndWait() -> LifecycleState", e.error());
                return std::unexpected(e.error());
            }
            return settleStopped(); // a stop claimed the transition first
        }

        // N.B. the transition to lifecycle::State::RUNNING will for the ExecutionPolicy:
        // * singleThreaded[Blocking] naturally block in the calling thread
        // * multiThreaded[Blocking] spawn two worker and block on 'waitDone()'
        // A swap or a restart that a worker requested runs the next run of this call.
        waitForRunAndClaims();

        // the message drain can throw when a child's error message would otherwise be dropped
        // (no msgOut subscriber); at this boundary that becomes the call's own error vocabulary
        std::expected<void, Error> settled = [&]() -> std::expected<void, Error> {
            try {
                processScheduledMessages();
                return settleStopped();
            } catch (const std::exception& e) {
                return std::unexpected(Error{std::format("runAndWait(): {}", e.what())});
            }
        }();
        if (!settled) {
            return settled;
        }
        if (_startError.has_value()) {
            return std::unexpected(*_startError);
        }
        if (this->state() == ERROR || runEndingBlock().has_value()) {
            return std::unexpected(runEndingError());
        }
        return {};
    }

    void waitDone() {
        [[maybe_unused]] const auto pe = _profilerHandler->startCompleteEvent("scheduler_base.waitDone");
        for (std::size_t nRunning = _nRunningJobs->value(); nRunning > 0UZ; nRunning = _nRunningJobs->value()) {
            _nRunningJobs->wait(nRunning);
        }
    }

    [[nodiscard]] std::shared_ptr<JobLists> jobs() const noexcept { return _executionOrder; }

protected:
    // the first block whose work() returned ERROR, or whose error a worker drained with no msgOut reader, since the
    // latest start
    [[nodiscard]] std::optional<std::string> runEndingBlock() {
        std::lock_guard guard(_runEndingBlockMutex);
        return _runEndingBlock;
    }

    // the error of a run that ended in ERROR, or of a run that a block's error after the stop ended. It names the first
    // block that ended the run and carries that block's latest reported error, or the name alone when the block
    // reported none. A run that no block ended returns the first error any child reported
    [[nodiscard]] Error runEndingError() {
        const std::optional<std::string> endingBlock = runEndingBlock();
        if (endingBlock.has_value()) {
            if (const auto reported = _latestErrorByChild.find(*endingBlock); reported != _latestErrorByChild.end()) {
                return reported->second;
            }
            return Error{std::format("block '{}' ended the run: its work() returned ERROR", *endingBlock)};
        }
        return _firstErrorFromChildren.value_or(Error{"a block error ended the run: the scheduler finished in the ERROR state"});
    }

    // A worker meets an ERROR from a block's work() or from a child's error that no msgOut reader takes. The block or
    // child is recorded for runAndWait() unless an earlier one was, and a message on msgOut names it. While the worker's
    // run is current, the scheduler moves to ERROR after that message. A scheduler that runs this one and reads ERROR
    // then finds the message. Otherwise a stop retired the run while the worker met the error. stop() publishes STOPPED
    // without waiting for the worker, and the state stays STOPPED. A restart or a graph swap that follows the stop
    // proceeds. runAndWait() returns the error.
    void failRun(std::string_view blockName, std::size_t generation, std::string_view what) {
        {
            std::lock_guard guard(_runEndingBlockMutex);
            if (!_runEndingBlock.has_value()) {
                _runEndingBlock = std::string(blockName);
            }
        }
        const bool runIsCurrent = gr::atomic_ref(_run.generation).load_acquire() == generation;
        Message    report;
        report.cmd         = message::Command::Notify;
        report.serviceName = this->unique_name;
        report.endpoint    = runIsCurrent ? "error in the run" : "error after the stop";
        report.data        = std::unexpected(Error{std::format("block '{}' {}{}", blockName, what, runIsCurrent ? "" : " after the stop reached it")});
        publishOnOwnWriter(std::move(report));
        if (runIsCurrent) {
            this->emitErrorMessageIfAny("LifecycleState (ERROR)", this->changeStateTo(lifecycle::State::ERROR));
        }
    }

    void disconnectAllEdges() {
        _graph->disconnectAllEdges();
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [](auto& block) {
            if (block->blockCategory() == TransparentBlockGroup) {
                auto* graph = static_cast<GraphWrapper<gr::Graph>*>(block.get());
                graph->blockRef().disconnectAllEdges();
            }
        });
    }

    // connects the pending edges of the graph and of its transparent subgraphs and primes the feedback loops. The
    // result lists the edges that did not connect: a port that does not exist, two ports of different types, or a
    // connection that the source port refused
    [[nodiscard]] std::vector<Edge> connectPendingEdges() {
        auto primeFeedbackPorts = [&](const gr::Graph& graph) {
            std::vector<graph::FeedbackLoop> feedbackLoops = gr::graph::detectFeedbackLoops(graph);
            for (auto& loop : feedbackLoops) {
                if (std::expected<std::size_t, Error> nPrimeSamples = gr::graph::calculateLoopPrimingSize(loop); nPrimeSamples) {
                    if (auto ret = gr::graph::primeLoop(loop, nPrimeSamples.value()); !ret) {
                        this->emitErrorMessage("connectPendingEdges()", std::format("failed to prime feedback loop: {}\nloop: {}", ret.error(), loop.edges));
                    }
                } else {
                    this->emitErrorMessage("connectPendingEdges()", std::format("failed to prime feedback loop: {}\nloop: {}", nPrimeSamples.error(), loop.edges));
                }
            }
        };

        _graph->connectPendingEdges();
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [](auto& block) {
            if (block->blockCategory() == TransparentBlockGroup) {
                auto* graph = static_cast<GraphWrapper<gr::Graph>*>(block.get());
                graph->blockRef().connectPendingEdges();
            }
        });
        // priming writes into the buffer of a connected edge. It runs once, after every transparent group's edges are
        // connected, and the flattened graph holds each loop inside a group.
        primeFeedbackPorts(gr::graph::flatten(*_graph)); // need to flatten graph due to potential loops from within the subgraph to blocks in the parents.

        std::vector<Edge> unconnected;
        graph::forEachEdge<TransparentBlockGroup>(*_graph, [&unconnected](const Edge& edge) {
            using enum Edge::EdgeState;
            if (edge.state() == ErrorConnecting || edge.state() == PortNotFound || edge.state() == IncompatiblePorts) {
                unconnected.push_back(edge);
            }
        });
        return unconnected;
    }

    // names each edge with its blocks, its ports and the state that kept it from connecting
    [[nodiscard]] static Error unconnectedEdgesError(const std::vector<Edge>& unconnected) {
        std::string edges;
        for (const Edge& edge : unconnected) {
            edges += std::format("{}{:l}", edges.empty() ? "" : "; ", edge);
        }
        return Error(std::format("edges could not be connected: {}", edges));
    }

    // idle worker backoff: hot spin, then yield, then a sleep doubling up to kMaxIdleSleep
    static constexpr std::size_t kIdleSpinIterations  = 100UZ;
    static constexpr std::size_t kIdleYieldIterations = 8UZ;
    static constexpr auto        kMaxIdleSleep        = std::chrono::microseconds(200);

    static void applyIdleBackoff(std::size_t nIdleIterations) noexcept {
        if (nIdleIterations <= kIdleSpinIterations) {
            return;
        }
        if (nIdleIterations <= kIdleSpinIterations + kIdleYieldIterations) {
            std::this_thread::yield();
            return;
        }
        const std::size_t shift = std::min(nIdleIterations - (kIdleSpinIterations + kIdleYieldIterations), 8UZ);
        std::this_thread::sleep_for(std::min(kMaxIdleSleep, std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(1UZ << shift))));
    }

    // a block whose work() returns ERROR ends the traversal, and the first such block since the start names the run's error
    work::Result traverseBlockListOnce(const std::vector<std::shared_ptr<BlockModel>>& blocks) {
        const std::size_t requestedWorkAllBlocks = max_work_items;
        std::size_t       performedWorkAllBlocks = 0UZ;
        bool              unfinishedBlocksExist  = false; // i.e. at least one block returned OK, INSUFFICIENT_INPUT_ITEMS, or INSUFFICIENT_OUTPU_ITEMS
        bool              exportedBlockMoved     = false;
        for (auto& currentBlock : blocks) {
            const auto [requested_work, performed_work, status] = currentBlock->work(requestedWorkAllBlocks);
            performedWorkAllBlocks += performed_work;
            if (performed_work > 0UZ && !exportedBlockMoved && isExported(*currentBlock)) {
                exportedBlockMoved = true;
            }

            if (status == work::Status::ERROR) {
                std::lock_guard guard(_runEndingBlockMutex);
                if (!_runEndingBlock.has_value()) {
                    _runEndingBlock = std::string(currentBlock->uniqueName());
                }
                return {requested_work, performedWorkAllBlocks, work::Status::ERROR};
            }
            // A block group performs no work of its own: Block::work() reports OK for every non-NormalBlock
            // category without consulting anything. graph::flatten puts the group in the execution order next
            // to the children it contains, so counting its permanent OK as unfinished work makes the all-DONE
            // condition unreachable and runAndWait() never returns. The children are in this same list and are
            // what termination is decided on. The group stays in the list because it also has to drain its
            // message port.
            if (status != work::Status::DONE && currentBlock->blockCategory() == block::Category::NormalBlock) {
                unfinishedBlocksExist = true;
            }
        }
#ifdef __EMSCRIPTEN__
        std::this_thread::sleep_for(std::chrono::microseconds(10u)); // workaround for incomplete std::atomic implementation (at least it seems for nodejs)
#endif
        advanceEnclosingProgress(exportedBlockMoved, !unfinishedBlocksExist);
        return {max_work_items, performedWorkAllBlocks, unfinishedBlocksExist ? work::Status::OK : work::Status::DONE};
    }

    void init() {
        awaitExchange();
        [[maybe_unused]] const auto pe = _profilerHandler->startCompleteEvent("scheduler_base.init");
        base_t::processScheduledMessages(); // make sure initial subscriptions are processed
        connectBlockMessagePorts();

        if constexpr (requires(Derived& d) { d.customInit(); }) {
            static_cast<Derived*>(this)->customInit();
        }
        _jobListsPool = _pool.get();
    }

    // re-entering INITIALISED must rebuild the same execution state that init() builds, because the graph
    // may have been exchanged or edited since: a stale _executionOrder runs the previous graph's blocks
    void reset() {
        awaitExchange();
        // waits for the previous run's workers before reinitializing the blocks. A worker still traversing would call
        // work() on a block whose edges reset() disconnects. stop() retired the workers, so the wait lasts at most one
        // traversal.
        waitForOtherWorkers();
        gr::atomic_ref(_nWorkersStarted).store_release(0UZ); // workerStarted() reports the next start's workers, not the last run's
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) { this->emitErrorMessageIfAny("reset() -> LifecycleState", block->changeStateTo(lifecycle::INITIALISED)); });
        disconnectAllEdges();
        connectBlockMessagePorts();

        if constexpr (requires(Derived& d) { d.customReset(); }) {
            static_cast<Derived*>(this)->customReset();
        } else if constexpr (requires(Derived& d) { d.customInit(); }) {
            static_cast<Derived*>(this)->customInit();
        }
        _jobListsPool = _pool.get();
    }

    // ends a start or a resume that cannot complete. The children that did start are stopped. The reason is kept for
    // startError() and runAndWait(). The scheduler enters ERROR, which only a reset leaves, and its stop() retires the
    // workers of a resumed run. A start returns without spawning a worker, and no run loop then exists to settle the
    // state later.
    void failStart(Error reason) {
        _startError = std::move(reason);
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) { stopChild(*block, "failStart() -> LifecycleState"); });
        this->emitErrorMessageIfAny("failStart() -> LifecycleState -> ERROR", this->changeStateTo(lifecycle::State::ERROR));
    }

    void start() {
        using enum gr::lifecycle::State;
        if (isOnOwnWorkerThread()) {
            deferStart();
            return;
        }
        on_scope_exit markStarted = [this] {
            std::lock_guard guard(_runMutex);
            if (_run.phase == RunPhase::starting) {
                _run.phase = RunPhase::started;
            }
        };
        awaitExchange();

        // A start that reads the destructor's flag requests a stop and returns before it counts or dispatches a
        // worker. The destructor sets the flag under the same lock and then reads the state.
        std::optional<std::size_t> workerGeneration;
        {
            std::lock_guard guard(_runMutex);
            if (!_run.destroying) {
                workerGeneration = gr::atomic_ref(_run.generation).fetch_add(1UZ) + 1UZ;
            }
        }
        if (!workerGeneration.has_value()) {
            this->emitErrorMessageIfAny("start() -> REQUESTED_STOP", this->changeStateTo(REQUESTED_STOP));
            return;
        }

        // stop() publishes STOPPED and retires the run's workers by generation, without waiting for
        // them. A worker the pool counted but has not started releases its count when the pool reaches
        // it, and a worker in its loop leaves at its next check. This run begins only once every count
        // of the previous run is released. A worker that started after this point would run the
        // previous job list, whose blocks are stopped. It would make no progress, reach no terminal
        // state, and hold _nRunningJobs above zero indefinitely. The advance of the generation above also
        // retires a run that ended other than through stop().
        //
        // The drain must happen before _executionOrderMutex is acquired: a queued worker acquires
        // that mutex to copy its job list before it can decrement _nRunningJobs, so waiting for it
        // while holding the mutex deadlocks the restart. A recursive mutex does not help, because
        // the two are different threads. Draining first also keeps the graph from being rewired and
        // keeps children and the watchdog from starting while the previous run unwinds.
        waitDone();
        gr::atomic_ref(_nWorkersStarted).store_release(0UZ);
        _startError.reset();

        // the settings staged since the scheduler initialized take effect before the run's job lists are fixed. A pool
        // other than the one the lists were sized on sizes them again
        if (std::expected<void, Error> applied = applyStagedSettings(); !applied.has_value()) {
            this->emitErrorMessage("start()", applied.error());
            failStart(std::move(applied.error()));
            return;
        }
        if constexpr (requires(Derived& d) { d.customInit(); }) {
            if (_pool.get() != _jobListsPool) {
                static_cast<Derived*>(this)->customInit();
                _jobListsPool = _pool.get();
            }
        }

        disconnectAllEdges();
        if (const std::vector<Edge> unconnected = connectPendingEdges(); !unconnected.empty()) {
            Error reason = unconnectedEdgesError(unconnected);
            this->emitErrorMessage("start()", reason);
            failStart(std::move(reason));
            return;
        }
        if (this->state() == IDLE) {
            if (auto result = this->changeStateTo(INITIALISED); !result) { // Need to go to INITIALISED first
                this->emitErrorMessage("start()", result.error());
            }
        }
        _firstErrorFromChildren.reset();
        _latestErrorByChild.clear();
        {
            std::lock_guard guard(_runEndingBlockMutex);
            _runEndingBlock.reset();
        }

        std::lock_guard lock(_executionOrderMutex);

        recordExportedBlocks();
        handProgressToNestedSchedulers();

        std::optional<Error>                     firstChildError;
        std::vector<std::shared_ptr<BlockModel>> startedSubSchedulers;
        // the sweep runs whole under this lock, released before the workers are dispatched: a worker requesting a stop takes it
        {
            std::lock_guard childLock(_childLifecycleMutex);
            if (lifecycle::isShuttingDown(this->state())) {
                return;
            }
            graph::forEachBlock<TransparentBlockGroup>(*_graph, [this, &firstChildError, &startedSubSchedulers](auto& block) { //
                if (block->blockCategory() == ScheduledBlockGroup) {
                    // We don't simply move to RUNNING, as schedulers block. This code path
                    // uses a separate thread. A sub-scheduler that cannot reach RUNNING refuses the start, and the
                    // refusal fails this start as a child's error does.
                    auto* schedulerModel = dynamic_cast<SchedulerModel*>(block.get());
                    if (schedulerModel) {
                        if (std::expected<void, Error> started = schedulerModel->start(); started.has_value()) {
                            startedSubSchedulers.push_back(block);
                        } else {
                            this->emitErrorMessage("start()", started.error());
                            if (!firstChildError.has_value()) {
                                firstChildError = std::move(started.error());
                            }
                        }
                    } else {
                        throw gr::exception(std::format("ScheduledBlockGroup is not a SchedulerModel {}", block->uniqueName()));
                    }
                } else {
                    // a child that never initialized or refuses to start — a throwing init or start() hook
                    // leaves the block unable to run — must fail the run itself, not only the message
                    // stream: the first such error is what runAndWait() returns
                    std::optional<Error>       initFailure = block->initError();
                    std::expected<void, Error> transitioned;
                    if (initFailure.has_value()) {
                        transitioned = std::unexpected(std::move(*initFailure));
                    } else {
                        transitioned = block->changeStateTo(lifecycle::RUNNING);
                    }
                    if (!transitioned && !firstChildError.has_value()) {
                        firstChildError = transitioned.error();
                    }
                    this->emitErrorMessageIfAny("LifecycleState -> RUNNING", std::move(transitioned));
                }
            });
        }

        // a sub-scheduler starts on its own thread and can fail after its start() returned. Its outcome is awaited here,
        // outside the sweep's lock, as an adoption awaits it
        for (const std::shared_ptr<BlockModel>& subScheduler : startedSubSchedulers) {
            auto* schedulerModel = dynamic_cast<SchedulerModel*>(subScheduler.get());
            if (awaitSubSchedulerStart(*subScheduler, *schedulerModel) == SubSchedulerStart::failed && !firstChildError.has_value()) {
                firstChildError = Error{std::format("sub-scheduler '{}' could not start: {}", subScheduler->uniqueName(), subSchedulerStartFailure(*schedulerModel))};
                this->emitErrorMessage("start()", *firstChildError);
            }
        }

        if (firstChildError.has_value()) {
            // a graph that could not start completely must not run degraded: a failed source never
            // publishes an end-of-stream, so the run would also never terminate on its own
            failStart(std::move(*firstChildError));
            return;
        }

        { // every job list of this run takes adopted blocks until its worker leaves its loop
            std::lock_guard guard(_adoptionBlocksMutex);
            _adoptionListClosed.assign(_adoptionBlocks.size(), false);
        }

        assert(_executionOrder != nullptr && !_executionOrder->empty());
        constexpr bool    singleThreaded = executionPolicy() == ExecutionPolicy::singleThreaded || executionPolicy() == ExecutionPolicy::singleThreadedBlocking;
        auto              jobListsCopy   = _executionOrder;
        const std::size_t nWorkers       = singleThreaded ? 1UZ : jobListsCopy->size();
        auto              ioThreadPool   = gr::thread_pool::Manager::defaultIoPool();

        stopWatchdogs();
        const std::size_t generation = _watchdogGeneration.value();

        // the whole generation is counted before the watchdog starts and before any worker is queued. waitDone() then
        // also covers the workers the pool has not started yet, and the watchdog finds the run's jobs counted
        std::ignore = _nRunningJobs->addAndGet(nWorkers);
        _nRunningJobs->notify_all();

        // keep outside of the lambda, as ~SchedulerBase() might finish before watchdog even starts
        gr::atomic_ref(_nWatchdogsRunning).fetch_add(1UZ);

        try {
            ioThreadPool->execute([this, generation] { this->runWatchDog(watchdog_timeout.value, timeout_inactivity_count.value, generation); });
        } catch (...) { // a rejected task would strand both counts and spin waitDone() and ~SchedulerBase() forever
            gr::atomic_ref(_nWatchdogsRunning).fetch_sub(1UZ);
            gr::atomic_ref(_nWatchdogsRunning).notify_all();
            std::ignore = _nRunningJobs->subAndGet(nWorkers);
            _nRunningJobs->notify_all();
            throw;
        }

        if constexpr (singleThreaded) {
            gr::atomic_ref(_nWorkersStarted).fetch_add(1UZ);
            dispatchWorker(0UZ, std::move(jobListsCopy), *workerGeneration);
        } else { // run on processing thread pool
            [[maybe_unused]] const auto pe = _profilerHandler->startCompleteEvent("scheduler_base.runOnPool");
            for (std::size_t runnerID = 0UZ; runnerID < nWorkers; runnerID++) {
                try {
                    _pool->execute([this, runnerID, jobListsCopy, workerGeneration = *workerGeneration]() {
                        if (gr::atomic_ref(_run.generation).load_acquire() != workerGeneration) { // a restart occurred before the pool reached this task
                            releaseWorkerCount(*_nRunningJobs);
                            return;
                        }
                        gr::atomic_ref(_nWorkersStarted).fetch_add(1UZ);
                        dispatchWorker(runnerID, jobListsCopy, workerGeneration);
                    });
                } catch (...) { // a rejected task never decrements, and the leaked count spins waitDone() forever
                    std::ignore = _nRunningJobs->subAndGet(nWorkers - runnerID);
                    _nRunningJobs->notify_all();
                    throw;
                }
            }
        }
        if constexpr (requires(Derived& d) { d.customStart(); }) {
            static_cast<Derived*>(this)->customStart();
        }
    }

    // A start requested on a worker of this scheduler, by a lifecycle message, records a restart and returns. A swap or
    // a restart already recorded takes the restart instead. A run dispatched inside the handler would block the
    // scheduler's messages under a single-threaded policy, and its start() would wait for the worker that runs it. The
    // restart keeps the generation it found. A start or a stop before the restart is applied cancels it.
    void deferStart() {
        std::lock_guard   guard(_runMutex);
        const std::size_t generation = _run.generation;
        if (_pendingExchange.has_value()) {
            _pendingExchange->restart    = true;
            _pendingExchange->generation = generation;
            _pendingExchange->requester  = std::this_thread::get_id();
        } else {
            _pendingExchange = PendingExchange{std::nullopt, profiling::Options{}, true, generation, std::this_thread::get_id()};
        }
    }

    // a scheduler's own poolWorker() takes the run's generation as a third parameter or omits it. A worker without the
    // generation ends only when it observes an inactive state
    void dispatchWorker(std::size_t runnerID, std::shared_ptr<JobLists> jobList, std::size_t generation) {
        if constexpr (requires(Derived& d, std::shared_ptr<JobLists> list) { d.poolWorker(0UZ, list, 0UZ); }) {
            static_cast<Derived*>(this)->poolWorker(runnerID, std::move(jobList), generation);
        } else {
            static_cast<Derived*>(this)->poolWorker(runnerID, std::move(jobList));
        }
    }

    // The single path by which a counted worker releases its count, whether it ran its job list or
    // was retired before starting. The pending graph exchange is claimed before the count is
    // released so that ~SchedulerBase()'s waitDone() cannot complete before the swap is applied.
    void releaseWorkerCount(gr::Sequence& nRunningJobs) {
        bool claimed = false;
        {
            std::lock_guard guard(_runMutex);
            if (_pendingExchange.has_value() && !_pendingExchange->claimed && _pendingExchange->requester == std::this_thread::get_id()) {
                _pendingExchange->claimed = true;
                claimed                   = true;
                gr::atomic_ref(_nDeferredExchanges).fetch_add(1UZ);
            }
        }
        std::ignore = nRunningJobs.subAndGet(1UZ);
        nRunningJobs.notify_all();
        if (claimed) {
            {
                const void*   previousApplying = std::exchange(applyingScheduler(), static_cast<const void*>(this));
                on_scope_exit restoreApplying  = [previousApplying] { applyingScheduler() = previousApplying; };
                applyPendingExchange();
            }
            gr::atomic_ref(_nDeferredExchanges).fetch_sub(1UZ);
            gr::atomic_ref(_nDeferredExchanges).notify_all();
        }
    }

    void poolWorker(const std::size_t runnerID, std::shared_ptr<std::vector<std::vector<std::shared_ptr<BlockModel>>>> jobList, const std::size_t generation) {
        using enum lifecycle::State;
        std::shared_ptr<gr::Sequence> progress     = _graph->_progress; // life-time guaranteed
        std::shared_ptr<gr::Sequence> nRunningJobs = _nRunningJobs;

        on_scope_exit decrementRunningJobs = [this, &nRunningJobs] { releaseWorkerCount(*nRunningJobs); }; // start() counted this worker in before queueing it

        // runs out before decrementRunningJobs, so applyPendingExchange() is not seen as on-worker
        const void*       previousActiveScheduler  = std::exchange(activeSchedulerWorker(), static_cast<const void*>(this));
        const std::size_t previousWorkerGeneration = std::exchange(activeWorkerGeneration(), generation);
        on_scope_exit     restoreActiveScheduler   = [previousActiveScheduler, previousWorkerGeneration] {
            activeSchedulerWorker()  = previousActiveScheduler;
            activeWorkerGeneration() = previousWorkerGeneration;
        };

        // counted under the lock that stop() takes to read the count. A worker that enters after stop() read zero sees
        // the scheduler shutting down and calls no work(). leaveLoop runs before decrementRunningJobs. Every blocking
        // block already in REQUESTED_STOP is therefore settled when waitDone() returns. No block's stop() hook runs
        // under this lock. A block's stop() hook that runs outside the scheduler's workers may therefore wait for them
        // to leave.
        {
            std::lock_guard workersLock(_workersInLoopMutex);
            ++_nWorkersInLoop;
        }
        on_scope_exit leaveLoop = [this] {
            std::lock_guard workersLock(_workersInLoopMutex);
            if (--_nWorkersInLoop == 0UZ) {
                removeWake();
                settleStoppedBlockingBlocks();
            }
        };
        on_scope_exit closeAdoption = [this, runnerID] { closeAdoptionList(runnerID); };

        gr::thread_pool::thread::setThreadName(std::format("pW{}-{}", runnerID, gr::meta::shorten_type_name(this->unique_name)));

        [[maybe_unused]] auto profiler_handler = _profiler.forThisThread();

        std::vector<std::shared_ptr<BlockModel>> localBlockList;
        {
            assert(jobList->size() > runnerID);
            std::lock_guard                          lock(_executionOrderMutex);
            std::vector<std::shared_ptr<BlockModel>> blocks = jobList->at(runnerID);
            localBlockList.reserve(blocks.size());
            std::ranges::copy(blocks, std::back_inserter(localBlockList));
        }

        {
            std::lock_guard workersLock(_workersInLoopMutex);
            registerWake(progress);
        }
        const gr::Sequence* previousWakeExempt = std::exchange(gr::detail::publishWakeExempt(), _wake.get());
        on_scope_exit       restoreWakeExempt  = [previousWakeExempt] { gr::detail::publishWakeExempt() = previousWakeExempt; };

        [[maybe_unused]] auto currentProgress    = this->_graph->progress().value();
        [[maybe_unused]] auto currentWake        = _wake->value();
        [[maybe_unused]] auto currentDraining    = gr::detail::drainingCalls();
        std::size_t           inactiveCycleCount = 0UZ;
        std::size_t           idleIterations     = 0UZ;
        std::size_t           msgToCount         = 0UZ;
        auto                  activeState        = this->state();
        do {
            [[maybe_unused]] auto pe = profiler_handler->startCompleteEvent("scheduler_base.work");
            if constexpr (parksIdleWorkers()) {
                // optionally tracking progress and block if there is none
                currentProgress = progress->value();
                currentWake     = _wake->value();
                currentDraining = gr::detail::drainingCalls();
            }

            // Process messages either when the ratio gate opens, or immediately when any entry-point port has
            // pending traffic. This keeps the ratio's amortisation of empty-queue checks while giving arriving
            // messages single-iteration latency (important for multi-hop sub-scheduler message paths).
            const bool hasPendingMessages   = this->msgIn.available() > 0UZ || _fromChildMessagePort.available() > 0UZ;
            const bool hasMessagesToProcess = msgToCount == 0UZ || hasPendingMessages;
            if (hasMessagesToProcess) {
                this->processScheduledMessages();
                // the run was retired. A restart among the messages above runs with the next run's workers, which own
                // the blocks from here
                if (gr::atomic_ref(_run.generation).load_acquire() != generation) {
                    break;
                }

                // Zombies are cleaned per-thread, as we remove from the localBlockList as well.
                // Cleaning zombies has low priority, so uses process_stream_to_message_ratio (a different ratio could be introduced)
                cleanupZombieBlocks(localBlockList);

                adoptBlocks(runnerID, localBlockList);

                const std::size_t nHandledBefore = gr::detail::handledMessageSpans();
                std::ranges::for_each(localBlockList, &BlockModel::processScheduledMessages);
                if (gr::detail::handledMessageSpans() != nHandledBefore) {
                    wakeWorkers(); // a reply a block wrote on this thread waits in the children's ring for the next pass
                }
                const auto previousState = activeState;
                activeState              = this->state();
                if (gr::atomic_ref(_run.generation).load_acquire() != generation) {
                    break; // the run was stopped, and a worker that saw no state since must not work the next run's blocks
                }
                if (hasPendingMessages || activeState != previousState) {
                    idleIterations = 0UZ;
                }
                msgToCount++;
            } else {
                if (std::has_single_bit(process_stream_to_message_ratio.value)) {
                    msgToCount = (msgToCount + 1U) & (process_stream_to_message_ratio.value - 1);
                } else {
                    msgToCount = (msgToCount + 1U) % process_stream_to_message_ratio.value;
                }
            }

            if (activeState == RUNNING) {
                if (gr::atomic_ref(_workQuiescenceRequested).load_acquire()) {
                    std::this_thread::yield();
                } else {
                    std::ignore = gr::atomic_ref(_nWorkersInWork).fetch_add_seq_cst(1UZ);
                    if (gr::atomic_ref(_workQuiescenceRequested).load_acquire()) {
                        gr::atomic_ref(_nWorkersInWork).fetch_sub(1UZ);
                    } else {
                        gr::work::Result result = traverseBlockListOnce(localBlockList);
                        gr::atomic_ref(_nWorkersInWork).fetch_sub(1UZ);
                        if (result.status == work::Status::DONE) {
                            break; // nothing happened -> shutdown this worker
                        } else if (result.status == work::Status::ERROR) {
                            failRun(runEndingBlock().value_or(""), generation, "returned ERROR");
                            break;
                        }
                        idleIterations = result.performed_work > 0UZ ? 0UZ : idleIterations + 1UZ;
                        applyIdleBackoff(idleIterations);
                        if (idleIterations > kIdleSpinIterations) {
                            msgToCount = 0UZ; // re-read lifecycle state every backoff period, bounding stop latency
                        }
                    }
                }
            } else {                                    // PAUSED or any other non-RUNNING state
                if (lifecycle::isActive(activeState)) { // a terminal state ends the loop below, do not sleep it out
                    sleepUntilStateChanges(activeState);
                }
                msgToCount = 0UZ;
            }

            // optionally tracking progress and block if there is none
            if constexpr (parksIdleWorkers()) {
                auto progressAfter = progress->value();
                if (currentProgress == progressAfter) {
                    inactiveCycleCount++;
                } else {
                    inactiveCycleCount = 0UZ;
                }

                currentProgress = progressAfter;
                // A block that drains an asynchronous input ends after a bound of calls in which nothing it waits on
                // moved. A parked worker would make one such call per park. The worker therefore does not park after
                // a pass in which it called a block that is draining an asynchronous input.
                const bool calledDrainingBlock = gr::detail::drainingCalls() != currentDraining;

                // parking in a non-RUNNING state would delay the worker's next state read by up to timeout_ms, and a
                // worker of a stopped run leaves instead. activeState can come from a message pass before a pause. The
                // worker therefore reads the state again. A pause publishes REQUESTED_PAUSE before its wake. A worker
                // that still reads RUNNING here read _wake before that wake, and the wake ends its park.
                if (activeState == RUNNING && inactiveCycleCount > timeout_inactivity_count && !calledDrainingBlock && gr::atomic_ref(_run.generation).load_acquire() == generation && this->state() == RUNNING) {
                    // allow a scheduler process to wait on progress before retrying (N.B. intended to save CPU/battery power)
                    // work, or a wake since the top of this pass, ends the park. A wake does not count as progress
                    waitUntilChanged(*progress, currentProgress, *_wake, currentWake, timeout_ms);
                    msgToCount = 0UZ;
                }
            }
        } while (lifecycle::isActive(activeState) && gr::atomic_ref(_run.generation).load_acquire() == generation);
    }

    // performs the graph swap or the restart that this thread claimed. The request is taken once every other worker has
    // left, with any start or swap that joined it
    void applyPendingExchange() {
        using enum lifecycle::State;

        waitDone(); // the claiming worker has already released its own count
        PendingExchange pending;
        {
            std::lock_guard guard(_runMutex);
            if (_run.destroying || !_pendingExchange.has_value()) {
                return; // destruction started and dropped the request: do not touch the graph
            }
            pending = std::move(*_pendingExchange);
            _pendingExchange.reset();
        }

        if (!pending.graph.has_value()) {
            applyDeferredStart(pending.generation);
            return;
        }
        if (this->state() == REQUESTED_STOP) {
            this->emitErrorMessageIfAny("applyPendingExchange() -> STOPPED", this->changeStateTo(STOPPED));
        }
        // A start in the same message batch as the swap leaves the scheduler active. exchange() then stops the run and
        // restores it itself. Under a single-threaded policy the restored run executes inside exchange() and has ended
        // when exchange() returns. The claim then restores no run of its own.
        const bool restoredByExchange = lifecycle::isActive(this->state());
        if (auto result = exchange(std::move(*pending.graph), pending.option); !result) {
            this->emitErrorMessage("applyPendingExchange()", result.error());
            return;
        }
        if (pending.restart && !restoredByExchange && !lifecycle::isActive(this->state())) {
            this->emitErrorMessageIfAny("applyPendingExchange() -> RUNNING", restoreRun(pending.generation, RUNNING));
        }
    }

    // The message that requested the start has published RUNNING. An unchanged generation runs the start. A changed one
    // means a stop came since, and the scheduler stops in place of the start.
    void applyDeferredStart(std::size_t generation) {
        using enum lifecycle::State;
        if (runGeneration() == generation && this->state() == RUNNING) {
            try {
                start();
            } catch (const std::exception& e) {
                this->emitErrorMessage("applyDeferredStart()", Error(std::format("start() throws: {}", e.what())));
                this->emitErrorMessageIfAny("applyDeferredStart() -> ERROR", this->changeStateTo(ERROR));
            }
            return;
        }
        if (lifecycle::isActive(this->state())) {
            this->emitErrorMessageIfAny("applyDeferredStart() -> REQUESTED_STOP", this->changeStateTo(REQUESTED_STOP));
        }
        std::lock_guard guard(_runMutex);
        if (_run.phase == RunPhase::starting) {
            _run.phase = RunPhase::started;
        }
    }

    // polls the state every millisecond until it changes or timeout_ms passes. Only a worker outside RUNNING calls it.
    void sleepUntilStateChanges(lifecycle::State observedState) {
        constexpr auto kChunk   = std::chrono::milliseconds(1);
        const auto     deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms.value);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(kChunk);
            if (this->state() != observedState) {
                return;
            }
        }
    }

    void runWatchDog(std::size_t timeOut_ms, std::size_t timeOut_count, std::size_t generation) {
        on_scope_exit _ = [this] {
            gr::atomic_ref(_nWatchdogsRunning).fetch_sub(1UZ);
            gr::atomic_ref(_nWatchdogsRunning).notify_all();
        };

        auto thisName = gr::meta::shorten_type_name(this->unique_name);
        gr::thread_pool::thread::setThreadName(std::format("WatchDog-{}", thisName));

        // start() counts the run's jobs before it spawns the watchdog. A count of zero means the run has ended.
        if (_watchdogGeneration.value() != generation || _nRunningJobs->value() == 0UZ || !lifecycle::isActive(this->state())) {
            return; // abort watchdog: retired, scheduler inactive, or jobs already finished.
        }

        std::size_t lastProgress  = _graph->_progress->value();
        std::size_t nWarnings     = 0;
        bool        stallReported = false; // one report per stall; progress or a state other than RUNNING re-arms it
        do {
            // the wait ends early only when the watchdog is retired
            if (_watchdogGeneration.waitUntil(generation, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeOut_ms))) {
                return;
            }
            // a period without progress wakes the parked workers, which then call every block again. The wake follows the
            // state read and leaves the progress sequence unchanged.
            std::size_t currentProgress = _graph->_progress->value();
            if ((_nRunningJobs->value() > 0UZ) && (currentProgress == lastProgress)) {
                if (this->state() != lifecycle::State::RUNNING) { // only a RUNNING graph is expected to make progress
                    nWarnings     = 0UZ;
                    stallReported = false;
                } else if (++nWarnings >= timeOut_count && !stallReported) {
                    stallReported = true;
                    emitStallReport(nWarnings, timeOut_ms);
                }
                wakeWorkers();
            } else {
                lastProgress  = currentProgress;
                nWarnings     = 0UZ;
                stallReported = false;
            }
        } while (_nRunningJobs->value() > 0UZ);
    }

    // The report is a notification, not an error: a parent scheduler turns an error from a child into an exception
    // when nothing reads its msgOut. The watchdog thread publishes while the workers do.
    void emitStallReport(std::size_t nPeriods, std::size_t periodMs) {
        Message message;
        message.cmd         = message::Command::Notify;
        message.serviceName = this->unique_name;
        message.endpoint    = "watchdog";
        message.data        = property_map{{"stalled_periods", static_cast<gr::Size_t>(nPeriods)}, {"period_ms", static_cast<gr::Size_t>(periodMs)}};
        publishOnOwnWriter(std::move(message));
    }

    // publishes one message on msgOut through a writer of its own on the multi-producer ring. The port's own writer
    // belongs to the caller that holds the message service's flag. A message that finds msgOut full is dropped and
    // counted.
    void publishOnOwnWriter(Message message) {
        auto                writer = this->msgOut.buffer().streamBuffer.new_writer();
        WriterSpanLike auto span   = writer.template tryReserve<SpanReleasePolicy::ProcessAll>(1UZ);
        if (span.empty()) {
            message::droppedMessageCount().fetch_add(1UZ, std::memory_order_relaxed);
            return;
        }
        span[0] = std::move(message);
        span.publish(1UZ);
    }

    // A parked worker waits until the graph's progress sequence or _wake leaves the value it read at the top of its
    // pass, or until timeout_ms passes. Work advances progress, which the inactivity count and the watchdog read.
    // Everything else that must end a park advances _wake and notifies the progress sequence: stop(), pause(),
    // resume(), the watchdog's period, a message from another thread (see registerWake()), a worker's message pass in
    // which a block handled a message, and an edit or a forward (see wakeParkedWorkers()).
    void wakeWorkers() {
        _wake->incrementAndGet();
        _graph->_progress->notify_all();
    }

    // The scheduler's message service runs on any thread, and a parent's worker runs a nested scheduler's. The service
    // can forward a message to a block, or add or remove a block or an edge, while the worker that calls the block
    // concerned is parked. A worker woken while a work quiescence holds parks again without calling its blocks. The
    // end of every work quiescence, an edit's or a parent's, therefore wakes the workers.
    void wakeParkedWorkers() {
        if constexpr (parksIdleWorkers()) {
            wakeWorkers();
        }
    }

    // While a worker of a parking policy is inside poolWorker(), each message published into msgIn or into the ring
    // the children send on advances _wake and notifies the progress sequence the worker parks on. A parked worker
    // handles the message without waiting out timeout_ms, and the graph's progress, which the watchdog reads, stays
    // unchanged. A message published on the worker's own thread leaves _wake alone and waits for the next message
    // pass. A reply that a block writes there is forwarded on that pass without a park between, since the pass in which
    // the block handled the request advances _wake. A message published while no worker is inside stays in its ring
    // until the first message pass of the next run. A worker that finds no registration registers, and the last worker
    // out removes the registration, so each run registers its own graph's progress sequence. Called under
    // _workersInLoopMutex.
    void registerWake(const std::shared_ptr<gr::Sequence>& progress) {
        if constexpr (parksIdleWorkers()) {
            if (!_wakeRings.empty()) {
                return;
            }
            _wakeRings.reserve(2UZ);
            for (MsgPortIn::BufferType ring : {this->msgIn.buffer().streamBuffer, _fromChildMessagePort.buffer().streamBuffer}) {
                ring.addWakeSequence(_wake, progress);
                _wakeRings.push_back(std::move(ring));
            }
        }
    }

    // called under _workersInLoopMutex by the last worker to leave poolWorker()
    void removeWake() {
        for (MsgPortIn::BufferType& ring : _wakeRings) {
            ring.removeWakeSequence(_wake);
        }
        _wakeRings.clear();
    }

    // a blocking block leaves REQUESTED_STOP in its own next work() call. With no worker inside poolWorker(), no such
    // call follows, and the scheduler moves the block to STOPPED itself. That transition runs no stop() hook. The block
    // is settled by whichever comes second: its REQUESTED_STOP in stop()'s sweep, or the last worker leaving
    // poolWorker(). The sweep settles a block it reaches after the last worker left. That worker can leave before the
    // sweep reaches the block, since stop() retires the workers before it sweeps. The last worker out settles every
    // block the sweep reached before. Both read _nWorkersInLoop under _workersInLoopMutex. Called by the last worker
    // under that mutex
    void settleStoppedBlockingBlocks() {
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) {
            if (block->blockCategory() != ScheduledBlockGroup && block->isBlocking() && block->state() == lifecycle::State::REQUESTED_STOP) {
                this->emitErrorMessageIfAny("settleStoppedBlockingBlocks() -> LifecycleState", block->changeStateTo(lifecycle::State::STOPPED));
            }
        });
    }

    // asks an active child to stop, then settles a child that reads REQUESTED_STOP. A non-blocking child moves to
    // STOPPED at once. A blocking child moves to STOPPED here only while no worker is inside poolWorker(). Otherwise its
    // own next work() call or the last worker to leave settles it. A child that never started, or that has stopped or
    // failed already, keeps its state, and its stop() hook does not run.
    void stopChild(BlockModel& block, std::string_view caller) {
        using enum lifecycle::State;
        if (lifecycle::isActive(block.state())) {
            this->emitErrorMessageIfAny(caller, block.changeStateTo(REQUESTED_STOP));
        }
        if (block.state() != REQUESTED_STOP) {
            return;
        }
        if (!block.isBlocking()) {
            this->emitErrorMessageIfAny(caller, block.changeStateTo(STOPPED));
            return;
        }
        std::lock_guard workersLock(_workersInLoopMutex);
        if (_nWorkersInLoop == 0UZ && block.state() == REQUESTED_STOP) {
            this->emitErrorMessageIfAny(caller, block.changeStateTo(STOPPED));
        }
    }

    void stop() {
        using enum lifecycle::State;
        // retires the run's workers. A queued worker releases its count without running. A worker in its loop leaves
        // at its next check, even one that misses the stop because the next start() has already set RUNNING.
        advanceRunGeneration();
        wakeWorkers();
        {
            std::lock_guard childLock(_childLifecycleMutex); // serialized against start()'s sweep
            graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) {
                if (block->blockCategory() == ScheduledBlockGroup) {
                    auto* schedulerModel = dynamic_cast<SchedulerModel*>(block.get());
                    if (schedulerModel) {
                        schedulerModel->stop();
                    } else {
                        throw gr::exception(std::format("ScheduledBlockGroup is not a SchedulerModel {}", block->uniqueName()));
                    }
                } else {
                    stopChild(*block, "forEachBlock -> stop() -> LifecycleState");
                }
            });
        }

        if (this->state() != ERROR) { // stop() also runs on the way into ERROR, which only reset() leaves
            this->emitErrorMessageIfAny("stop() -> LifecycleState ->STOPPED", this->changeStateTo(STOPPED));
        }
        if constexpr (requires(Derived& d) { d.customStop(); }) {
            static_cast<Derived*>(this)->customStop();
        }
    }

    void pause() {
        using enum lifecycle::State;
        wakeWorkers();
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) {
            this->emitErrorMessageIfAny("pause() -> LifecycleState", block->changeStateTo(REQUESTED_PAUSE));
            if (!block->isBlocking()) { // N.B. no other thread/constraint to consider before shutting down
                this->emitErrorMessageIfAny("pause() -> LifecycleState", block->changeStateTo(PAUSED));
            }
        });
        this->emitErrorMessageIfAny("pause() -> LifecycleState", this->changeStateTo(PAUSED));
        if constexpr (requires(Derived& d) { d.customPause(); }) {
            static_cast<Derived*>(this)->customPause();
        }
    }

    void resume() {
        using enum lifecycle::State;
        wakeWorkers();
        std::vector<Edge> unconnected;
        {
            WorkQuiescenceGuard quiescence(this);
            unconnected = connectPendingEdges();
        }
        if (!unconnected.empty()) {
            Error reason = unconnectedEdgesError(unconnected);
            this->emitErrorMessage("resume()", reason);
            failStart(std::move(reason));
            return;
        }
        graph::forEachBlock<TransparentBlockGroup>(*_graph, [this](auto& block) { this->emitErrorMessageIfAny("resume() -> LifecycleState", block->changeStateTo(RUNNING)); });
        wakeWorkers(); // a worker that parked before the blocks reached RUNNING calls them now
        if constexpr (requires(Derived& d) { d.customResume(); }) {
            static_cast<Derived*>(this)->customResume();
        }
    }

    enum class SubSchedulerStart { workerRunning, failed, stopped, noWorkerInTime };

    // waits at most watchdog_timeout for a sub-scheduler's start to settle. A worker counts itself before its run can end
    // in ERROR, so ERROR with no worker counted is a failed start. A stop that claims the start leaves no worker and no
    // ERROR
    [[nodiscard]] SubSchedulerStart awaitSubSchedulerStart(const BlockModel& block, SchedulerModel& schedulerModel) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(watchdog_timeout.value);
        while (true) {
            const lifecycle::State state = block.state();
            if (schedulerModel.workerStarted()) {
                return SubSchedulerStart::workerRunning;
            }
            if (state == lifecycle::State::ERROR) {
                return SubSchedulerStart::failed;
            }
            if (lifecycle::isShuttingDown(state)) {
                return SubSchedulerStart::stopped;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return SubSchedulerStart::noWorkerInTime;
            }
            std::this_thread::yield();
        }
    }

    [[nodiscard]] static std::string subSchedulerStartFailure(const SchedulerModel& schedulerModel) {
        const std::optional<Error> reason = schedulerModel.startError();
        return reason.has_value() ? reason->message : std::string("its start ended in ERROR");
    }

    // a sub-scheduler's RUNNING transition runs its whole loop, so it starts through the threaded wrapper rather than
    // on the thread that adopts it, and only an executing worker shows that its pool had a thread for it. A start that
    // cannot complete ends the sub-scheduler in ERROR without a worker, and the report carries its startError().
    void startAdoptedScheduler(const std::shared_ptr<BlockModel>& newBlock) {
        using enum lifecycle::State;
        auto* schedulerModel = dynamic_cast<SchedulerModel*>(newBlock.get());
        if (schedulerModel == nullptr) {
            this->emitErrorMessage("adoptBlock", std::format("ScheduledBlockGroup is not a SchedulerModel {}", newBlock->uniqueName()));
            return;
        }
        if (auto started = schedulerModel->startAdopted(); !started.has_value()) {
            this->emitErrorMessageIfAny("adoptBlock", started);
            return;
        }

        switch (awaitSubSchedulerStart(*newBlock, *schedulerModel)) {
        case SubSchedulerStart::failed: this->emitErrorMessage("adoptBlock", std::format("adopted sub-scheduler '{}' could not start: {}", newBlock->uniqueName(), subSchedulerStartFailure(*schedulerModel))); break;
        case SubSchedulerStart::noWorkerInTime: this->emitErrorMessage("adoptBlock", std::format("no worker of adopted sub-scheduler '{}' began executing", newBlock->uniqueName())); break;
        case SubSchedulerStart::workerRunning:
        case SubSchedulerStart::stopped: break;
        }
    }

    [[nodiscard]] bool hasOpenAdoptionList() {
        std::lock_guard guard(_adoptionBlocksMutex);
        for (std::size_t i = 0UZ; i < _adoptionBlocks.size(); ++i) {
            if (i >= _adoptionListClosed.size() || !_adoptionListClosed[i]) {
                return true;
            }
        }
        return false;
    }

    // queues the block for a job list whose worker remains. Returns false when every worker has left
    [[nodiscard]] bool queueForAdoption(const std::shared_ptr<BlockModel>& newBlock) {
        std::lock_guard          guard(_adoptionBlocksMutex);
        std::vector<std::size_t> openLists;
        for (std::size_t i = 0UZ; i < _adoptionBlocks.size(); ++i) {
            if (i >= _adoptionListClosed.size() || !_adoptionListClosed[i]) {
                openLists.push_back(i);
            }
        }
        if (openLists.empty()) {
            return false;
        }
        _adoptionBlocks[openLists[std::hash<BlockModel*>{}(newBlock.get()) % openLists.size()]].push_back(newBlock);
        return true;
    }

    void adoptBlock(const std::shared_ptr<BlockModel>& newBlock) {
        using enum lifecycle::State;
        if (const auto connectResult = _toChildMessagePort.connect(*newBlock->msgIn); !connectResult.has_value()) {
            this->emitErrorMessage("connectBlockMessagePorts()", std::format("Failed to connect scheduler input message port to child '{}'", newBlock->uniqueName()));
        }
        auto toSchedulerBuffer = _fromChildMessagePort.buffer();
        newBlock->msgOut->setBuffer(toSchedulerBuffer.streamBuffer, toSchedulerBuffer.tagBuffer);

        if (!lifecycle::isActive(this->state())) {
            return;
        }

        if (newBlock->blockCategory() == ScheduledBlockGroup) {
            // the scheduler starts an added scheduler and then queues it. The worker that takes it calls into it, and its
            // init() runs inside the start
            if (hasOpenAdoptionList()) {
                startAdoptedScheduler(newBlock);
                if (queueForAdoption(newBlock)) {
                    wakeParkedWorkers();
                }
            }
            return;
        }
        if (!queueForAdoption(newBlock)) {
            return;
        }

        switch (newBlock->state()) {
        case STOPPED:
        case IDLE: //
            this->emitErrorMessageIfAny("adoptBlock -> INITIALIZED", newBlock->changeStateTo(INITIALISED));
            this->emitErrorMessageIfAny("adoptBlock -> INITIALIZED", newBlock->changeStateTo(RUNNING));
            break;
        case INITIALISED: //
            this->emitErrorMessageIfAny("adoptBlock -> INITIALIZED", newBlock->changeStateTo(RUNNING));
            break;
        default: this->emitErrorMessage("propertyCallbackEmplaceBlock", std::format("Unexpected block state during emplacement: {}", gr::meta::enumName(newBlock->state()).value_or("")));
        }
        wakeParkedWorkers();
    }

    std::optional<Message> propertyCallbackEmplaceBlock([[maybe_unused]] std::string_view propertyName, Message message) {
        using enum lifecycle::State;
        assert(propertyName == scheduler::property::kEmplaceBlock);
        using namespace std::string_literals;
        const auto& messageData = message.data.value();

        message.endpoint = scheduler::property::kBlockEmplaced;

        auto* targetGraph = findTargetSubGraph(messageData);
        if (targetGraph == nullptr) {
            message.data = std::unexpected(Error{std::format("No target graph for the message {}", message)});
            return message;
        }

        std::string  blockType;
        property_map blockProperties;

        if (auto yamlIt = messageData.find("yaml"); yamlIt != messageData.end()) {
            // YAML path: create block from a serialised block definition string
            const auto yamlStr = yamlIt->second.value_or(std::string_view{});
            if (yamlStr.empty()) {
                message.data = std::unexpected(Error{"yaml field is empty"s});
                return message;
            }
            auto parsed = pmt::yaml::deserialize(yamlStr);
            if (!parsed) {
                message.data = std::unexpected(Error{std::format("Could not parse yaml: {}", parsed.error().message)});
                return message;
            }

            if (auto idIt = parsed->find("id"); idIt != parsed->end()) {
                blockType = std::string(idIt->second.value_or(std::string_view{}));
            }
            if (blockType.empty()) {
                message.data = std::unexpected(Error{"yaml block definition is missing id field"s});
                return message;
            }

            if (blockType == "SUBGRAPH") {
                // Wrap the single block definition so loadGraphFromMap can process it
                property_map       graphMap;
                Tensor<pmt::Value> blocksSeq;
                blocksSeq.push_back(pmt::Value(*parsed));
                graphMap["blocks"] = std::move(blocksSeq);

                const std::size_t blocksBefore = targetGraph->blocks().size();
                try {
                    detail::loadGraphFromMap(gr::globalPluginLoader(), *targetGraph, std::move(graphMap));
                } catch (const std::exception& e) {
                    message.data = std::unexpected(Error{std::format("Failed to create subgraph from yaml: {}", e.what())});
                    return message;
                }

                const auto& blocks = targetGraph->blocks();
                if (blocks.size() <= blocksBefore) {
                    message.data = std::unexpected(Error{"No block was added from yaml"s});
                    return message;
                }

                for (std::size_t i = blocksBefore; i < blocks.size(); ++i) {
                    adoptBlock(blocks[i]);
                }

                auto replyData            = serializeBlock(gr::globalPluginLoader(), blocks[blocksBefore], BlockSerializationFlags::All);
                replyData["_targetGraph"] = targetGraph->unique_name.value();
                this->emitMessage(scheduler::property::kBlockEmplaced, std::move(replyData));
                return {};
            }

            // Normal block from YAML: read parameters, stripping auto-generated system fields
            if (auto it = parsed->find("parameters"); it != parsed->end()) {
                if (const auto* p = it->second.get_if<property_map>()) {
                    blockProperties = *p;
                    blockProperties.erase("unique_name"); // auto-generated, not user-settable
                }
            }
        } else {
            // Non-YAML path: read type and properties directly from the message
            blockType = std::string(messageData.at("type").value_or(std::string_view{}));
            if (blockType.empty()) {
                message.data = std::unexpected(Error{std::format("No type specified for the message {}", message)});
                return message;
            }
            if (auto it = messageData.find("properties"); it != messageData.end()) {
                if (const auto* result = it->second.get_if<property_map>()) {
                    blockProperties = *result;
                }
            }
        }

        // For the YAML path, settings from the serialised block definition are applied
        // via loadParametersFromPropertyMap after emplacement
        const bool   isYamlPath   = messageData.contains("yaml");
        property_map yamlSettings = isYamlPath ? std::exchange(blockProperties, {}) : property_map{};

        const std::shared_ptr<BlockModel>& newBlock = [&]() -> const std::shared_ptr<BlockModel>& {
            WorkQuiescenceGuard quiescence(this); // _blocks is traversed by every worker and by forEachBlock
            return targetGraph->emplaceBlock(blockType, blockProperties);
        }();

        if (isYamlPath && !yamlSettings.empty()) {
            newBlock->settings().loadParametersFromPropertyMap(yamlSettings);
            // loadParametersFromPropertyMap() only stores; without activating the context nothing is ever
            // staged and the block keeps its constructor defaults (Graph_yaml_importer does this itself)
            if (newBlock->settings().activateContext() == std::nullopt) {
                this->emitErrorMessage("propertyCallbackEmplaceBlock", std::format("could not activate the loaded settings context of '{}'", newBlock->uniqueName()));
            }
            // and applying them is what makes the block report them: it is not connected or adopted yet, so
            // this is the same point in its life at which Graph::addBlock() applies a constructor's settings
            std::ignore = newBlock->settings().applyStagedParameters();
        }

        adoptBlock(newBlock);

        auto replyData            = serializeBlock(gr::globalPluginLoader(), newBlock, BlockSerializationFlags::All);
        replyData["_targetGraph"] = targetGraph->unique_name.value();
        this->emitMessage(scheduler::property::kBlockEmplaced, std::move(replyData));

        // Message is sent as a reaction to emplaceBlock, no need for a separate one
        return {};
    }

    std::optional<Message> propertyCallbackRemoveBlock([[maybe_unused]] std::string_view propertyName, Message message) {
        assert(propertyName == scheduler::property::kRemoveBlock);
        using namespace std::string_literals;
        auto&      messageData = message.data.value();
        const auto uniqueName  = messageData.at("uniqueName").value_or(std::string_view{});
        if (uniqueName.empty()) {
            message.data = std::unexpected(Error{std::format("No uniqueName in the message {}", message)});
            return message;
        }

        message.endpoint = scheduler::property::kBlockRemoved;

        auto* targetGraph = findTargetSubGraph(messageData);

        if (targetGraph == nullptr) {
            message.data = std::unexpected(Error{std::format("No target graph for the message {}", message)});
            return message;
        }

        messageData["_targetGraph"] = targetGraph->unique_name.value();
        {
            WorkQuiescenceGuard quiescence(this); // _blocks is traversed by every worker and by forEachBlock
            if (auto removedBlock = targetGraph->removeBlockByName(uniqueName); removedBlock.has_value()) {
                makeZombie(std::move(*removedBlock));
            } else {
                message.data = std::unexpected(removedBlock.error());
            }
        }

        return {message};
    }

    std::optional<Message> propertyCallbackRemoveEdge([[maybe_unused]] std::string_view propertyName, Message message) {
        assert(propertyName == scheduler::property::kRemoveEdge);
        using namespace std::string_literals;
        auto&      messageData = message.data.value();
        const auto sourceBlock = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_SOURCE_BLOCK)).value_or(std::string_view{});
        const auto sourcePort  = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_SOURCE_PORT)).value_or(std::string_view{});
        if (sourceBlock.empty() || sourcePort.empty()) {
            message.data = std::unexpected(Error{std::format("No source definition for the message {}", message)});
            return message;
        }

        message.endpoint = scheduler::property::kEdgeRemoved;

        auto* targetGraph = findTargetSubGraph(messageData);

        if (targetGraph == nullptr) {
            message.data = std::unexpected(Error{std::format("No target graph for the message {}", message)});
            return message;
        }

        // optional: restrict the removal to a single edge of a fan-out
        const auto destinationBlock = messageData.contains(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_BLOCK)) ? messageData.at(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_BLOCK)).value_or(std::string_view{}) : std::string_view{};
        const auto destinationPort  = messageData.contains(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_PORT)) ? messageData.at(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_PORT)).value_or(std::string_view{}) : std::string_view{};

        messageData["_targetGraph"] = targetGraph->unique_name.value();
        {
            WorkQuiescenceGuard quiescence(this);
            if (auto result = targetGraph->removeEdgeBySourcePort(sourceBlock, sourcePort, destinationBlock, destinationPort); result.has_value()) {
                messageData["nEdgesRemoved"] = static_cast<gr::Size_t>(*result);
            } else {
                message.data = std::unexpected(result.error());
            }
        }

        return message;
    }

    // the EdgeEmplaced reply lists the edges the new one displaced under "displacedEdges", keyed by index as in a
    // GraphInspect reply
    std::optional<Message> propertyCallbackEmplaceEdge([[maybe_unused]] std::string_view propertyName, Message message) {
        assert(propertyName == scheduler::property::kEmplaceEdge);
        using namespace std::string_literals;
        auto&      messageData      = message.data.value();
        const auto sourceBlock      = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_SOURCE_BLOCK)).value_or(std::string_view{});
        const auto sourcePort       = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_SOURCE_PORT)).value_or(std::string_view{});
        const auto destinationBlock = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_BLOCK)).value_or(std::string_view{});
        const auto destinationPort  = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_DESTINATION_PORT)).value_or(std::string_view{});
        // checked_access_ptr terminates on a null unless not_null is turned off, so the
        // non-terminating form is what keeps the incompleteness report below reachable: a message
        // whose buffer size or weight is of the wrong type is a sender's input and is refused as one
        [[maybe_unused]] const auto minBufferSize = checked_access_ptr<gr::Size_t, false>{messageData.at(std::pmr::string(gr::serialization_fields::EDGE_MIN_BUFFER_SIZE)).get_if<gr::Size_t>()};
        [[maybe_unused]] const auto weight        = checked_access_ptr<std::int32_t, false>{messageData.at(std::pmr::string(gr::serialization_fields::EDGE_WEIGHT)).get_if<std::int32_t>()};
        const auto                  edgeName      = messageData.at(std::pmr::string(gr::serialization_fields::EDGE_NAME)).value_or(std::string_view{});

        if (sourceBlock.empty() || sourcePort.empty() || destinationBlock.empty() || destinationPort.empty() || minBufferSize == nullptr || weight == nullptr || edgeName.empty()) {
            message.data = std::unexpected(Error{std::format("Message is incomplete {}", message)});
            return message;
        }

        message.endpoint = scheduler::property::kEdgeEmplaced;

        auto* targetGraph = findTargetSubGraph(messageData);

        if (targetGraph == nullptr) {
            message.data = std::unexpected(Error{std::format("No target graph for the message {}", message)});
            return message;
        }

        messageData["_targetGraph"] = targetGraph->unique_name.value();
        {
            WorkQuiescenceGuard quiescence(this);
            const std::size_t   effectiveMinBufferSize = (*minBufferSize == gr::undefined_Size) ? gr::undefined_size : static_cast<std::size_t>(*minBufferSize);
            if (auto result = targetGraph->emplaceEdge(sourceBlock, std::string(sourcePort), destinationBlock, std::string(destinationPort), effectiveMinBufferSize, *weight, edgeName); result.has_value()) {
                property_map displacedEdges;
                for (std::size_t index = 0UZ; index < result->size(); ++index) {
                    displacedEdges[convert_string_domain(std::to_string(index))] = serializeEdge((*result)[index]);
                }
                messageData["displacedEdges"] = std::move(displacedEdges);
            } else {
                message.data = std::unexpected(result.error());
            }
        }

        return message;
    }

    /*
      Zombie Tutorial:

      Blocks cannot be deleted unless stopped, but stopping may take time (asynchronous).
      We therefore move such blocks to the "zombie list" and disconnect them immediately from the graph,
      allowing them to stop and be deleted safely.

      Each worker thread periodically calls cleanupZombieBlocks(), which:
      - removes fully stopped zombies from the zombie list
      - erases corresponding entries from its own localBlockList
      - updates the shared _executionOrder to ensure zombies do not reappear on restart

      This mechanism supports safe dynamic block removal while the scheduler is running, without blocking execution.
    */
    void cleanupZombieBlocks(std::vector<std::shared_ptr<BlockModel>>& localBlockList) {
        using enum lifecycle::State;
        if (localBlockList.empty()) {
            return;
        }

        std::lock_guard guard(_zombieBlocksMutex);

        auto it = _zombieBlocks.begin();

        while (it != _zombieBlocks.end()) {
            const auto localBlockIt = std::ranges::find(localBlockList, *it);
            if (localBlockIt == localBlockList.end()) {
                // we only care about the blocks local to our thread.
                ++it;
                continue;
            }

            bool shouldDelete = false;

            switch ((*it)->state()) {
            case IDLE:
            case STOPPED:
            case INITIALISED: // block can be deleted immediately
                shouldDelete = true;
                break;
            case ERROR: // delete as well
                shouldDelete = true;
                break;
            case REQUESTED_STOP: // block will be deleted later
                break;
            case REQUESTED_PAUSE: // zombie that never reached PAUSED -- stop it directly
                stopChild(**it, "cleanupZombieBlocks");
                break;
            case PAUSED: // zombie was in REQUESTED_PAUSE and now finally in PAUSED. Can be stopped now.
                // Will be deleted in a next zombie maintenance period
                stopChild(**it, "cleanupZombieBlocks");
                break;
            case RUNNING: assert(false && "Doesn't happen: zombie blocks are never running"); break;
            }

            if (shouldDelete) {
                localBlockList.erase(localBlockIt);

                std::shared_ptr<BlockModel> zombieRaw = *it;
                it                                    = _zombieBlocks.erase(it); // ~Block() runs here

                // We need to remove zombieRaw from jobLists as well, in case Scheduler ever goes to INITIALIZED again.
                std::lock_guard lock(_executionOrderMutex);
                for (auto& jobList : *this->_executionOrder) {
                    auto job_it = std::remove(jobList.begin(), jobList.end(), zombieRaw);
                    if (job_it != jobList.end()) {
                        jobList.erase(job_it, jobList.end());
                        break;
                    }
                }

            } else {
                ++it;
            }
        }
    }

    // a worker that leaves its loop hands the blocks still queued for its job list to a job list whose worker remains
    void closeAdoptionList(std::size_t runnerID) {
        std::lock_guard guard(_adoptionBlocksMutex);
        if (runnerID >= _adoptionListClosed.size()) {
            return;
        }
        _adoptionListClosed[runnerID] = true;
        if (runnerID >= _adoptionBlocks.size()) {
            return;
        }
        for (std::size_t i = 0UZ; i < _adoptionBlocks.size() && !_adoptionBlocks[runnerID].empty(); ++i) {
            if (i >= _adoptionListClosed.size() || !_adoptionListClosed[i]) {
                std::ranges::move(_adoptionBlocks[runnerID], std::back_inserter(_adoptionBlocks[i]));
                _adoptionBlocks[runnerID].clear();
            }
        }
    }

    void adoptBlocks(std::size_t runnerID, std::vector<std::shared_ptr<BlockModel>>& localBlockList) {
        std::lock_guard guard(_adoptionBlocksMutex);

        if (runnerID >= _adoptionBlocks.size()) {
            return; // scheduler was reinitialized with fewer batches; this runner has no pending blocks
        }
        auto& newBlocks = _adoptionBlocks[runnerID];

        localBlockList.reserve(localBlockList.size() + newBlocks.size());
        localBlockList.insert(localBlockList.end(), newBlocks.begin(), newBlocks.end());
        newBlocks.clear();
    }

    /*
      Moves a block to the zombie list:

      - Stops the block through stopChild() if it is still running or paused.
      - Removes the block from adoption lists (to handle edge cases such as Add Block → Remove Block).
      - Adds it to the zombie list.

      The block will be physically deleted by cleanupZombieBlocks() when it reaches a safe state.
    */
    void makeZombie(std::shared_ptr<BlockModel> block) {
        stopChild(*block, "makeZombie");

        {
            // Handle edge case: If we receive two consecutive "Add Block X" "Remove Block X" messages
            // it would be zombie before being adopted, so we need to remove it from adoption list
            std::lock_guard guard(_adoptionBlocksMutex);
            for (std::vector<std::shared_ptr<BlockModel>>& adoptionList : _adoptionBlocks) {
                if (auto it = std::ranges::find(adoptionList, block); it != adoptionList.end()) {
                    adoptionList.erase(it);
                    break;
                }
            }
        }

        std::lock_guard guard(_zombieBlocksMutex);
        _zombieBlocks.push_back(std::move(block));
    }

    // Moves all blocks into the zombie list
    // Useful for bulk operations such as "set grc yaml" message
    void makeAllZombies() {
        std::lock_guard guard(_zombieBlocksMutex);

        for (auto& block : this->_graph->blocks()) {
            stopChild(*block, "makeAllZombies");
            _zombieBlocks.push_back(std::move(block));
        }

        this->_graph->clear();
    }

    std::optional<Message> propertyCallbackGraphGRC([[maybe_unused]] std::string_view propertyName, Message message) {
        using enum lifecycle::State;
        assert(propertyName == scheduler::property::kGraphGRC);

        auto& pluginLoader = gr::globalPluginLoader();
        if (message.cmd == message::Command::Get) {
            message.data = property_map{{"value", gr::saveGrc(pluginLoader, *_graph)}};
        } else if (message.cmd == message::Command::Set) {
            const auto& messageData = message.data.value();
            auto        yamlContent = messageData.at("value").value_or(std::string_view{});
            if (yamlContent.empty()) {
                message.data = std::unexpected(Error{std::format("Yaml content not found")});
            } else {
                try {
                    auto newGraph = gr::loadGrc(pluginLoader, yamlContent);

                    if (auto allowed = swapAllowedFromThisThread(); !allowed) { // before the current blocks are retired
                        message.data = std::unexpected(allowed.error());
                        return message;
                    }
                    makeAllZombies();

                    const auto originalState = this->state();

                    if (auto result = this->exchange(std::move(newGraph)); !result) {
                        this->emitErrorMessage("propertyCallbackGraphGRC", "Failed to exchange graph");
                        return {};
                    }

                    message.data = property_map{{"originalSchedulerState", static_cast<int>(originalState)}};
                } catch (const std::exception& e) {
                    message.data = std::unexpected(Error{std::format("Error parsing YAML: {}", e.what())});
                }
            }

        } else {
            throw gr::exception(std::format("Unexpected command type {}", message.cmd));
        }

        return message;
    }

    std::optional<Message> propertyCallbackSchedulerInspect([[maybe_unused]] std::string_view propertyName, Message message) {
        assert(propertyName == scheduler::property::kSchedulerInspect);

        if (const bool yamlSerialize =
                [&] {
                    if (!message.data) {
                        return false;
                    }
                    if (const auto it = message.data->find("serialization_format"); it != message.data->cend()) {
                        return it->second == "yaml";
                    }
                    return false;
                }();
            !yamlSerialize) {
            message.data = [&] {
                property_map result;
                result[std::pmr::string(serialization_fields::BLOCK_NAME)]        = std::string(this->name);
                result[std::pmr::string(serialization_fields::BLOCK_UNIQUE_NAME)] = std::string(this->unique_name);
                result[std::pmr::string(serialization_fields::BLOCK_CATEGORY)]    = std::string(gr::meta::enumName(blockCategory).value_or(""));

                // Requesting graph serialization
                property_map serializedChildren;
                auto         graphData = _graph->propertyCallbackGraphInspect(graph::property::kGraphInspect, {});
                if (!graphData.has_value()) {
                    return result;
                }
                serializedChildren[std::pmr::string(_graph->unique_name)] = graphData->data.value();

                result[std::pmr::string(serialization_fields::BLOCK_CHILDREN)] = std::move(serializedChildren);
                return result;
            }();
        } else {
            message.data = {{"yamlData", saveGrc(gr::globalPluginLoader(), *_graph)}};
        }

        message.endpoint = scheduler::property::kSchedulerInspected;
        return message;
    }

    std::optional<Message> propertyCallbackInspectBlock([[maybe_unused]] std::string_view propertyName, Message message) {
        auto result = _graph->propertyCallbackInspectBlock(propertyName, message);
        if (result) {
            result->serviceName = this->unique_name;
        }
        return result;
    }

    std::optional<Message> propertyCallbackReplaceBlock([[maybe_unused]] std::string_view propertyName, Message message) {
        assert(propertyName == scheduler::property::kReplaceBlock);
        using namespace std::string_literals;
        const auto& messageData = message.data.value();
        const auto  uniqueName  = messageData.at("uniqueName").value_or(std::string_view{});
        const auto  type        = messageData.at("type").value_or(std::string_view{});
        if (uniqueName.empty() || type.empty()) {
            message.data = std::unexpected(Error{std::format("No uniqueName or type in the message {}", message)});
            return message;
        }
        const property_map& properties = [&] {
            if (auto it = messageData.find("properties"); it != messageData.end()) {
                auto* result = it->second.get_if<property_map>();
                if (result == nullptr) {
                    return property_map{};
                } else {
                    return *result;
                }
            } else {
                return property_map{};
            }
        }();

        auto* targetGraph = findTargetSubGraph(messageData);

        if (targetGraph == nullptr) {
            message.data = std::unexpected(Error{std::format("No target graph for the message {}", message)});
            return message;
        }

        auto [oldBlock, newBlockRaw] = [&] {
            WorkQuiescenceGuard quiescence(this); // _blocks is traversed by every worker and by forEachBlock
            return targetGraph->replaceBlock(uniqueName, type, properties);
        }();
        makeZombie(std::move(oldBlock));
        wakeParkedWorkers();

        std::optional<Message> result = gr::Message{};
        result->endpoint              = scheduler::property::kBlockReplaced;
        result->data                  = serializeBlock(gr::globalPluginLoader(), newBlockRaw, BlockSerializationFlags::All);

        (*result->data)["_targetGraph"]            = targetGraph->unique_name.value();
        (*result->data)["replacedBlockUniqueName"] = uniqueName;

        return result;
    }
};

namespace detail {
// contiguous slices keep chain neighbors on the same worker
inline JobLists batchBlocks(std::span<const std::shared_ptr<BlockModel>> blocks, std::size_t n_batches) {
    JobLists result;
    result.reserve(n_batches);
    const std::size_t nBlocks = blocks.size();
    for (std::size_t batch = 0UZ; batch < n_batches; ++batch) {
        const std::size_t first = batch * nBlocks / n_batches;
        const std::size_t last  = (batch + 1UZ) * nBlocks / n_batches;
        const auto        slice = blocks.subspan(first, last - first);
        result.emplace_back(slice.begin(), slice.end());
    }
    return result;
}

inline void printExecutionOrder(const std::vector<std::vector<std::shared_ptr<BlockModel>>>& executionOrder) {
    std::size_t batchIndex = 0;
    for (const auto& batch : executionOrder) {
        std::print("Batch #{}:\n", batchIndex++);
        for (const auto& block : batch) {
            std::print("  - {} ({})\n", block->name(), block->uniqueName());
        }
    }
}

} // namespace detail

template<ExecutionPolicy execution = ExecutionPolicy::singleThreaded, profiling::ProfilerLike TProfiler = profiling::null::Profiler>
struct Simple : SchedulerBase<Simple<execution, TProfiler>, execution, TProfiler> {
    using Description = Doc<R""(Simple loop based Scheduler, which iterates over all blocks in the order they have beein defined and emplaced definition in the graph.)"">;

    using SchedulerBase<Simple<execution, TProfiler>, execution, TProfiler>::SchedulerBase;

    void customInit() {
        [[maybe_unused]] const auto pe = this->_profilerHandler->startCompleteEvent("scheduler_simple.init");

        // generate job list
        const gr::Graph   flatGraph = graph::flatten(*this->_graph);
        const std::size_t nBlocks   = flatGraph.blocks().size();

        std::size_t n_batches = 1UZ;
        switch (this->executionPolicy()) {
        case ExecutionPolicy::singleThreaded:
        case ExecutionPolicy::singleThreadedBlocking: break;
        case ExecutionPolicy::multiThreaded: n_batches = this->nJobLists(nBlocks); break;
        default:;
        }

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(n_batches);
        *this->_executionOrder = detail::batchBlocks(flatGraph.blocks(), n_batches);
    }
};

template<ExecutionPolicy execution = ExecutionPolicy::singleThreaded, profiling::ProfilerLike TProfiler = profiling::null::Profiler>
struct BreadthFirst : SchedulerBase<BreadthFirst<execution, TProfiler>, execution, TProfiler> {
    using Description = Doc<R""(Breadth First Scheduler which traverses the graph starting from the source blocks in a breath first fashion
detecting cycles and blocks which can be reached from several source blocks.)"">;

    using SchedulerBase<BreadthFirst<execution, TProfiler>, execution, TProfiler>::SchedulerBase;

    static_assert(execution == ExecutionPolicy::singleThreaded || execution == ExecutionPolicy::multiThreaded, "Unsupported execution policy");

    void customInit() {
        /* implements Breadth-first search scheduling algorithm (https://en.wikipedia.org/wiki/Breadth-first_search)
         * 1. compute 'adjacencyList'
         * 2. determine all 'sourceBlocks' S (no incoming edges)
         * 3. initialise queue Q with S
         * 4. while Q not empty:
         *   - dequeue Block B
         *   - if B not visited:
         *     - mark visited
         *     - add B to result
         *   - for each outgoing edge from B:
         *     - if target not yet reached, enqueue target
         * 5. append every block of the flattened graph that Q never reached, in the flattened graph's order
         *
         * For more details see also:
         * [1] T. H. Cormen, C. E. Leiserson, R. L. Rivest, and C. Stein, "Introduction to Algorithms", 3rd ed., MIT Press, 2009, ch. 22.2.
         * [2] P. Morin, "Open Data Structures". [Online]. available at: https://opendatastructures.org/
         */
        using block_t                  = std::shared_ptr<BlockModel>;
        [[maybe_unused]] const auto pe = this->_profilerHandler->startCompleteEvent("breadth_first.init");

        gr::Graph                      flatGraph     = gr::graph::flatten(*this->_graph);
        const gr::graph::AdjacencyList adjacencyList = graph::computeAdjacencyList(flatGraph);
        const std::vector<block_t>     sourceBlocks  = graph::findSourceBlocks(adjacencyList);

        std::vector<block_t>        blockList;
        std::unordered_set<block_t> visited;
        std::queue<block_t>         queue;
        std::set<block_t>           reached;

        for (const auto& src : sourceBlocks) {
            if (reached.insert(src).second) {
                queue.push(src);
            }
        }

        while (!queue.empty()) {
            block_t current = queue.front();
            queue.pop();

            if (visited.insert(current).second) {
                blockList.push_back(current);
            }

            // enqueue outgoing neighbours, but only once
            if (adjacencyList.contains(current)) {
                for (const auto& edges : adjacencyList.at(current) | std::views::values) {
                    for (const auto* edge : edges) {
                        const auto& dst = edge->destinationBlock();
                        if (reached.insert(dst).second) {
                            queue.push(dst);
                        }
                    }
                }
            }
        }

        for (const auto& block : flatGraph.blocks()) {
            if (!visited.contains(block)) {
                blockList.push_back(block);
            }
        }

        const std::size_t n_batches = (execution == ExecutionPolicy::multiThreaded) ? this->nJobLists(blockList.size()) : 1UZ;

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(n_batches);
        *this->_executionOrder = detail::batchBlocks(blockList, n_batches);
    }
};

template<ExecutionPolicy execution = ExecutionPolicy::singleThreaded, profiling::ProfilerLike TProfiler = profiling::null::Profiler>
struct DepthFirst : SchedulerBase<DepthFirst<execution, TProfiler>, execution, TProfiler> {
    using Description = Doc<R""(Depth First Scheduler which traverses the graph starting from the source blocks in a depth-first manner.)"">;

    using SchedulerBase<DepthFirst<execution, TProfiler>, execution, TProfiler>::SchedulerBase;

    static_assert(execution == ExecutionPolicy::singleThreaded || execution == ExecutionPolicy::multiThreaded, "Unsupported execution policy");

    void customInit() {
        /**
         * implements Depth-first search scheduling algorithm (https://en.wikipedia.org/wiki/Depth-first_search)
         * 1. compute 'adjacencyList'
         * 2. determine all `sourceBlocks' S (no incoming edges)
         * 3. initialise visited set
         * 4. for each source s in S:
         *   - recursively visit(s)
         * 5. visit(Block B):
         *   - if B visited: return
         *   - mark B visited
         *   - add B to result
         *   - for each outgoing edge from B:
         *     - recursively visit(destination)
         * 6. append every block of the flattened graph that no visit reached, in the flattened graph's order
         *
         * For more details see also:
         * [1] T. H. Cormen, C. E. Leiserson, R. L. Rivest, and C. Stein, "Introduction to Algorithms", 3rd ed., MIT Press, 2009, ch. 22.2.
         * [2] P. Morin, "Open Data Structures". [Online]. available at: https://opendatastructures.org/
         */
        using block_t                  = std::shared_ptr<BlockModel>;
        [[maybe_unused]] const auto pe = this->_profilerHandler->startCompleteEvent("depth_first.init");

        gr::Graph                  flatGraph     = gr::graph::flatten(*this->_graph);
        const graph::AdjacencyList adjacencyList = graph::computeAdjacencyList(flatGraph);
        const std::vector<block_t> sourceBlocks  = graph::findSourceBlocks(adjacencyList);

        std::vector<block_t> blockList;
        std::set<block_t>    visited;

        auto dfs = [&](this auto&& self, const block_t& node) -> void {
            if (!visited.insert(node).second) {
                return; // already visited
            }
            blockList.push_back(node);

            if (adjacencyList.contains(node)) {
                for (const auto& edges : adjacencyList.at(node) | std::views::values) {
                    for (const auto* edge : edges) {
                        self(edge->destinationBlock());
                    }
                }
            }
        };

        for (const auto& src : sourceBlocks) {
            dfs(src);
        }

        for (const auto& block : flatGraph.blocks()) {
            if (!visited.contains(block)) {
                blockList.push_back(block);
            }
        }

        const std::size_t n_batches = (execution == ExecutionPolicy::multiThreaded) ? this->nJobLists(blockList.size()) : 1UZ;

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(n_batches);
        *this->_executionOrder = detail::batchBlocks(blockList, n_batches);
    }
};

} // namespace gr::scheduler

#endif // GNURADIO_SCHEDULER_HPP
