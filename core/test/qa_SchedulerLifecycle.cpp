#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gnuradio-4.0/BlockingSync.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

namespace qa_sched {

// isBlocking() keeps the scheduler from moving this block to PAUSED itself, so it settles in
// REQUESTED_PAUSE and must be able to leave it
struct BlockingSource : gr::Block<BlockingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(BlockingSource, out);

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return true; }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    std::size_t _nReceived  = 0UZ;
    int         _nStopCalls = 0;

    void stop() { _nStopCalls++; }

    void processOne(float) { _nReceived++; }
};

constexpr std::size_t kSamplesBeforeTerminal = 32UZ;

// returns DONE without an external stop request, so finaliseIO() must route the DONE path through REQUESTED_STOP
struct DoneSource : gr::Block<DoneSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(DoneSource, out);

    int         _nStopCalls = 0;
    std::size_t _nEmitted   = 0UZ;

    void stop() { _nStopCalls++; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= kSamplesBeforeTerminal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t nPublish = std::min(outSpan.size(), 8UZ);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

struct FailingSource : gr::Block<FailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FailingSource, out);

    int         _nStopCalls = 0;
    std::size_t _nEmitted   = 0UZ;

    void stop() { _nStopCalls++; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= kSamplesBeforeTerminal) {
            outSpan.publish(0UZ);
            return gr::work::Status::ERROR;
        }
        const std::size_t nPublish = std::min(outSpan.size(), 8UZ);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

// a source whose device is absent: start() throws, so the block lands in ERROR before one sample moves
struct ThrowingStartSource : gr::Block<ThrowingStartSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ThrowingStartSource, out);

    std::size_t _nEmitted = 0UZ;

    void start() { throw gr::exception("the device refused to open"); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        _nEmitted += outSpan.size();
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }
};

// a source whose device opens only once it is plugged in. start() throws until then. An open device delivers a
// bounded stream.
struct PluggableSource : gr::Block<PluggableSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PluggableSource, out);

    bool        _pluggedIn = false;
    std::size_t _nEmitted  = 0UZ;

    void start() {
        if (!_pluggedIn) {
            throw gr::exception("the device is not plugged in");
        }
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= kSamplesBeforeTerminal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t nPublish = std::min(outSpan.size(), 8UZ);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

// a source whose device is slow to open. start() returns once gSlowStartReleased is set. An open device delivers an
// endless stream.
inline std::atomic<bool> gSlowStartEntered{false};
inline std::atomic<bool> gSlowStartReleased{false};

struct SlowStartSource : gr::Block<SlowStartSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SlowStartSource, out);

    void start() {
        gSlowStartEntered.store(true, std::memory_order_release);
        gSlowStartReleased.wait(false, std::memory_order_acquire);
    }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

// a source whose device is named by a setting it cannot resolve: settingsChanged() throws while the graph
// is built, which is the user code Block::init() runs, and without a device the block ends the stream on
// its first work call
struct ThrowingInitSource : gr::Block<ThrowingInitSource> {
    gr::PortOut<float> out;

    gr::Annotated<std::string, "device name"> device_name = "";

    GR_MAKE_REFLECTABLE(ThrowingInitSource, out, device_name);

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) { throw gr::exception("the device is not registered"); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return gr::work::Status::DONE;
    }
};

// the start()/stop() hooks of a start-then-stop cycle, observed from the requesting thread
inline std::atomic<int> gStartHooks{0};
inline std::atomic<int> gStopHooks{0};

struct RaceSource : gr::Block<RaceSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RaceSource, out);

    void start() {
        gStartHooks.fetch_add(1, std::memory_order_release);
        gStartHooks.notify_all();
    }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct RaceSink : gr::Block<RaceSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(RaceSink, in);

    void start() {
        gStartHooks.fetch_add(1, std::memory_order_release);
        gStartHooks.notify_all();
    }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    void processOne(float) {}
};

struct SelfStoppingSource : gr::Block<SelfStoppingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SelfStoppingSource, out);

    int         _nStopCalls = 0;
    std::size_t _nEmitted   = 0UZ;

    void stop() { _nStopCalls++; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_nEmitted >= kSamplesBeforeTerminal) {
            outSpan.publish(0UZ);
            this->requestStop();
            return gr::work::Status::OK;
        }
        const std::size_t nPublish = std::min(outSpan.size(), 8UZ);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

// produces nothing until gSourceGate opens, then a bounded stream. The gate is read before the call is
// counted, so a test that flips the gate after seeing a call knows that call ran gated
inline std::atomic<bool>        gSourceGate{false};
inline std::atomic<std::size_t> gGatedSourceCalls{0UZ};

struct GatedSource : gr::Block<GatedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GatedSource, out);

    std::size_t _nEmitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const bool open = gSourceGate.load(std::memory_order_acquire);
        gGatedSourceCalls.fetch_add(1UZ, std::memory_order_release);
        gGatedSourceCalls.notify_all();
        if (!open) {
            outSpan.publish(0UZ);
            return gr::work::Status::OK;
        }
        if (_nEmitted >= kSamplesBeforeTerminal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t nPublish = std::min(outSpan.size(), 8UZ);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

// A thread outside the scheduler offers samples, advances the block's progress sequence and notifies
// it, the way a capture source's receive thread does. The block's next call publishes what was offered.
// Each call records the time it began, up to kMaxTimedCalls calls.
inline constexpr std::size_t                                             kMaxTimedCalls = 1UZ << 14;
inline std::atomic<std::size_t>                                          gOfferedSamples{0UZ};
inline std::atomic<std::size_t>                                          gPublishedOffers{0UZ};
inline std::atomic<std::size_t>                                          gOfferedSourceCalls{0UZ};
inline std::array<std::chrono::steady_clock::time_point, kMaxTimedCalls> gOfferedSourceCallTimes{};

struct OfferedSource : gr::Block<OfferedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OfferedSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t call = gOfferedSourceCalls.load(std::memory_order_relaxed);
        if (call < kMaxTimedCalls) {
            gOfferedSourceCallTimes[call] = std::chrono::steady_clock::now();
        }
        const std::size_t nPublish = std::min(gOfferedSamples.exchange(0UZ, std::memory_order_acq_rel), outSpan.size());
        gPublishedOffers.fetch_add(nPublish, std::memory_order_relaxed);
        outSpan.publish(nPublish);
        gOfferedSourceCalls.store(call + 1UZ, std::memory_order_release);
        gOfferedSourceCalls.notify_all();
        return gr::work::Status::OK;
    }
};

// returns the call count once it exceeds nCalls
inline std::size_t awaitOfferedSourceCallBeyond(std::size_t nCalls) {
    std::size_t seen = gOfferedSourceCalls.load(std::memory_order_acquire);
    while (seen <= nCalls) {
        gOfferedSourceCalls.wait(seen, std::memory_order_acquire);
        seen = gOfferedSourceCalls.load(std::memory_order_acquire);
    }
    return seen;
}

[[nodiscard]] inline double median(std::vector<double> values) {
    std::ranges::sort(values);
    return values.empty() ? 0.0 : values[values.size() / 2UZ];
}

struct EndlessSource : gr::Block<EndlessSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

// paces itself to wall-clock time rather than to buffer availability: the clock port is left unconnected,
// so BlockingSync's timer thread is what releases the next chunk. Nothing in the block ends the stream
struct PacedSource : gr::Block<PacedSource>, gr::BlockingSync<PacedSource> {
    gr::PortIn<std::uint8_t, gr::Optional> clk_in;
    gr::PortOut<float>                     out;

    gr::Annotated<float, "sample_rate">     sample_rate = 100'000.f;
    gr::Annotated<gr::Size_t, "chunk_size"> chunk_size  = 16U;

    GR_MAKE_REFLECTABLE(PacedSource, clk_in, out, sample_rate, chunk_size);

    // the timer task reads members of this class, which ~Block() would already have outlived
    ~PacedSource() { this->stopTimerAndJoin(); }

    void start() { this->blockingSyncStart(); }
    void stop() { this->blockingSyncStop(); }

    gr::work::Status processBulk(gr::InputSpanLike auto& clkIn, gr::OutputSpanLike auto& outSpan) {
        const std::size_t nSamples = this->syncSamples(clkIn, outSpan);
        std::ignore                = clkIn.consume(0UZ);
        if (nSamples == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < nSamples; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nSamples);
        return gr::work::Status::OK;
    }
};

// only this block ends the stream, so a job list that never gets a thread hangs the graph
struct StoppingSink : gr::Block<StoppingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(StoppingSink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(float) {
        if (++_nReceived >= kSamplesBeforeTerminal) {
            this->requestStop();
        }
    }
};

// holds one thread of a two-thread pool for as long as it is alive
struct PoolOccupier {
    std::atomic<bool> _running{false};
    std::atomic<bool> _release{false};
    std::atomic<bool> _returned{false};

    explicit PoolOccupier(gr::thread_pool::TaskExecutor& pool) {
        pool.execute([this] {
            _running.store(true, std::memory_order_release);
            _running.notify_all();
            _release.wait(false, std::memory_order_acquire);
            _returned.store(true, std::memory_order_release);
            _returned.notify_all();
        });
        _running.wait(false, std::memory_order_acquire);
    }

    ~PoolOccupier() {
        _release.store(true, std::memory_order_release);
        _release.notify_all();
        _returned.wait(false, std::memory_order_acquire);
    }

    PoolOccupier(const PoolOccupier&)            = delete;
    PoolOccupier& operator=(const PoolOccupier&) = delete;
};

using TestScheduler     = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;
using SerialScheduler   = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;
using BlockingScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>;

// samples the adopted sub-scheduler's graph has moved, observed from outside its thread
inline std::atomic<std::size_t> gSubSchedulerSamples{0UZ};

struct SharedCountingSink : gr::Block<SharedCountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(SharedCountingSink, in);

    void processOne(float) { gSubSchedulerSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// samples a graph has moved, read by the test while the run goes on
inline std::atomic<std::size_t> gObservedSamples{0UZ};

struct ObservedSink : gr::Block<ObservedSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(ObservedSink, in);

    void processOne(float) { gObservedSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// adoptBlock is the scheduler's entry point for a block added to an already running graph. A message handler calls it
// on a worker inside processScheduledMessages(), which also forwards the children's messages to msgOut. The test calls
// it from its own thread. The wrapper holds the same flag as processScheduledMessages(). A report of adoptBlock() and
// a worker's forwarding then never use msgOut's single writer at once
struct AdoptingScheduler : TestScheduler {
    using TestScheduler::TestScheduler;

    void adoptBlock(const std::shared_ptr<gr::BlockModel>& newBlock) {
        while (std::atomic_flag_test_and_set_explicit(&this->_processingScheduledMessages, std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        TestScheduler::adoptBlock(newBlock);
        std::atomic_flag_clear_explicit(&this->_processingScheduledMessages, std::memory_order_release);
    }

    [[nodiscard]] std::size_t nWorkersStarted() { return gr::atomic_ref(this->_nWorkersStarted).load_acquire(); }
};

// a scheduler that supplies its own worker instead of the one the base provides: the smallest loop that runs a job
// list, so that what it exercises is the dispatch every scheduler shares and not the base worker's body
struct OwnWorkerScheduler : gr::scheduler::SchedulerBase<OwnWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded> {
    using Base = gr::scheduler::SchedulerBase<OwnWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded>;
    using Base::SchedulerBase;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        const auto      blocks    = flatGraph.blocks();

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1UZ);
        this->_executionOrder->clear();
        this->_executionOrder->emplace_back(blocks.begin(), blocks.end());
    }

    void poolWorker(std::size_t runnerID, std::shared_ptr<gr::scheduler::JobLists> jobList, std::size_t generation) {
        std::shared_ptr<gr::Sequence> nRunningJobs = this->_nRunningJobs;
        gr::on_scope_exit             release      = [this, &nRunningJobs] { this->releaseWorkerCount(*nRunningJobs); };

        std::vector<std::shared_ptr<gr::BlockModel>> localBlockList;
        {
            std::lock_guard lock(this->_executionOrderMutex);
            localBlockList = jobList->at(runnerID);
        }

        while (gr::lifecycle::isActive(this->state()) && gr::atomic_ref(this->_workerGeneration).load_acquire() == generation) {
            const gr::work::Result result = this->traverseBlockListOnce(localBlockList);
            if (result.status == gr::work::Status::DONE || result.status == gr::work::Status::ERROR) {
                return;
            }
        }
    }
};

inline std::atomic<std::size_t> gTwoArgumentWorkers{0UZ};

// a scheduler whose own worker takes the job list alone, without the run's generation. The worker runs until the
// scheduler leaves an active state.
struct TwoArgumentWorkerScheduler : gr::scheduler::SchedulerBase<TwoArgumentWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded> {
    using Base = gr::scheduler::SchedulerBase<TwoArgumentWorkerScheduler, gr::scheduler::ExecutionPolicy::multiThreaded>;
    using Base::SchedulerBase;

    void customInit() {
        const gr::Graph flatGraph = gr::graph::flatten(*this->_graph);
        const auto      blocks    = flatGraph.blocks();

        std::lock_guard lock(this->_executionOrderMutex);
        std::lock_guard guard(this->_adoptionBlocksMutex);
        this->_adoptionBlocks.clear();
        this->_adoptionBlocks.resize(1UZ);
        this->_executionOrder->clear();
        this->_executionOrder->emplace_back(blocks.begin(), blocks.end());
    }

    void poolWorker(std::size_t runnerID, std::shared_ptr<gr::scheduler::JobLists> jobList) {
        gTwoArgumentWorkers.fetch_add(1UZ, std::memory_order_relaxed);
        std::shared_ptr<gr::Sequence> nRunningJobs = this->_nRunningJobs;
        gr::on_scope_exit             release      = [this, &nRunningJobs] { this->releaseWorkerCount(*nRunningJobs); };

        std::vector<std::shared_ptr<gr::BlockModel>> localBlockList;
        {
            std::lock_guard lock(this->_executionOrderMutex);
            localBlockList = jobList->at(runnerID);
        }

        while (gr::lifecycle::isActive(this->state())) {
            const gr::work::Result result = this->traverseBlockListOnce(localBlockList);
            if (result.status == gr::work::Status::DONE || result.status == gr::work::Status::ERROR) {
                return;
            }
        }
    }
};

template<typename TScheduler>
[[nodiscard]] std::shared_ptr<gr::SchedulerWrapper<TScheduler>> makeSubScheduler(std::string_view poolName) {
    using namespace boost::ut;

    gr::Graph innerFlow;
    auto&     innerSource = innerFlow.emplaceBlock<EndlessSource>();
    auto&     innerSink   = innerFlow.emplaceBlock<SharedCountingSink>();
    expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

    auto inner = std::make_shared<gr::SchedulerWrapper<TScheduler>>(gr::property_map{{"poolName", std::string(poolName)}});
    inner->setGraph(std::move(innerFlow));
    return inner;
}

// an error message carries no property map, so a failed endpoint is told apart from a reply by its data alone
[[nodiscard]] std::string awaitErrorMessage(gr::MsgPortIn& port, std::string_view endpoint, std::size_t nAttempts = 2000UZ) {
    for (std::size_t i = 0UZ; i < nAttempts; ++i) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == endpoint && !message.data.has_value()) {
                std::string text = message.data.error().message;
                std::ignore      = messages.consume(messages.size());
                return text;
            }
        }
        std::ignore = messages.consume(messages.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

struct WatchdogProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] std::size_t nWatchdogsRunning() { return gr::atomic_ref(this->_nWatchdogsRunning).load_acquire(); }
};

[[nodiscard]] gr::Graph makeGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<BlockingSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] gr::Graph makeEndlessGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<EndlessSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

[[nodiscard]] bool awaitState(const TestScheduler& scheduler, gr::lifecycle::State expected) {
    for (std::size_t i = 0UZ; i < 2000UZ; ++i) {
        if (scheduler.state() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

constexpr auto kRunBound   = std::chrono::milliseconds(500);
constexpr auto kEventBound = std::chrono::seconds(5);

template<typename TPredicate>
[[nodiscard]] bool awaitCondition(TPredicate satisfied, std::chrono::milliseconds bound = kEventBound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!satisfied()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

[[nodiscard]] bool awaitObservedSamplesAbove(std::size_t nSamples) {
    return awaitCondition([nSamples] { return gObservedSamples.load(std::memory_order_relaxed) > nSamples; });
}

// runAndWait() on its own thread with a deadline, so a stop that fails to take fails the assertion
// instead of hanging ctest; `duringStartup` runs on the caller's thread the moment the runner exists
template<typename TScheduler, typename TDuringStartup>
[[nodiscard]] bool runAndWaitWithin(TScheduler& scheduler, std::chrono::milliseconds bound, TDuringStartup duringStartup) {
    std::mutex              mutex;
    std::condition_variable finished;
    bool                    returned = false;

    std::thread runner([&scheduler, &mutex, &finished, &returned] {
        std::ignore = scheduler.runAndWait();
        {
            std::lock_guard lock(mutex);
            returned = true;
        }
        finished.notify_one();
    });
    duringStartup();

    bool inTime = false;
    {
        std::unique_lock lock(mutex);
        inTime = finished.wait_for(lock, bound, [&returned] { return returned; });
    }
    if (!inTime) {
        scheduler.requestStop(); // release the run loop that the lost stop left behind
    }
    runner.join();
    return inTime;
}

template<typename TScheduler>
[[nodiscard]] bool runAndWaitWithin(TScheduler& scheduler, std::chrono::milliseconds bound) {
    return runAndWaitWithin(scheduler, bound, [] {});
}

constexpr std::string_view kOccupiedPoolName = "qa_occupied_cpu";
constexpr std::string_view kAdoptionPoolName = "qa_adoption_cpu";

[[nodiscard]] std::shared_ptr<gr::thread_pool::TaskExecutor> fixedPool(std::string_view name, std::uint32_t nThreads) {
    auto pool = std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(name, gr::thread_pool::TaskType::CPU_BOUND, nThreads, nThreads), "CPU");
    gr::thread_pool::Manager::instance().replacePool(std::string(name), pool);
    return pool;
}

[[nodiscard]] std::shared_ptr<gr::thread_pool::TaskExecutor> twoThreadPool() { return fixedPool(kOccupiedPoolName, 2U); }

void startAndPause(TestScheduler& scheduler) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    expect(scheduler.exchange(makeGraph()).has_value());
    expect(scheduler.changeStateTo(INITIALISED).has_value());
    expect(scheduler.changeStateTo(RUNNING).has_value());
    expect(awaitState(scheduler, RUNNING)) << "scheduler did not reach RUNNING";

    expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
    expect(awaitState(scheduler, PAUSED)) << "scheduler did not reach PAUSED";
}

} // namespace qa_sched

const boost::ut::suite<"scheduler pause lifecycle"> schedulerLifecycleTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a paused scheduler can be stopped and then destroyed"_test = [] {
        qa_sched::TestScheduler scheduler;
        qa_sched::startAndPause(scheduler);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "scheduler did not reach STOPPED after a pause";
    };

    "a paused scheduler is destructible without a prior stop"_test = [] {
        qa_sched::TestScheduler scheduler;
        qa_sched::startAndPause(scheduler);
        // ~SchedulerBase() must request the stop itself; the workers are parked, not gone
    };

    "a paused scheduler resumes"_test = [] {
        qa_sched::TestScheduler scheduler;
        qa_sched::startAndPause(scheduler);

        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitState(scheduler, RUNNING)) << "scheduler did not resume";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "scheduler did not stop after resuming";
    };
};

const boost::ut::suite<"block stop hook on terminal paths"> stopHookTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a source that returns DONE runs its stop hook once"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(source._nStopCalls, 1)) << "stop() must run for a block that ends the stream itself";
        expect(source.state() == STOPPED);
        expect(eq(sink._nStopCalls, 1)) << "the downstream block stops via the end-of-stream tag";
        expect(gt(sink._nReceived, 0UZ));
    };

    "a source that returns ERROR runs every block's stop hook once"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::FailingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value()) << "a run the scheduler finished in ERROR is reported as failed, not as a successful run";

        expect(scheduler.state() == ERROR) << "a failing block drives the scheduler to ERROR";
        expect(eq(source._nStopCalls, 1)) << "stop() must run for the block that failed";
        expect(eq(sink._nStopCalls, 1)) << "stop() must run for the blocks torn down alongside it";
    };

    "a start() that throws fails runAndWait"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value()) << "a graph whose source never started must not report a successful run";
        expect(eq(source._nEmitted, 0UZ)) << "no sample moves through a block that refused to start";
    };

    "a start() that throws fails runAndWait with a message subscriber attached"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value()) << "a delivered error message must not turn the failed run into a success";
        expect(eq(source._nEmitted, 0UZ));
    };

    "an init() that throws fails runAndWait"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingInitSource>(gr::property_map{{"device_name", std::string("absent")}});
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        expect(source.state() == ERROR) << "a block whose init() hook threw must not report itself initialized";

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        const std::expected<void, gr::Error> result = scheduler.runAndWait();
        expect(!result.has_value()) << "a graph whose source never initialized must not report a successful run";
        if (!result.has_value()) {
            expect(result.error().message.find(std::string(source.unique_name)) != std::string::npos) << "the error must name the block that failed to initialize";
            expect(result.error().message.find("the device is not registered") != std::string::npos) << "the error must carry what the init() hook threw";
        }
        expect(eq(sink._nReceived, 0UZ)) << "no sample moves through a block that never initialized";
    };

    "an init() that throws fails runAndWait with a message subscriber attached"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingInitSource>(gr::property_map{{"device_name", std::string("absent")}});
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value()) << "a delivered error message must not turn the failed run into a success";
        expect(eq(sink._nReceived, 0UZ));
    };

    "a repeated run of a graph that failed to initialize fails again"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingInitSource>(gr::property_map{{"device_name", std::string("absent")}});
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value());
        expect(!scheduler.runAndWait().has_value()) << "a second run must not clear a block's init failure";
        expect(eq(sink._nReceived, 0UZ));
    };

    "an ordinary requestStop runs the stop hook once"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::SelfStoppingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(source._nStopCalls, 1)) << "the REQUESTED_STOP path must not fire stop() a second time on the way to STOPPED";
        expect(eq(sink._nStopCalls, 1));
    };
};

// a start that cannot complete spawns no worker, so the start itself ends the scheduler in ERROR and keeps the reason,
// whichever call started it
const boost::ut::suite<"a start that cannot complete"> failedStartTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a failed start ends in ERROR for a caller that starts the scheduler itself"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        std::ignore = scheduler.changeStateTo(RUNNING); // the start's outcome is read from the state

        expect(qa_sched::awaitCondition([&scheduler] { return !gr::lifecycle::isActive(scheduler.state()); }, qa_sched::kRunBound)) << "a scheduler whose start could not complete still reads active";
        expect(scheduler.state() == ERROR) << "a start that could not complete must end the scheduler in ERROR";

        const std::optional<gr::Error> reason = scheduler.startError();
        expect(reason.has_value()) << "the scheduler must keep the reason its start could not complete";
        if (reason.has_value()) {
            expect(reason->message.find(std::string(source.unique_name)) != std::string::npos) << "the reason must name the block that failed to start";
            expect(reason->message.find("the device refused to open") != std::string::npos) << "the reason must carry what the start() hook threw";
        }
        expect(eq(source._nEmitted, 0UZ)) << "no sample moves through a block that refused to start";
        expect(eq(sink._nReceived, 0UZ));
    };

    "a failed start ends in ERROR for a sub-scheduler started on its own thread"_test = [] {
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>();
        inner->setGraph(std::move(innerFlow));

        inner->start();
        expect(inner->_schedulerThread.joinable()) << "the sub-scheduler did not start its thread";
        if (inner->_schedulerThread.joinable()) {
            inner->_schedulerThread.join(); // the start runs on this thread and has ended with it
        }

        expect(inner->blockRef().state() == ERROR) << "a sub-scheduler whose start could not complete must end in ERROR";
        expect(inner->blockRef().startError().has_value()) << "the sub-scheduler must keep the reason its start could not complete";
        expect(eq(innerSource._nEmitted, 0UZ));
        inner->stop();
    };

    "a sub-scheduler whose start fails inside its parent's start ends the parent in ERROR with the reason"_test = [] {
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>();
        inner->setGraph(std::move(innerFlow));

        gr::Graph                             flow       = qa_sched::makeEndlessGraph();
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        std::ignore = scheduler.changeStateTo(RUNNING); // the start's outcome is read from the state

        expect(qa_sched::awaitCondition([&scheduler] { return !gr::lifecycle::isActive(scheduler.state()); }, qa_sched::kRunBound)) << "a parent whose sub-scheduler could not start still reads active";
        expect(scheduler.state() == ERROR) << "a sub-scheduler's failed start must end its parent in ERROR";
        expect(innerBlock->state() == ERROR) << "the sub-scheduler's own start must have failed";

        const std::optional<gr::Error> reason = scheduler.startError();
        expect(reason.has_value()) << "the parent must keep the reason its start could not complete";
        if (reason.has_value()) {
            expect(reason->message.find(innerName) != std::string::npos) << "the reason must name the sub-scheduler: " << reason->message;
            expect(reason->message.find(std::string(innerSource.unique_name)) != std::string::npos) << "the reason must name the block that failed to start: " << reason->message;
            expect(reason->message.find("the device refused to open") != std::string::npos) << "the reason must carry what the start() hook threw: " << reason->message;
        }
        expect(eq(innerSource._nEmitted, 0UZ));
        if (gr::lifecycle::isActive(scheduler.state())) {
            scheduler.requestStop();
        }
    };

    "runAndWait returns the reason a start could not complete and leaves the scheduler in ERROR"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        const std::expected<void, gr::Error> result = scheduler.runAndWait();
        expect(!result.has_value()) << "a graph whose source never started must not report a successful run";
        expect(scheduler.state() == ERROR) << "a start that could not complete must end the scheduler in ERROR under runAndWait() as well";

        const std::optional<gr::Error> reason = scheduler.startError();
        expect(reason.has_value()) << "the reason must stay readable after runAndWait() returned it";
        if (!result.has_value()) {
            expect(result.error().message.find(std::string(source.unique_name)) != std::string::npos) << "the error must name the block that failed to start";
            expect(result.error().message.find("the device refused to open") != std::string::npos) << "the error must carry what the start() hook threw";
            if (reason.has_value()) {
                expect(eq(result.error().message, reason->message)) << "runAndWait() must return the reason the scheduler keeps";
            }
        }
        expect(eq(sink._nReceived, 0UZ));
    };

    "a scheduler whose start failed runs again with runAndWait once its block can start"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::PluggableSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value());
        expect(scheduler.state() == ERROR) << "the first run's start could not complete";

        source._pluggedIn = true;
        expect(scheduler.runAndWait().has_value()) << "runAndWait() must reset a scheduler in ERROR and run it";
        expect(!scheduler.startError().has_value()) << "a start that completed must not report an earlier failure";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the stream must pass once the source can start";
        expect(scheduler.state() == STOPPED);
    };

    "a scheduler whose start failed runs again with a new start once its block can start"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::PluggableSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        std::ignore = scheduler.changeStateTo(RUNNING);
        expect(scheduler.state() == ERROR) << "the first start could not complete";

        source._pluggedIn = true;
        expect(scheduler.changeStateTo(INITIALISED).has_value()) << "a scheduler in ERROR must accept a reset";
        expect(scheduler.changeStateTo(RUNNING).has_value());
        scheduler.waitDone(); // the workers end once the bounded stream has passed
        expect(!scheduler.startError().has_value()) << "a start that completed must not report an earlier failure";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the stream must pass once the source can start";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the second run did not stop";
    };
};

const boost::ut::suite<"a scheduler that supplies its own worker"> ownWorkerTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a scheduler whose own worker takes no generation runs a graph to its end"_test = [] {
        qa_sched::gTwoArgumentWorkers.store(0UZ, std::memory_order_relaxed);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TwoArgumentWorkerScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value()) << "a run that ends with DONE succeeds";

        expect(gt(qa_sched::gTwoArgumentWorkers.load(std::memory_order_relaxed), 0UZ)) << "the dispatch must reach the scheduler's own two-argument worker";
        expect(ge(source._nEmitted, qa_sched::kSamplesBeforeTerminal)) << "the source must have run to its end";
        expect(eq(sink._nReceived, source._nEmitted)) << "the whole stream must pass through the scheduler's own worker";
        expect(scheduler.state() == STOPPED);
    };
};

const boost::ut::suite<"stop requested during the start transient"> startStopRaceTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop landing inside start() still runs the blocks' stop lifecycle"_test = [] {
        constexpr int nCycles = 64;

        std::size_t nCyclesLeftActive  = 0UZ;
        std::size_t nCyclesInError     = 0UZ;
        std::size_t nCyclesMissingStop = 0UZ;

        for (int cycle = 0; cycle < nCycles; ++cycle) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::RaceSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            qa_sched::TestScheduler scheduler;
            expect(scheduler.exchange(std::move(flow)).has_value());

            qa_sched::gStartHooks.store(0, std::memory_order_release);
            qa_sched::gStopHooks.store(0, std::memory_order_release);

            std::jthread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });

            // every block is RUNNING once both start() hooks have run, so the stop below lands while
            // start() is dispatching its pool workers
            for (int seen = qa_sched::gStartHooks.load(std::memory_order_acquire); seen < 2; seen = qa_sched::gStartHooks.load(std::memory_order_acquire)) {
                qa_sched::gStartHooks.wait(seen);
            }
            scheduler.requestStop();

            runner.join();

            if (qa_sched::gStopHooks.load(std::memory_order_acquire) != 2) {
                nCyclesMissingStop++;
            }
            if (gr::lifecycle::isActive(scheduler.state())) {
                nCyclesLeftActive++;
            }
            if (scheduler.state() == ERROR) {
                nCyclesInError++;
            }
        }

        expect(eq(nCyclesMissingStop, 0UZ)) << "cycles in which a block's stop() hook was skipped";
        expect(eq(nCyclesLeftActive, 0UZ)) << "cycles that left the scheduler in an active state";
        expect(eq(nCyclesInError, 0UZ)) << "cycles in which a worker drove the scheduler to ERROR";
    };
};

const boost::ut::suite<"stop requested before RUNNING"> preRunningStopTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop requested while IDLE keeps runAndWait from starting the run"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        scheduler.requestStop();

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kRunBound)) << "runAndWait() blocked on a stop requested before it ran";
        expect(!gr::lifecycle::isActive(scheduler.state())) << "runAndWait() left the scheduler active";
        expect(eq(sink._nReceived, 0UZ)) << "a latched stop must not be overwritten by a reinitializing runAndWait()";
    };

    "a stop requested while INITIALISED keeps runAndWait from starting the run"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        scheduler.requestStop();

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kRunBound)) << "runAndWait() blocked on a stop requested before it ran";
        expect(!gr::lifecycle::isActive(scheduler.state())) << "runAndWait() left the scheduler active";
        expect(eq(sink._nReceived, 0UZ)) << "a latched stop must not be overwritten by a reinitializing runAndWait()";
    };

    "a stop racing the startup transient always releases runAndWait"_test = [] {
        constexpr int nCycles = 12;

        std::size_t nCyclesBlocked    = 0UZ;
        std::size_t nCyclesLeftActive = 0UZ;

        for (int cycle = 0; cycle < nCycles && nCyclesBlocked == 0UZ; ++cycle) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            qa_sched::BlockingScheduler scheduler;
            expect(scheduler.exchange(std::move(flow)).has_value());

            // no barrier: the stop lands wherever the runner happens to be, IDLE included
            if (!qa_sched::runAndWaitWithin(scheduler, qa_sched::kRunBound, [&scheduler] { scheduler.requestStop(); })) {
                nCyclesBlocked++;
            }
            if (gr::lifecycle::isActive(scheduler.state())) {
                nCyclesLeftActive++;
            }
        }

        expect(eq(nCyclesBlocked, 0UZ)) << "cycles in which runAndWait() did not return within the deadline";
        expect(eq(nCyclesLeftActive, 0UZ)) << "cycles that left the scheduler in an active state";
    };
};

const boost::ut::suite<"job lists sized to the free pool threads"> jobListSizingTests = [] {
    using namespace boost::ut;

    "a graph runs on a pool whose threads are not all free"_test = [] {
        auto                   pool = qa_sched::twoThreadPool();
        qa_sched::PoolOccupier occupier(*pool);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StoppingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler({{"poolName", std::string(qa_sched::kOccupiedPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        expect(qa_sched::runAndWaitWithin(scheduler, std::chrono::seconds(5))) << "a job list that got no pool thread stranded its blocks";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the sink must run for the stream to end";
    };

    // start() counts a worker generation before queueing it, so a stop that publishes STOPPED can
    // leave counted workers that never ran. start() drains those before acquiring
    // _executionOrderMutex, since a queued worker acquires the same mutex to copy its job list
    // before it can decrement. The drain terminates only because the workers are retired first: a
    // task that reaches the pool after the stop releases its count instead of running. Occupying
    // every thread of a fixed-size pool is what keeps a worker queued across the stop.
    "a restart completes while a worker of the previous generation is still queued"_test = [] {
        using enum gr::lifecycle::State;

        auto pool      = qa_sched::twoThreadPool();
        auto occupierA = std::make_unique<qa_sched::PoolOccupier>(*pool);
        auto occupierB = std::make_unique<qa_sched::PoolOccupier>(*pool);

        qa_sched::TestScheduler scheduler({{"poolName", std::string(qa_sched::kOccupiedPoolName)}});
        expect(scheduler.exchange(qa_sched::makeGraph()).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value()); // the generation is counted and queued behind the occupiers
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the scheduler did not publish STOPPED with its workers still queued";

        std::mutex              mutex;
        std::condition_variable finished;
        bool                    restarted = false;

        std::thread restarter([&scheduler, &mutex, &finished, &restarted] {
            std::ignore = scheduler.changeStateTo(INITIALISED);
            std::ignore = scheduler.changeStateTo(RUNNING);
            {
                std::lock_guard lock(mutex);
                restarted = true;
            }
            finished.notify_one();
        });

        // changeStateTo() publishes the new state and only then runs the hook, so RUNNING is
        // already visible while start() sits in its drain, and INITIALISED is observable for no
        // longer than reset() takes. What this step needs is that the restart has begun, which is
        // what leaving STOPPED means.
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() != STOPPED; })) << "the restart never left STOPPED";
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the restart reach the drain

        occupierA.reset(); // the queued generation becomes runnable only now
        occupierB.reset();

        bool inTime = false;
        {
            std::unique_lock lock(mutex);
            inTime = finished.wait_for(lock, std::chrono::seconds(5), [&restarted] { return restarted; });
        }
        expect(inTime) << "start() deadlocked against a queued worker of the previous generation";
        if (inTime) {
            restarter.join();
            expect(!scheduler.startError().has_value()) << "the restart could not start its blocks";
            std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted scheduler did not stop again";
        } else {
            restarter.detach(); // joining a thread stuck on the mutex would hang the suite instead of failing it
        }
    };
};

// a blocking block leaves REQUESTED_STOP in its own work() call. In these cases no worker calls work() after the stop:
// - the run's worker is still queued
// - no run has started since the reset
// - a pause has parked the workers
// - a running worker reads the stop before its next call
const boost::ut::suite<"a blocking block reaches STOPPED when its scheduler stops"> blockingBlockStopTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a blocking block whose worker never ran stops with the scheduler and runs after a restart"_test = [] {
        // occupying every thread of a fixed-size pool keeps the run's worker queued across the stop
        auto pool      = qa_sched::twoThreadPool();
        auto occupierA = std::make_unique<qa_sched::PoolOccupier>(*pool);
        auto occupierB = std::make_unique<qa_sched::PoolOccupier>(*pool);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StoppingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler({{"poolName", std::string(qa_sched::kOccupiedPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(!scheduler.workerStarted()) << "the run's worker must still be queued behind the occupied threads";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
        expect(source.state() == STOPPED) << "a blocking block whose worker never ran must stop with the scheduler";

        occupierA.reset(); // the queued worker of the stopped run becomes runnable
        occupierB.reset();
        expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the queued worker of the stopped run was not retired";
        expect(!scheduler.workerStarted()) << "a queued worker of the stopped run must be retired unstarted";
        expect(source.state() == STOPPED) << "a worker of the stopped run must leave the blocking block stopped";
        expect(eq(sink._nReceived, 0UZ)) << "a worker of the stopped run must not move a sample";

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the restarted run did not end";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the restart must run the blocking block";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted scheduler did not stop";
    };

    "a blocking block stopped after a reset stops with the scheduler and runs after a restart"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StoppingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the first run did not end";
        expect(scheduler.workerStarted()) << "the first run's workers must have run";
        expect(source.state() == STOPPED) << "the end of the stream must stop the source";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the first run did not stop";
        const std::size_t nFirstRun = sink._nReceived;

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(source.state() == INITIALISED) << "the reset must reinitialize the source";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the scheduler did not stop after the reset";
        expect(source.state() == STOPPED) << "a blocking block that no run has started since the reset must stop with the scheduler";

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the restarted run did not end";
        expect(gt(sink._nReceived, nFirstRun)) << "the restart must run the blocking block";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted scheduler did not stop";
    };

    "a blocking block of a paused run reaches STOPPED when the scheduler stops and runs after a restart"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the run did not move a sample";
        expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
        expect(qa_sched::awaitState(scheduler, PAUSED)) << "the scheduler did not pause";
        // a worker reads the pause some iterations late. Until the workers leave, quiescence holds each worker out of
        // work(), as the pause does once read
        scheduler.requestWorkQuiescence();
        expect(scheduler.isProcessing()) << "the paused run's workers must be parked, not gone";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the paused scheduler did not stop";
        expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the paused run's workers did not leave";
        scheduler.releaseWorkQuiescence();
        expect(source.state() == STOPPED) << "a blocking block of a paused run must reach STOPPED when the scheduler stops";

        const std::size_t nFirstRun = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(nFirstRun)) << "the restart must run the blocking block";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted scheduler did not stop";
    };

    // the stop lands between two work() calls at a point the test cannot choose. The case stops and restarts one
    // running graph many times and ends at the first restart that moves no sample
    "a blocking block of a running graph reaches STOPPED at every stop and runs after every restart"_test = [] {
        constexpr std::size_t kCycles = 20UZ;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::size_t nRuns                 = 0UZ;
        std::size_t nStopsWithoutWorkers  = 0UZ;
        std::size_t nStopsShortOfStopped  = 0UZ;
        std::size_t nStopsThatDidNotDrain = 0UZ;
        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            const std::size_t nBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
            std::ignore               = scheduler.changeStateTo(INITIALISED);
            std::ignore               = scheduler.changeStateTo(RUNNING);
            if (!qa_sched::awaitObservedSamplesAbove(nBefore)) {
                break;
            }
            nRuns++;
            nStopsWithoutWorkers += scheduler.isProcessing() ? 0UZ : 1UZ;

            std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            if (!qa_sched::awaitState(scheduler, STOPPED) || !qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) {
                nStopsThatDidNotDrain++;
                break;
            }
            nStopsShortOfStopped += source.state() == STOPPED ? 0UZ : 1UZ;
        }

        expect(eq(nStopsWithoutWorkers, 0UZ)) << "every stop must land on a run whose workers are inside their loop";
        expect(eq(nStopsThatDidNotDrain, 0UZ)) << "a stop of the running graph did not end its workers";
        expect(eq(nStopsShortOfStopped, 0UZ)) << "stops of a running graph that left the blocking block short of STOPPED";
        expect(eq(nRuns, kCycles)) << "every restart after a stop of the running graph must run the blocking block";
    };
};

const boost::ut::suite<"adopting a sub-scheduler"> subSchedulerAdoptionTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "an adopted sub-scheduler runs on its own thread"_test = [] {
        qa_sched::gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);

        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::SharedCountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::SerialScheduler>>();
        inner->setGraph(std::move(innerFlow));
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        qa_sched::AdoptingScheduler outer;
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());
        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";

        std::atomic<bool> returned{false};
        std::atomic<bool> innerRunningOnReturn{false};
        std::thread       adopter([&outer, &innerBlock, &returned, &innerRunningOnReturn] {
            outer.adoptBlock(innerBlock);
            innerRunningOnReturn.store(innerBlock->state() == RUNNING, std::memory_order_relaxed);
            returned.store(true, std::memory_order_release);
        });

        // the sub-scheduler's own start() has run to completion once its graph moves samples, whichever
        // thread that start() ran on, so the stop below cannot race it
        expect(qa_sched::awaitCondition([] { return qa_sched::gSubSchedulerSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the sub-scheduler's graph never ran";
        const bool adoptReturned = qa_sched::awaitCondition([&returned] { return returned.load(std::memory_order_acquire); });

        inner->stop(); // releases a sub-scheduler loop that is running on the adopting thread
        adopter.join();
        outer.requestStop();
        runner.join();

        expect(adoptReturned) << "adoptBlock ran the sub-scheduler's whole loop on the adopting thread";
        expect(innerRunningOnReturn.load(std::memory_order_relaxed)) << "the sub-scheduler must be running when adoptBlock returns";
    };

    "adoption into a pool with no free thread is refused"_test = [] {
        qa_sched::gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);
        auto pool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 2U);

        auto                                  inner      = qa_sched::makeSubScheduler<qa_sched::TestScheduler>(qa_sched::kAdoptionPoolName);
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        qa_sched::AdoptingScheduler outer({{"poolName", std::string(qa_sched::kAdoptionPoolName)}});
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());

        gr::MsgPortIn fromOuter;
        expect(outer.msgOut.connect(fromOuter).has_value());

        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";
        expect(qa_sched::awaitCondition([&outer] { return outer.nWorkersStarted() >= 2UZ; })) << "the adopting scheduler did not fill the pool";

        outer.adoptBlock(innerBlock);

        const std::string reported = qa_sched::awaitErrorMessage(fromOuter, "adoptBlock");
        expect(!reported.empty()) << "adoption into a full pool must report an error";
        expect(reported.find(std::string(qa_sched::kAdoptionPoolName)) != std::string::npos) << std::format("the error must name the pool: '{}'", reported);
        expect(innerBlock->state() != RUNNING) << "a sub-scheduler whose worker cannot be run must not publish RUNNING";
        expect(eq(qa_sched::gSubSchedulerSamples.load(std::memory_order_relaxed), 0UZ)) << "a refused sub-scheduler must not have run";

        inner->stop();
        outer.requestStop();
        runner.join();
    };

    "adoption into a pool with a free thread runs the adopted graph"_test = [] {
        qa_sched::gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);
        auto pool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 3U);

        auto                                  inner      = qa_sched::makeSubScheduler<qa_sched::TestScheduler>(qa_sched::kAdoptionPoolName);
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        qa_sched::AdoptingScheduler outer({{"poolName", std::string(qa_sched::kAdoptionPoolName)}});
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());

        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";
        expect(qa_sched::awaitCondition([&outer] { return outer.nWorkersStarted() >= 2UZ; })) << "the adopting scheduler did not claim its own threads";

        outer.adoptBlock(innerBlock);

        expect(innerBlock->state() == RUNNING) << "a sub-scheduler the pool can run must be running when adoptBlock returns";
        expect(inner->workerStarted()) << "adoptBlock returned before the sub-scheduler's worker began executing";
        expect(qa_sched::awaitCondition([] { return qa_sched::gSubSchedulerSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the adopted graph never moved a sample";

        inner->stop();
        outer.requestStop();
        runner.join();
    };

    "a sub-scheduler that supplies its own worker is adopted"_test = [] {
        qa_sched::gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);
        auto pool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 3U);

        auto                                  inner      = qa_sched::makeSubScheduler<qa_sched::OwnWorkerScheduler>(qa_sched::kAdoptionPoolName);
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        qa_sched::AdoptingScheduler outer({{"poolName", std::string(qa_sched::kAdoptionPoolName)}});
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());

        gr::MsgPortIn fromOuter;
        expect(outer.msgOut.connect(fromOuter).has_value());

        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";
        expect(qa_sched::awaitCondition([&outer] { return outer.nWorkersStarted() >= 2UZ; })) << "the adopting scheduler did not claim its own threads";

        outer.adoptBlock(innerBlock);

        // adoptBlock() reports before it returns, so one read of the port covers the whole adoption
        const std::string reported = qa_sched::awaitErrorMessage(fromOuter, "adoptBlock", 1UZ);
        expect(reported.empty()) << std::format("a sub-scheduler that runs must not be reported as failing: '{}'", reported);
        expect(innerBlock->state() == RUNNING) << "a sub-scheduler with its own worker must be running when adoptBlock returns";
        expect(inner->workerStarted()) << "a worker supplied by the scheduler itself is not reported as started";
        expect(qa_sched::awaitCondition([] { return qa_sched::gSubSchedulerSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the adopted graph never moved a sample";

        inner->stop();
        outer.requestStop();
        runner.join();
    };

    // the adoption waits up to watchdog_timeout for a worker of the sub-scheduler. A start that failed has none to wait
    // for. The report and the sub-scheduler's own start errors reach msgOut together, from the test's thread and a
    // worker
    "an adopted sub-scheduler whose start fails is reported with its reason before the timeout"_test = [] {
        auto pool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 3U);

        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>(gr::property_map{{"poolName", std::string(qa_sched::kAdoptionPoolName)}});
        inner->setGraph(std::move(innerFlow));
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        const auto                  timeout = qa_sched::kRunBound;
        qa_sched::AdoptingScheduler outer({{"poolName", std::string(qa_sched::kAdoptionPoolName)}, {"watchdog_timeout", static_cast<gr::Size_t>(timeout.count())}});
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());

        gr::MsgPortIn fromOuter;
        expect(outer.msgOut.connect(fromOuter).has_value());

        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";
        expect(qa_sched::awaitCondition([&outer] { return outer.nWorkersStarted() >= 2UZ; })) << "the adopting scheduler did not claim its own threads";

        const auto adoptionBegan = std::chrono::steady_clock::now();
        outer.adoptBlock(innerBlock);
        const auto adoptionTook = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - adoptionBegan);

        expect(innerBlock->state() == ERROR) << "the sub-scheduler's start must have failed";
        expect(adoptionTook < timeout) << std::format("adoptBlock waited {} ms for a sub-scheduler whose start failed, against a timeout of {} ms", adoptionTook.count(), timeout.count());

        const std::string reported = qa_sched::awaitErrorMessage(fromOuter, "adoptBlock");
        expect(reported.find(std::string(innerSource.unique_name)) != std::string::npos) << std::format("the report must name the block that failed to start: '{}'", reported);
        expect(reported.find("the device refused to open") != std::string::npos) << std::format("the report must carry what the start() hook threw: '{}'", reported);
        expect(eq(innerSource._nEmitted, 0UZ));

        inner->stop();
        outer.requestStop();
        runner.join();
    };

    // a start still in progress has no worker yet and is not a failure. The adoption keeps waiting for the worker.
    "an adopted sub-scheduler whose start is slow is waited for and runs"_test = [] {
        qa_sched::gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);
        qa_sched::gSlowStartEntered.store(false, std::memory_order_release);
        qa_sched::gSlowStartReleased.store(false, std::memory_order_release);
        auto pool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 3U);

        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::SlowStartSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::SharedCountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>(gr::property_map{{"poolName", std::string(qa_sched::kAdoptionPoolName)}});
        inner->setGraph(std::move(innerFlow));
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        qa_sched::AdoptingScheduler outer({{"poolName", std::string(qa_sched::kAdoptionPoolName)}, {"watchdog_timeout", static_cast<gr::Size_t>(std::chrono::milliseconds(qa_sched::kEventBound).count())}});
        expect(outer.exchange(qa_sched::makeEndlessGraph()).has_value());

        gr::MsgPortIn fromOuter;
        expect(outer.msgOut.connect(fromOuter).has_value());

        std::thread runner([&outer] { std::ignore = outer.runAndWait(); });
        expect(qa_sched::awaitState(outer, RUNNING)) << "the adopting scheduler did not reach RUNNING";
        expect(qa_sched::awaitCondition([&outer] { return outer.nWorkersStarted() >= 2UZ; })) << "the adopting scheduler did not claim its own threads";

        std::atomic<bool> returned{false};
        std::thread       adopter([&outer, &innerBlock, &returned] {
            outer.adoptBlock(innerBlock);
            returned.store(true, std::memory_order_release);
        });

        expect(qa_sched::awaitCondition([] { return qa_sched::gSlowStartEntered.load(std::memory_order_acquire); })) << "the sub-scheduler's start never reached its source";
        std::this_thread::sleep_for(qa_sched::kRunBound); // the start stays in progress for the timeout the failed-start case allows
        const bool returnedDuringStart = returned.load(std::memory_order_acquire);
        qa_sched::gSlowStartReleased.store(true, std::memory_order_release);
        qa_sched::gSlowStartReleased.notify_all();
        adopter.join();

        expect(!returnedDuringStart) << "adoptBlock must wait for a sub-scheduler whose start is still in progress";
        const std::string reported = qa_sched::awaitErrorMessage(fromOuter, "adoptBlock", 1UZ);
        expect(reported.empty()) << std::format("a slow start must not be reported as failing: '{}'", reported);
        expect(innerBlock->state() == RUNNING) << "a sub-scheduler whose start completed must be running when adoptBlock returns";
        expect(inner->workerStarted()) << "adoptBlock returned before the sub-scheduler's worker began executing";
        expect(qa_sched::awaitCondition([] { return qa_sched::gSubSchedulerSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the adopted graph never moved a sample";

        inner->stop();
        outer.requestStop();
        runner.join();
    };
};

const boost::ut::suite<"watchdog lifetime"> watchdogTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "restarts inside one check interval leave a single watchdog"_test = [] {
        constexpr std::size_t kCycles = 8UZ;

        // every cycle below falls inside one check interval, so no earlier watchdog can time out on its own
        qa_sched::WatchdogProbe scheduler({{"watchdog_timeout", gr::Size_t(5000)}});
        expect(scheduler.exchange(qa_sched::makeEndlessGraph()).has_value());

        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());
            expect(qa_sched::awaitState(scheduler, RUNNING)) << std::format("cycle {} did not reach RUNNING", cycle);
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(scheduler, STOPPED)) << std::format("cycle {} did not reach STOPPED", cycle);
        }

        // a watchdog left behind by an earlier cycle keeps going for as long as some run has jobs
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitState(scheduler, RUNNING)) << "the final run did not reach RUNNING";

        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.nWatchdogsRunning() <= 1UZ; })) << std::format("{} watchdogs are alive after {} restarts", scheduler.nWatchdogsRunning(), kCycles);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the final run did not stop";
    };
};

const boost::ut::suite<"a transparent subgraph in the execution order"> subgraphTerminationTests = [] {
    using namespace boost::ut;

    // graph::flatten puts the group's own block next to the children it contains, and Block::work reports
    // OK for every non-NormalBlock category without consulting anything, so the all-DONE condition was
    // unreachable and runAndWait() never returned
    "a transparent subgraph does not keep the scheduler running"_test = [] {
        gr::Graph flow;
        std::ignore = flow.emplaceBlock<qa_sched::DoneSource>();

        auto  wrapper = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto& sink    = wrapper->graph()->emplaceBlock<qa_sched::CountingSink>();

        const std::shared_ptr<gr::BlockModel>& subgraph = flow.addBlock(wrapper);
        subgraph->setName("inner");
        expect(wrapper->exportPort(true, std::string(sink.unique_name), gr::PortDirection::INPUT, "in", "in").has_value());
        expect(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, subgraph, gr::PortDefinition{"in"}).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::atomic<bool> running{true};
        std::thread       worker([&scheduler, &running] {
            std::ignore = scheduler.runAndWait();
            running.store(false);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(!running.load()) << "a finished chain inside a subgraph must still reach DONE";
        worker.join(); // the failure above is already reported; the suite timeout covers a true hang

        expect(eq(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };
};

const boost::ut::suite<"a subgraph's progress sequence"> subgraphProgressTests = [] {
    using namespace boost::ut;

    // A subgraph's blocks were initialized with the subgraph's own progress sequence while the scheduler
    // waits on the top-level one. Under singleThreadedBlocking the worker then parks on a sequence the work
    // inside the subgraph cannot move, and the stop that follows never releases it. Neither half triggers
    // it alone: the same graph stops in a millisecond under singleThreaded, and so does a flat chain under
    // singleThreadedBlocking.
    "a subgraph's blocks publish progress where the scheduler waits"_test = [] {
        gr::Graph flow;
        std::ignore = flow.emplaceBlock<qa_sched::BlockingSource>();

        auto  wrapper  = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        auto& interior = *wrapper->graph();
        auto& sink     = interior.emplaceBlock<qa_sched::CountingSink>();

        const std::shared_ptr<gr::BlockModel>& subgraph = flow.addBlock(wrapper);
        subgraph->setName("inner");
        expect(wrapper->exportPort(true, std::string(sink.unique_name), gr::PortDirection::INPUT, "in", "in").has_value());
        expect(flow.connect(flow.blocks()[0], gr::PortDefinition{"out"}, subgraph, gr::PortDefinition{"in"}).has_value());

        expect(std::addressof(interior.progress()) == std::addressof(flow.progress())) //
            << "a subgraph's blocks must publish to the sequence its parent's scheduler waits on";

        qa_sched::BlockingScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::atomic<bool> running{true};
        std::thread       worker([&scheduler, &running] {
            std::ignore = scheduler.runAndWait();
            running.store(false);
        });

        const auto started = std::chrono::steady_clock::now();
        while (sink._nReceived < qa_sched::kSamplesBeforeTerminal && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(sink._nReceived > 0UZ) << "the chain must run at all";

        expect(scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP).has_value());

        const auto stopRequested = std::chrono::steady_clock::now();
        while (running.load() && std::chrono::steady_clock::now() - stopRequested < std::chrono::seconds(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(!running.load()) << "the worker did not leave the progress wait after a stop";
        worker.join(); // the failure above is already reported; the suite timeout covers a true hang
    };
};

const boost::ut::suite<"the zero-progress park"> zeroProgressParkTests = [] {
    using namespace boost::ut;

    // under singleThreadedBlocking the worker parks on the progress sequence once it has seen more than
    // timeout_inactivity_count zero-progress traversals. Opening the gate moves no sequence, and the
    // watchdog's first tick sits beyond the run bound, so the run completes only if the park releases
    // on its own timeout. With timeout_inactivity_count 0 a park necessarily separates the source's
    // first, gated call from its second.
    "a parked scheduler resumes within its timeout without a progress notify"_test = [] {
        qa_sched::gSourceGate.store(false, std::memory_order_release);
        qa_sched::gGatedSourceCalls.store(0UZ, std::memory_order_release);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::GatedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler({{"timeout_ms", gr::Size_t(25)}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(10'000)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        const bool completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [] {
            for (std::size_t seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire); seen == 0UZ; seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire)) {
                qa_sched::gGatedSourceCalls.wait(seen);
            }
            qa_sched::gSourceGate.store(true, std::memory_order_release);
        });
        expect(completed) << "the parked worker never re-ran the source";
        expect(eq(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };

    // With timeout_ms 1 a park that nothing ends early lasts one millisecond. The test alternates two
    // trials in one run. Each starts at a call that the worker follows with a park. In the first, a
    // notify arrives kSettle into the park, and the trial times the source's next call. In the second,
    // the park is left alone, and the trial times from call to call. A notify that ends the park brings
    // the next call in a fraction of the undisturbed park.
    "a producer's notify ends the park"_test = [] {
        using Micros                       = std::chrono::duration<double, std::micro>;
        constexpr std::size_t kTrials      = 25UZ;
        constexpr std::size_t kMaxAttempts = 2000UZ;
        constexpr auto        kSettle      = std::chrono::microseconds(100); // the worker reaches its park well inside this

        qa_sched::gOfferedSamples.store(0UZ);
        qa_sched::gPublishedOffers.store(0UZ);
        qa_sched::gOfferedSourceCalls.store(0UZ);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::OfferedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler({{"timeout_ms", gr::Size_t(1)}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(10'000)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::vector<double> notifiedWakeUs;
        std::vector<double> undisturbedParkUs;
        std::size_t         nOffered = 0UZ;

        const bool completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            bool notifyTurn = true;
            for (std::size_t attempt = 0UZ; attempt < kMaxAttempts && (notifiedWakeUs.size() < kTrials || undisturbedParkUs.size() < kTrials); ++attempt) {
                const std::size_t nCalls = qa_sched::awaitOfferedSourceCallBeyond(qa_sched::gOfferedSourceCalls.load(std::memory_order_acquire));
                if (nCalls + 2UZ >= qa_sched::kMaxTimedCalls) {
                    break;
                }
                std::this_thread::sleep_for(kSettle);
                if (qa_sched::gOfferedSourceCalls.load(std::memory_order_acquire) != nCalls) {
                    continue; // another pass followed that call within kSettle, and no park came between them
                }
                if (notifyTurn) {
                    const auto notifiedAt = std::chrono::steady_clock::now();
                    qa_sched::gOfferedSamples.fetch_add(1UZ, std::memory_order_acq_rel);
                    ++nOffered;
                    source.progress->incrementAndGet();
                    source.progress->notify_all();
                    std::ignore       = qa_sched::awaitOfferedSourceCallBeyond(nCalls);
                    const auto wokeAt = qa_sched::gOfferedSourceCallTimes[nCalls];
                    if (wokeAt < notifiedAt) {
                        continue; // the park ended on its timeout before the notify
                    }
                    notifiedWakeUs.push_back(Micros(wokeAt - notifiedAt).count());
                } else {
                    std::ignore = qa_sched::awaitOfferedSourceCallBeyond(nCalls);
                    undisturbedParkUs.push_back(Micros(qa_sched::gOfferedSourceCallTimes[nCalls] - qa_sched::gOfferedSourceCallTimes[nCalls - 1UZ]).count());
                }
                notifyTurn = !notifyTurn;
            }
            std::ignore = qa_sched::awaitCondition([&nOffered] { return qa_sched::gPublishedOffers.load(std::memory_order_relaxed) == nOffered; });
            scheduler.requestStop();
        });

        const double notifiedMedianUs    = qa_sched::median(notifiedWakeUs);
        const double undisturbedMedianUs = qa_sched::median(undisturbedParkUs);
        std::println("notify to next call: median {:.0f} us over {} trials; undisturbed park: median {:.0f} us over {} trials", notifiedMedianUs, notifiedWakeUs.size(), undisturbedMedianUs, undisturbedParkUs.size());

        expect(completed) << "the run did not end after its trials";
        expect(ge(notifiedWakeUs.size(), kTrials)) << "too few notifies reached a parked worker";
        expect(ge(undisturbedParkUs.size(), kTrials)) << "too few parks ran undisturbed";
        expect(eq(qa_sched::gPublishedOffers.load(), nOffered)) << "an offered sample was not published";
        expect(lt(notifiedMedianUs, undisturbedMedianUs / 2.0)) << std::format("the worker resumed {:.0f} us after a notify, and an undisturbed park lasted {:.0f} us", notifiedMedianUs, undisturbedMedianUs);
    };
};

const boost::ut::suite<"a job list that finishes before the others"> upstreamReleaseTests = [] {
    using namespace boost::ut;

    // A worker leaves as soon as every block of its job list reports DONE, and under multiThreaded a job
    // list may hold a single block, so the block that ends the stream gets no further work() call. An
    // upstream source learns that its last consumer is gone only from the reader counts of its output
    // ports, which the finished block lowers by releasing its input.
    "the finished consumer releases its upstream source"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StoppingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "the source kept running after its only consumer finished";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };

    // the same shutdown for a source paced by wall-clock time rather than by back-pressure: it never
    // reports DONE of its own accord, so the released input port is the only thing that ends the run
    "a wall-clock paced source stops with its consumer"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::PacedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StoppingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "the paced source kept running after its only consumer finished";
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };
};

int main() { /* tests are statically registered */ }
