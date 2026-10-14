#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/BlockingSync.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

#include "build_configure.hpp"
#include "plugins/cross_object_scheduler.hpp"

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

// takes int16 samples. An edge recorded by port names from a float output reaches this input and cannot connect.
struct Int16Sink : gr::Block<Int16Sink> {
    gr::PortIn<std::int16_t> in;

    GR_MAKE_REFLECTABLE(Int16Sink, in);

    std::size_t _nReceived = 0UZ;

    void processOne(std::int16_t) { _nReceived++; }
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

// consumes none of the items on its asynchronous input. Once the stream ends behind the items queued there, the block
// drains, and only the bound on its calls in which nothing moved ends it
struct UntakenRemainderRelay : gr::Block<UntakenRemainderRelay> {
    gr::PortIn<float, gr::Async>  in;
    gr::PortOut<float, gr::Async> out;

    GR_MAKE_REFLECTABLE(UntakenRemainderRelay, in, out);

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        _nCalls++;
        std::ignore = inSpan.consume(0UZ);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
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

// publishes nothing and returns ERROR once its gate is open
struct GatedFailingSource : gr::Block<GatedFailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GatedFailingSource, out);

    const std::atomic<bool>* _gate = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        return _gate != nullptr && _gate->load(std::memory_order_acquire) ? gr::work::Status::ERROR : gr::work::Status::OK;
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

constexpr std::size_t kEndingSamples = 16UZ;

// Publishes kEndingSamples on its first call and nothing on its second. Its third call publishes kEndingSamples more
// and returns DONE.
struct EndingSource : gr::Block<EndingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndingSource, out);

    std::size_t _nCalls = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        ++_nCalls;
        if (_nCalls == 2UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        const std::size_t nPublish = std::min(outSpan.size(), kEndingSamples);
        for (std::size_t i = 0UZ; i < nPublish; ++i) {
            outSpan[i] = 1.0f;
        }
        outSpan.publish(nPublish);
        return _nCalls == 1UZ ? gr::work::Status::OK : gr::work::Status::DONE;
    }
};

constexpr std::size_t kIdleRecords = 8UZ;

// Publishes nothing. Once `_finished` reads STOPPED, it records the progress sequence at each of its next
// kIdleRecords calls.
struct IdleSource : gr::Block<IdleSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(IdleSource, out);

    const CountingSink*      _finished = nullptr;
    std::vector<std::size_t> _progressSeen;
    std::atomic<std::size_t> _nRecorded{0UZ};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        if (_finished != nullptr && _finished->state() == gr::lifecycle::State::STOPPED && _progressSeen.size() < kIdleRecords) {
            _progressSeen.push_back(this->progress->value());
            _nRecorded.store(_progressSeen.size(), std::memory_order_release);
        }
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

struct EndlessSource : gr::Block<EndlessSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

// publishes one sample for each release and nothing in between. Its graph makes no progress until the test releases a sample.
struct ReleasedSource : gr::Block<ReleasedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ReleasedSource, out);

    std::atomic<std::size_t>* _nReleased = nullptr;
    std::atomic<std::size_t>  _nWorkCalls{0UZ};

    // counts every call, the calls that publish nothing included
    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        _nWorkCalls.fetch_add(1UZ, std::memory_order_relaxed);
        return gr::Block<ReleasedSource>::work(requestedWork);
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ || _nReleased == nullptr || _nReleased->load(std::memory_order_acquire) == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        _nReleased->fetch_sub(1UZ, std::memory_order_acq_rel);
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
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

// a sub-scheduler that counts the work() calls of the scheduler that runs it once each call has returned
template<typename TScheduler>
struct CountedSchedulerWrapper : gr::SchedulerWrapper<TScheduler> {
    std::atomic<std::size_t> _nReturnedWorkCalls{0UZ};

    [[nodiscard]] gr::work::Result work(std::size_t requestedWork = gr::undefined_size) override {
        const gr::work::Result result = gr::SchedulerWrapper<TScheduler>::work(requestedWork);
        _nReturnedWorkCalls.fetch_add(1UZ, std::memory_order_release);
        return result;
    }
};

using CountedSubScheduler = CountedSchedulerWrapper<SerialScheduler>;

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

// runs a test-supplied action in its stop() hook, on the thread that requested the stop
struct StopActionSink : gr::Block<StopActionSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(StopActionSink, in);

    std::function<void()> _onStop;
    int                   _nStopCalls = 0;

    void stop() {
        if (_onStop) {
            _onStop();
        }
        _nStopCalls++;
    }

    void processOne(float) { gObservedSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// samples a parent scheduler's own graph has moved, observed from outside its threads
inline std::atomic<std::size_t> gParentSamples{0UZ};

struct ParentCountingSink : gr::Block<ParentCountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(ParentCountingSink, in);

    void processOne(float) { gParentSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// adoptBlock is the scheduler's entry point for a block added to an already running graph. A message handler calls it
// on a worker inside processScheduledMessages(), which also forwards the children's messages to msgOut. The test calls
// it from its own thread and reports a refused adoption on msgOut under "adoptBlock", as a reply carries it. The
// wrapper holds the same flag as processScheduledMessages(). That report and a worker's forwarding then never use
// msgOut's single writer at once
struct AdoptingScheduler : TestScheduler {
    using TestScheduler::TestScheduler;

    void adoptBlock(const std::shared_ptr<gr::BlockModel>& newBlock) {
        while (std::atomic_flag_test_and_set_explicit(&this->_processingScheduledMessages, std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (auto adopted = TestScheduler::adoptBlock(newBlock); !adopted) {
            this->emitErrorMessage("adoptBlock", adopted.error());
        }
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

        while (gr::lifecycle::isActive(this->state()) && gr::atomic_ref(this->_run.generation).load_acquire() == generation) {
            const std::optional<gr::work::Result> result = this->traverseBlockListAsWork(localBlockList);
            if (result.has_value() && (result->status == gr::work::Status::DONE || result->status == gr::work::Status::ERROR)) {
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

    // the watchdogs asleep in their period. A watchdog sleeps on the generation sequence that a retirement advances.
    // Nothing else waits on that sequence. A watchdog counts as running from the scheduler's start() on. The pool
    // starts it when one of its threads takes the task. One that starts after its run has ended returns without
    // sleeping.
    [[nodiscard]] std::size_t nSleepingWatchdogs() const { return this->_watchdogGeneration.nTimedWaiters(); }

    // retires the watchdogs and returns once each has returned
    void retireWatchdogs() { TestScheduler::retireWatchdogs(); }
};

// reads the scheduler's wake count. With no message sent and no lifecycle change, only the watchdog advances it: once
// per period in which the graph makes no progress
template<typename TScheduler>
struct WakeProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] std::size_t nWakes() const { return this->_wake->value(); }
};

// reads the pool the scheduler dispatches its workers on
template<typename TScheduler>
struct PoolProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] std::string poolInUse() const { return std::string(this->_pool->name()); }
};

// takes every message waiting on a port and counts the watchdog's stall reports among them
struct StallReports {
    std::size_t count       = 0UZ;
    std::size_t lastPeriods = 0UZ; // stalled periods the latest report names
    std::string serviceName{};     // when set, only the reports of the scheduler of this name count

    void take(gr::MsgPortIn& port) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (message.endpoint == "watchdog" && message.cmd == gr::message::Command::Notify && message.data.has_value() && (serviceName.empty() || message.serviceName == serviceName)) {
                ++count;
                lastPeriods = message.data->at("stalled_periods").value_or<gr::Size_t>(0U);
            }
        }
        std::ignore = messages.consume(messages.size());
    }
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

// a single-threaded sub-scheduler whose source returns ERROR once the gate is open
struct GatedSubScheduler {
    std::shared_ptr<CountedSubScheduler> scheduler;
    std::string                          failingName; // the unique name of the source that fails
};

[[nodiscard]] GatedSubScheduler makeGatedSubScheduler(const std::atomic<bool>& gate) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<GatedFailingSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    source._gate = &gate;
    GatedSubScheduler gated{std::make_shared<CountedSubScheduler>(), std::string(source.unique_name)};
    gated.scheduler->setGraph(std::move(flow));
    return gated;
}

// the parent's run ended within its bound in ERROR, with an error that names the sub-scheduler and its failing source
void expectFailedBySubScheduler(const TestScheduler& parent, bool ended, const std::expected<void, gr::Error>& result, std::string_view innerName, std::string_view failingName, gr::lifecycle::State innerState) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    expect(ended) << "the parent ran on after its sub-scheduler failed";
    expect(!result.has_value()) << "a sub-scheduler that failed while it ran must fail its parent's run";
    if (!result.has_value()) {
        expect(result.error().message.find(innerName) != std::string::npos) << "the error must name the sub-scheduler: " << result.error().message;
        expect(result.error().message.find(failingName) != std::string::npos) << "the error must carry the sub-scheduler's reason: " << result.error().message;
    }
    expect(parent.state() == ERROR) << "the parent's run must end in ERROR";
    expect(innerState == ERROR) << "the sub-scheduler's run must end in ERROR";
}

void expectFailedBySubScheduler(const TestScheduler& parent, bool ended, const std::expected<void, gr::Error>& result, std::string_view innerName, const GatedSubScheduler& inner) { expectFailedBySubScheduler(parent, ended, result, innerName, inner.failingName, inner.scheduler->blockRef().state()); }

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
// instead of hanging ctest; `duringStartup` runs on the caller's thread the moment the runner exists. A given
// `result` receives what runAndWait() returned
template<typename TScheduler, typename TDuringStartup>
[[nodiscard]] bool runAndWaitWithin(TScheduler& scheduler, std::chrono::milliseconds bound, TDuringStartup duringStartup, std::expected<void, gr::Error>* result = nullptr) {
    std::mutex              mutex;
    std::condition_variable finished;
    bool                    returned = false;

    std::thread runner([&scheduler, &mutex, &finished, &returned, result] {
        std::expected<void, gr::Error> outcome = scheduler.runAndWait();
        if (result != nullptr) {
            *result = std::move(outcome);
        }
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

// stops the scheduler of a finite graph through `stopBeforeEntry` and then calls runAndWait(). A stop completed before
// runAndWait() is entered belongs to no run, and runAndWait() runs the graph to its end.
template<typename TStop>
void expectRunAfterStopBeforeEntry(TStop stopBeforeEntry) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<DoneSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    BlockingScheduler scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    stopBeforeEntry(scheduler);

    std::expected<void, gr::Error> result;
    expect(runAndWaitWithin(scheduler, kEventBound, [] {}, &result)) << "runAndWait() did not finish the finite graph";
    expect(result.has_value()) << "the run after the stop must succeed";
    expect(eq(sink._nReceived, kSamplesBeforeTerminal)) << "a stop completed before runAndWait() was entered must not cancel its run";
    expect(scheduler.state() == STOPPED) << "the run must end STOPPED";
}

constexpr std::string_view kOccupiedPoolName = "qa_occupied_cpu";
constexpr std::string_view kAdoptionPoolName = "qa_adoption_cpu";

// a pool that the manager holds under its name while the case runs. At the end of the case the name resolves to the
// default CPU pool, and the pool's threads end once no scheduler holds the pool. A WebAssembly build runs a fixed number
// of threads in the whole program. A case therefore keeps its threads only while it runs.
template<typename TPool>
struct NamedPool {
    std::string            name;
    std::shared_ptr<TPool> pool;

    NamedPool(std::string_view poolName, std::shared_ptr<TPool> namedPool) : name(poolName), pool(std::move(namedPool)) { gr::thread_pool::Manager::instance().replacePool(name, pool); }
    NamedPool(const NamedPool&)            = delete;
    NamedPool& operator=(const NamedPool&) = delete;
    ~NamedPool() { gr::thread_pool::Manager::instance().replacePool(name, gr::thread_pool::Manager::defaultCpuPool()); }

    TPool& operator*() const { return *pool; }
    TPool* operator->() const { return pool.get(); }
};

// a pool of fixed size, held under its name while the case runs
[[nodiscard]] NamedPool<gr::thread_pool::TaskExecutor> fixedPool(std::string_view name, std::uint32_t nThreads) { return {name, std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(name, gr::thread_pool::TaskType::CPU_BOUND, nThreads, nThreads), "CPU")}; }

[[nodiscard]] NamedPool<gr::thread_pool::TaskExecutor> twoThreadPool() { return fixedPool(kOccupiedPoolName, 2U); }

using PoolParkingScheduler    = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreadedBlocking>;
using PoolParkingBreadthFirst = gr::scheduler::BreadthFirst<gr::scheduler::ExecutionPolicy::multiThreadedBlocking>;
using PoolParkingDepthFirst   = gr::scheduler::DepthFirst<gr::scheduler::ExecutionPolicy::multiThreadedBlocking>;

constexpr std::string_view kParkingPoolName = "qa_parking_cpu";
constexpr std::size_t      kFiniteSamples   = 1UZ << 16;

// The chain and release cases' sources wait inside work() for the test's signal. Their worker therefore never parks.
// The test signals them without advancing the progress sequence. The first wake that a parked worker can receive comes
// from the source's publication.

// waits inside work() until `_open` reads true, then publishes the values 0 to kFiniteSamples - 1 and ends the stream
struct GatedFiniteSource : gr::Block<GatedFiniteSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GatedFiniteSource, out);

    const std::atomic<bool>* _open     = nullptr;
    std::size_t              _nEmitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_open == nullptr || !awaitCondition([this] { return _open->load(std::memory_order_acquire); })) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        if (_nEmitted >= kFiniteSamples) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t nPublish = std::min(outSpan.size(), kFiniteSamples - _nEmitted);
        for (std::size_t i = 0UZ; i < nPublish; ++i) {
            outSpan[i] = static_cast<float>(_nEmitted + i);
        }
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return gr::work::Status::OK;
    }
};

// waits inside work() until a release arrives or `_holding` reads false, and publishes one sample for each release
struct HeldSource : gr::Block<HeldSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldSource, out);

    std::atomic<std::size_t>* _nReleased = nullptr;
    const std::atomic<bool>*  _holding   = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const bool released = _nReleased != nullptr && _holding != nullptr && awaitCondition([this] { return _nReleased->load(std::memory_order_acquire) > 0UZ || !_holding->load(std::memory_order_acquire); });
        if (outSpan.size() == 0UZ || !released || _nReleased->load(std::memory_order_acquire) == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        _nReleased->fetch_sub(1UZ, std::memory_order_acq_rel);
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// counts the work() calls of its worker, the calls that find no sample included
struct PassThrough : gr::Block<PassThrough> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PassThrough, in, out);

    std::atomic<std::size_t> _nWorkCalls{0UZ};

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        _nWorkCalls.fetch_add(1UZ, std::memory_order_relaxed);
        return gr::Block<PassThrough>::work(requestedWork);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

// counts the work() calls of its worker, the calls that find no sample included
struct CallCountingSink : gr::Block<CallCountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CallCountingSink, in);

    std::atomic<std::size_t> _nWorkCalls{0UZ};

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        _nWorkCalls.fetch_add(1UZ, std::memory_order_relaxed);
        return gr::Block<CallCountingSink>::work(requestedWork);
    }

    void processOne(float) {}
};

// counts the samples that arrive in the order GatedFiniteSource numbers them
struct OrderedSink : gr::Block<OrderedSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(OrderedSink, in);

    std::size_t _nInOrder = 0UZ;

    void processOne(float value) {
        if (value == static_cast<float>(_nInOrder)) {
            ++_nInOrder;
        }
    }
};

// records when work() first calls it and publishes nothing. Its output needs no reader.
struct FirstCallSource : gr::Block<FirstCallSource> {
    gr::PortOut<float, gr::Async, gr::Optional> out;

    GR_MAKE_REFLECTABLE(FirstCallSource, out);

    std::atomic<bool>                     _called{false};
    std::chrono::steady_clock::time_point _firstCallAt{};

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_called.load(std::memory_order_relaxed)) {
            _firstCallAt = std::chrono::steady_clock::now();
            _called.store(true, std::memory_order_release);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

// true once `count` threads wait on the graph's progress sequence
template<typename TScheduler>
[[nodiscard]] bool awaitParkedWorkers(const TScheduler& scheduler, std::size_t count) {
    return awaitCondition([&scheduler, count] { return scheduler.graph().progress().nTimedWaiters() == count; });
}

// reads the scheduler's wake count and whether a work quiescence is requested. The pool cases also read the count of
// workers that have not left their loop, and hand a block to a job list from the test's thread
template<typename TScheduler>
struct ParkingProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] const gr::Sequence* wakeSequence() const { return this->_wake.get(); }

    [[nodiscard]] bool workQuiescenceRequested() { return gr::atomic_ref(this->_nWorkQuiescenceRequests).load_acquire() > 0UZ; }

    [[nodiscard]] std::size_t nRunningJobs() const { return this->_nRunningJobs->value(); }

    // adds a block to a job list of the running graph, as an EmplaceBlock message does once the block is emplaced
    void adopt(const std::shared_ptr<gr::BlockModel>& block) {
        const auto adopted = this->adoptBlock(block);
        boost::ut::expect(adopted.has_value() && adopted->has_value()) << std::format("'{}' joined no job list of the running graph", block->uniqueName());
    }

    // true once every worker has taken the blocks queued for its job list
    [[nodiscard]] bool adoptionQueuesEmpty() {
        std::lock_guard guard(this->_adoptionBlocksMutex);
        return std::ranges::all_of(this->_adoptionBlocks, [](const auto& queued) { return queued.empty(); });
    }

    // queues a block for job list `runnerID` and closes that list, as the list's worker does when it leaves its loop
    // with the block still queued
    void queueAndClose(std::size_t runnerID, const std::shared_ptr<gr::BlockModel>& block) {
        {
            std::lock_guard guard(this->_adoptionBlocksMutex);
            this->_adoptionBlocks.at(runnerID).push_back(block);
        }
        this->closeAdoptionList(runnerID);
    }
};

using BlockingProbe    = ParkingProbe<BlockingScheduler>;
using PoolParkingProbe = ParkingProbe<PoolParkingScheduler>;

// publishes a message into msgIn from a thread exempt from the scheduler's wake, as a worker's thread publishes. No
// parked worker returns on that publication. The calling thread then runs the scheduler's message service until msgIn
// is empty, as a parent scheduler's worker runs a nested scheduler's. That thread runs none of the run's blocks
template<typename TProbe, typename TSend>
void serveOnCallingThread(TProbe& scheduler, TSend send) {
    const gr::Sequence* previousExempt = std::exchange(gr::detail::publishWakeExempt(), scheduler.wakeSequence());
    send();
    gr::detail::publishWakeExempt() = previousExempt;
    std::ignore                     = awaitCondition([&scheduler] {
        scheduler.processScheduledMessages();
        return scheduler.msgIn.streamReader().available() == 0UZ;
    });
}

// records when its msgIn first delivers a message with the endpoint "stamp"
struct MessageStampSink : gr::Block<MessageStampSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(MessageStampSink, in);

    std::atomic<bool>                     _stamped{false};
    std::chrono::steady_clock::time_point _stampedAt{};

    void processOne(float) {}

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message> messages) {
        if (!_stamped.load(std::memory_order_relaxed) && std::ranges::any_of(messages, [](const gr::Message& message) { return message.endpoint == "stamp"; })) {
            _stampedAt = std::chrono::steady_clock::now();
            _stamped.store(true, std::memory_order_release);
        }
    }
};

// records when a parent's worker first calls its work(). Its own workers call its blocks and never call it
struct CalledSubScheduler : gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> {
    using Base = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;
    using Base::Base;

    std::atomic<bool>                     _called{false};
    std::chrono::steady_clock::time_point _firstCallAt{};

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        if (!_called.load(std::memory_order_relaxed)) {
            _firstCallAt = std::chrono::steady_clock::now();
            _called.store(true, std::memory_order_release);
        }
        return Base::work(requestedWork);
    }
};

// a blocking source whose first work() call waits for a release. With `_releaseInPause` its pause() hook releases the
// call and waits up to kSweepParkBound for a worker to park on `_progress`. The worker's passes after the release then
// fall inside the pause's sweep, before the scheduler publishes PAUSED. Without it the call returns once
// `_pauseWaits()` holds, while pause() waits for the work() calls in progress. The call then spans the start of the
// pause
struct PauseProbeSource : gr::Block<PauseProbeSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PauseProbeSource, out);

    static constexpr auto kSweepParkBound = std::chrono::milliseconds(100);

    const gr::Sequence*   _progress       = nullptr;
    bool                  _releaseInPause = true;
    std::function<bool()> _pauseWaits;
    std::atomic<bool>     _inside{false};
    std::atomic<bool>     _released{false};
    std::atomic<bool>     _parkedInSweep{false};

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return true; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_released.load(std::memory_order_acquire)) {
            _inside.store(true, std::memory_order_release);
            std::ignore = awaitCondition([this] { return _released.load(std::memory_order_acquire) || (_pauseWaits && _pauseWaits()); });
        }
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }

    void release() { _released.store(true, std::memory_order_release); }

    void pause() {
        if (!_releaseInPause) {
            return;
        }
        release();
        const bool parked = _progress != nullptr && awaitCondition([this] { return _progress->nTimedWaiters() > 0U; }, kSweepParkBound);
        _parkedInSweep.store(parked, std::memory_order_release);
    }
};

// a sink that reports itself blocking. While a worker of its run is inside the scheduler's loop, a removal or a
// replacement leaves the sink in REQUESTED_STOP until its own next work() call moves it to STOPPED
struct BlockingSink : gr::Block<BlockingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(BlockingSink, in);

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return true; }

    void processOne(float) {}
};

// true once a message to `endpoint` arrives within `bound`. Every message read is consumed.
[[nodiscard]] bool awaitReply(gr::MsgPortIn& port, std::string_view endpoint, std::chrono::milliseconds bound) {
    return awaitCondition(
        [&port, endpoint] {
            auto       messages = port.streamReader().get();
            const bool found    = std::ranges::any_of(messages, [endpoint](const gr::Message& message) { return message.endpoint == endpoint; });
            std::ignore         = messages.consume(messages.size());
            return found;
        },
        bound);
}

constexpr std::string_view kDeferringPoolName = "qa_deferring_cpu";

// a growable pool whose execute() returns before a thread takes the task. It holds every task until release() hands
// the held tasks to its threads, and it counts a held task as queued
struct DeferringPool : gr::thread_pool::TaskExecutor {
    std::unique_ptr<gr::thread_pool::BasicThreadPool>        _threads;
    mutable std::mutex                                       _mutex;
    std::vector<gr::thread_pool::detail::move_only_function> _held;
    bool                                                     _released = false;

    explicit DeferringPool(std::uint32_t maxThreads) : _threads(std::make_unique<gr::thread_pool::BasicThreadPool>(kDeferringPoolName, gr::thread_pool::TaskType::CPU_BOUND, 1U, maxThreads)) {}

    void execute(gr::thread_pool::detail::move_only_function&& task) override {
        std::unique_lock lock(_mutex);
        if (!_released) {
            _held.push_back(std::move(task));
            return;
        }
        lock.unlock();
        _threads->execute(std::move(task));
    }

    void release() {
        std::vector<gr::thread_pool::detail::move_only_function> held;
        {
            std::lock_guard lock(_mutex);
            _released = true;
            held.swap(_held);
        }
        for (auto& task : held) {
            _threads->execute(std::move(task));
        }
    }

    [[nodiscard]] gr::thread_pool::TaskType type() const noexcept override { return _threads->poolType(); }
    [[nodiscard]] std::string_view          name() const noexcept override { return _threads->poolName(); }
    [[nodiscard]] std::string_view          device() const noexcept override { return "CPU"; }
    [[nodiscard]] std::size_t               numThreads() const override { return _threads->numThreads(); }
    [[nodiscard]] std::size_t               numTasksQueued() const override {
        std::lock_guard lock(_mutex);
        return _held.size() + _threads->numTasksQueued();
    }
    [[nodiscard]] std::size_t                   numTasksRunning() const override { return _threads->numTasksRunning(); }
    [[nodiscard]] std::size_t                   numTasksRecycled() const override { return _threads->numTasksRecycled(); }
    void                                        setThreadBounds(std::uint32_t min, std::uint32_t max) override { _threads->setThreadBounds(min, max); }
    [[nodiscard]] std::pair<uint32_t, uint32_t> threadBounds() const override { return {_threads->minThreads(), _threads->maxThreads()}; }
    [[nodiscard]] std::uint32_t                 minThreads() const override { return _threads->minThreads(); }
    [[nodiscard]] std::uint32_t                 maxThreads() const override { return _threads->maxThreads(); }
    void                                        requestShutdown() override { _threads->requestShutdown(); }
    [[nodiscard]] bool                          isShutdown() const override { return _threads->isShutdown(); }
};

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

// set by a failing block when its work() call starts. The test requests the stop once it reads true
inline std::atomic<bool> gInsideWork{false};
// the lifecycle state a failing block read when it failed. IDLE means the block has not failed
inline std::atomic<gr::lifecycle::State> gStateAtFailure{gr::lifecycle::State::IDLE};
// a failing block holds its failure while this reads false
inline std::atomic<bool> gFailureReleased{true};

constexpr std::string_view kFlushFailure = "the device could not flush its last buffer";
constexpr std::string_view kReadFailure  = "the device could not deliver its last sample";
constexpr std::string_view kRunFailure   = "the device stopped delivering samples";

// the run generation of the scheduler under test, which a stop advances before it reaches any block
inline std::function<std::size_t()> gRunGeneration;

template<typename TScheduler>
struct GenerationProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] std::size_t generation() { return gr::atomic_ref(this->_run.generation).load_acquire(); }
};

// waits inside work() until the stop reaches the block, records the block's state, and then holds until the test
// releases the failure. A blocking block's stop() hook moves it to REQUESTED_STOP during the call. A block that does not
// block sees the stop begin as a new run generation, and its stop() hook runs once the call has returned. A stop that
// never arrives leaves the recorded state at IDLE and the case fails on it
inline void awaitStopInsideWork(const auto& block, bool blocking) {
    gInsideWork.store(true, std::memory_order_release);
    const std::size_t run = gRunGeneration();
    if (awaitCondition([&block, blocking, run] { return blocking ? block.state() == gr::lifecycle::State::REQUESTED_STOP : gRunGeneration() != run; })) {
        gStateAtFailure.store(block.state(), std::memory_order_release);
    }
    std::ignore = awaitCondition([] { return gFailureReleased.load(std::memory_order_acquire); });
}

// a device that cannot flush on its way down. The processBulk() call that the stop reaches reports the failure and
// returns ERROR. A blocking source reads REQUESTED_STOP at that point, a non-blocking source RUNNING
template<bool kBlocking>
struct FlushFailingSource : gr::Block<FlushFailingSource<kBlocking>> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FlushFailingSource, out);

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return kBlocking; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        awaitStopInsideWork(*this, kBlocking);
        this->emitErrorMessage("processBulk", kFlushFailure);
        return gr::work::Status::ERROR;
    }
};

// a device that fails while the graph runs. The first processBulk() call reports the failure and returns ERROR
struct RunningFailingSource : gr::Block<RunningFailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(RunningFailingSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        gStateAtFailure.store(this->state(), std::memory_order_release);
        this->emitErrorMessage("processBulk", kRunFailure);
        return gr::work::Status::ERROR;
    }
};

// a device that fails and requests its own stop in the same processBulk() call
struct SelfStoppingFailingSource : gr::Block<SelfStoppingFailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SelfStoppingFailingSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        outSpan.publish(0UZ);
        this->requestStop();
        gStateAtFailure.store(this->state(), std::memory_order_release);
        this->emitErrorMessage("processBulk", kRunFailure);
        return gr::work::Status::ERROR;
    }
};

// a device whose last sample cannot be read. The processOne() call that the stop reaches throws
struct ReadFailingSource : gr::Block<ReadFailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ReadFailingSource, out);

    [[nodiscard]] float processOne() const {
        awaitStopInsideWork(*this, false);
        throw gr::exception(kReadFailure);
    }
};

struct StopInsideWorkResult {
    bool                           entered = false; // a block entered its work() call before the stop
    std::expected<void, gr::Error> outcome;
};

// runs the graph on its own thread and requests the stop once a block is inside its work() call
template<typename TScheduler>
[[nodiscard]] StopInsideWorkResult runAndStopInsideWork(GenerationProbe<TScheduler>& scheduler) {
    gRunGeneration = [&scheduler] { return scheduler.generation(); };
    gInsideWork.store(false, std::memory_order_release);
    gStateAtFailure.store(gr::lifecycle::State::IDLE, std::memory_order_release);
    std::expected<void, gr::Error> outcome;
    std::thread                    runner([&scheduler, &outcome] { outcome = scheduler.runAndWait(); });
    const bool                     entered = awaitCondition([] { return gInsideWork.load(std::memory_order_acquire); });
    std::ignore                            = scheduler.changeStateTo(gr::lifecycle::State::REQUESTED_STOP);
    runner.join();
    return {entered, outcome};
}

// the error messages taken from a port, as pairs of sender and text
struct ErrorReports {
    std::vector<std::pair<std::string, std::string>> errors;

    void take(gr::MsgPortIn& port) {
        auto messages = port.streamReader().get();
        for (const gr::Message& message : messages) {
            if (!message.data.has_value()) {
                errors.emplace_back(message.serviceName, message.data.error().message);
            }
        }
        std::ignore = messages.consume(messages.size());
    }

    [[nodiscard]] bool has(std::string_view sender, std::string_view text) const {
        return std::ranges::any_of(errors, [sender, text](const auto& error) { return error.first == sender && error.second.find(text) != std::string::npos; });
    }
};

// a source that fails after the stop reached it, feeding a counting sink, under scheduler TScheduler
template<typename TScheduler, typename TSource>
void expectErrorAfterStopFailsRun(gr::lifecycle::State expectedStateAtFailure, std::string_view failure) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<TSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    GenerationProbe<TScheduler> scheduler;
    gr::MsgPortIn               fromScheduler;
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());

    const auto [entered, outcome] = runAndStopInsideWork<TScheduler>(scheduler);
    expect(entered) << "the source never entered its work() call";
    expect(gStateAtFailure.load(std::memory_order_acquire) == expectedStateAtFailure) << "the stop had not reached the source when it failed";
    expect(!outcome.has_value()) << "a run whose block failed while stopping is reported as a clean stop";
    if (!outcome.has_value()) {
        expect(outcome.error().message.find(failure) != std::string::npos) << "the run's error carries what the block reported";
    }
    expect(scheduler.state() == STOPPED) << "the stop's STOPPED stays the scheduler's state";
    ErrorReports reports;
    reports.take(fromScheduler);
    expect(reports.has(source.unique_name, failure)) << "the scheduler's message port carries the block's error";
    expect(reports.has(scheduler.unique_name, source.unique_name)) << "the scheduler reports the block that failed after the stop";
    expect(source.state() == STOPPED) << "the failing block completes the stop it was asked for";
}

// a source that fails before any stop, feeding a counting sink, under scheduler TScheduler
template<typename TScheduler, typename TSource>
void expectErrorWhileRunningEndsRun(gr::lifecycle::State expectedStateAtFailure) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<TSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    TScheduler    scheduler;
    gr::MsgPortIn fromScheduler;
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());

    gStateAtFailure.store(IDLE, std::memory_order_release);
    const std::expected<void, gr::Error> outcome = scheduler.runAndWait();
    expect(gStateAtFailure.load(std::memory_order_acquire) == expectedStateAtFailure) << "the source failed in another state";
    expect(!outcome.has_value()) << "a run whose block failed is reported as a success";
    if (!outcome.has_value()) {
        expect(outcome.error().message.find(kRunFailure) != std::string::npos) << "the run's error carries what the block reported";
    }
    expect(scheduler.state() == ERROR) << "the scheduler finishes in ERROR";
    ErrorReports reports;
    reports.take(fromScheduler);
    expect(reports.has(source.unique_name, kRunFailure)) << "the scheduler's message port carries the block's error";
    expect(source.state() == STOPPED) << "the failing block ends STOPPED";
    expect(eq(sink._nStopCalls, 1)) << "the scheduler stops the blocks torn down alongside the failing one";
}

// sends 64 notifications on its message port for every call that receives samples, which fills an undrained msgOut fast
struct NotifyingSink : gr::Block<NotifyingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(NotifyingSink, in);

    std::atomic<std::size_t> _nReceived{0UZ};

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        _nReceived.fetch_add(inSpan.size(), std::memory_order_relaxed);
        for (std::size_t i = 0UZ; i < 64UZ; ++i) {
            this->emitMessage("tick", {});
        }
        return gr::work::Status::OK;
    }
};

#ifdef GR_TEST_WITH_BLOCK_LIBRARY
constexpr std::string_view kLibrarySource    = "test::library_silent_source";
constexpr std::string_view kLibrarySink      = "test::library_quiet_sink";
constexpr std::string_view kLibraryScheduler = "test::cross_object_scheduler";

// loads the test's shared object of blocks and a scheduler once. The loader keeps an object that registered entries
// mapped after the loader is gone
[[nodiscard]] inline bool loadCrossObjectLibrary() {
    static const bool loaded = [] {
        const std::vector<std::string> directories{std::string(TESTS_BINARY_PATH) + "/cross_object_library"};
        gr::PluginLoader               loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), directories);
        return gr::globalBlockRegistry().contains(kLibrarySource) && gr::globalBlockRegistry().contains(kLibrarySink) && gr::globalSchedulerRegistry().contains(kLibraryScheduler);
    }();
    return loaded;
}

// work() calls and start() calls of ThreadMarkProbe, and the answers it got
inline std::atomic<std::size_t> gMarkWorkCalls{0UZ};
inline std::atomic<std::size_t> gMarkOwnWorkerCalls{0UZ};
inline std::atomic<std::size_t> gMarkStarts{0UZ};
inline std::atomic<bool>        gMarkFirstStartAdmitted{false};
inline std::atomic<bool>        gMarkRestartAdmitted{false};

// asks the scheduler that runs it, with this program's code, whether the calling thread is the scheduler's worker,
// and whether the scheduler admits a graph swap from the thread that starts the blocks
struct ThreadMarkProbe : gr::Block<ThreadMarkProbe> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ThreadMarkProbe, out);

    gr::testing::CrossObjectScheduler* _scheduler = nullptr;

    void start() {
        const bool admitted = _scheduler->swapAllowedFromThisThread().has_value();
        (gMarkStarts.fetch_add(1UZ) == 0UZ ? gMarkFirstStartAdmitted : gMarkRestartAdmitted).store(admitted);
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_scheduler->isOnOwnWorkerThread()) {
            gMarkOwnWorkerCalls.fetch_add(1UZ);
        }
        gMarkWorkCalls.fetch_add(1UZ);
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};
#endif

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

const boost::ut::suite<"a block error while the scheduler stops"> errorWhileStoppingTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a processBulk() ERROR of a non-blocking block in the call the stop waits for fails the run"_test = [] {
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::SerialScheduler, qa_sched::FlushFailingSource<false>>(RUNNING, qa_sched::kFlushFailure);
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::BlockingScheduler, qa_sched::FlushFailingSource<false>>(RUNNING, qa_sched::kFlushFailure);
    };

    "a processBulk() ERROR after the stop reached a blocking block fails the run"_test = [] {
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::SerialScheduler, qa_sched::FlushFailingSource<true>>(REQUESTED_STOP, qa_sched::kFlushFailure);
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::BlockingScheduler, qa_sched::FlushFailingSource<true>>(REQUESTED_STOP, qa_sched::kFlushFailure);
    };

    "a processOne() that throws in the call the stop waits for fails the run"_test = [] {
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::SerialScheduler, qa_sched::ReadFailingSource>(RUNNING, qa_sched::kReadFailure);
        qa_sched::expectErrorAfterStopFailsRun<qa_sched::BlockingScheduler, qa_sched::ReadFailingSource>(RUNNING, qa_sched::kReadFailure);
    };

    "a processBulk() ERROR before any stop ends the run in ERROR"_test = [] {
        qa_sched::expectErrorWhileRunningEndsRun<qa_sched::SerialScheduler, qa_sched::RunningFailingSource>(RUNNING);
        qa_sched::expectErrorWhileRunningEndsRun<qa_sched::BlockingScheduler, qa_sched::RunningFailingSource>(RUNNING);
    };

    "a block that requests its own stop and returns ERROR ends the run in ERROR"_test = [] {
        qa_sched::expectErrorWhileRunningEndsRun<qa_sched::SerialScheduler, qa_sched::SelfStoppingFailingSource>(REQUESTED_STOP);
        qa_sched::expectErrorWhileRunningEndsRun<qa_sched::BlockingScheduler, qa_sched::SelfStoppingFailingSource>(REQUESTED_STOP);
    };

    "a failing call that the stop waits for leaves the scheduler STOPPED and free to restart"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::FlushFailingSource<false>>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::GenerationProbe<qa_sched::TestScheduler> scheduler;
        gr::MsgPortIn                                      fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        qa_sched::gRunGeneration = [&scheduler] { return scheduler.generation(); };
        qa_sched::gInsideWork.store(false, std::memory_order_release);
        qa_sched::gStateAtFailure.store(IDLE, std::memory_order_release);
        qa_sched::gFailureReleased.store(true, std::memory_order_release);
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([] { return qa_sched::gInsideWork.load(std::memory_order_acquire); })) << "the source never entered its work() call";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(scheduler.state() == STOPPED) << "the stop returns with the scheduler STOPPED";
        expect(qa_sched::gStateAtFailure.load(std::memory_order_acquire) == RUNNING) << "the stop returned before the failing call that began before it";

        qa_sched::ErrorReports reports;
        expect(qa_sched::awaitCondition([&] { return (reports.take(fromScheduler), reports.has(scheduler.unique_name, source.unique_name)); })) << "the scheduler did not report the block that failed after the stop";
        expect(scheduler.state() == STOPPED) << "the failing call that the stop waited for must not move the scheduler out of STOPPED";

        qa_sched::gInsideWork.store(false, std::memory_order_release);
        expect(scheduler.changeStateTo(INITIALISED).has_value()) << "the restart after the stop is refused";
        expect(scheduler.changeStateTo(RUNNING).has_value()) << "the restart after the stop is refused";
        expect(qa_sched::awaitCondition([] { return qa_sched::gInsideWork.load(std::memory_order_acquire); })) << "the restarted run never called the source";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted run did not stop";
    };

    "a graph swap completes when a block of the running graph returns ERROR while stopping"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::FlushFailingSource<false>>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string failingName(source.unique_name);

        qa_sched::GenerationProbe<qa_sched::TestScheduler> scheduler;
        gr::MsgPortIn                                      fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        qa_sched::gRunGeneration = [&scheduler] { return scheduler.generation(); };
        qa_sched::gInsideWork.store(false, std::memory_order_release);
        qa_sched::gStateAtFailure.store(IDLE, std::memory_order_release);
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([] { return qa_sched::gInsideWork.load(std::memory_order_acquire); })) << "the source never entered its work() call";

        gr::Graph next;
        auto&     nextSource = next.emplaceBlock<qa_sched::EndlessSource>();
        auto&     nextSink   = next.emplaceBlock<qa_sched::ObservedSink>();
        expect(next.connect<"out", "in">(nextSource, nextSink).has_value());

        const auto swapped = scheduler.exchange(std::move(next));
        expect(swapped.has_value()) << "the swap is refused because a block of the old graph failed while stopping";
        expect(qa_sched::gStateAtFailure.load(std::memory_order_acquire) == RUNNING) << "the swap's stop had not begun when the source failed";
        expect(qa_sched::awaitCondition([] { return qa_sched::gObservedSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the swapped-in graph does not run";

        qa_sched::ErrorReports reports;
        expect(qa_sched::awaitCondition([&] { return (reports.take(fromScheduler), reports.has(scheduler.unique_name, failingName)); })) << "the scheduler did not report the block that failed while stopping";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the swapped-in graph did not stop";
    };
};

namespace qa_sched {

constexpr auto             kStageWait         = std::chrono::milliseconds(2000);
constexpr std::string_view kHookOrderPoolName = "qa_hook_order_cpu";

// shared by the eight blocks of a chain whose fourth block blocks in work(). The blocking stage waits in processOne()
// until its stop() or pause() hook runs, at most kStageWait. The source holds one work() call that it makes while the
// stage waits, until the stage's hook has run. Each block that does not block counts the stop() and pause() hooks that
// ran while it was inside processOne() or processBulk().
struct HookOrderProbe {
    std::mutex                         mutex;
    std::condition_variable            changed;
    bool                               stageWaits    = false;
    bool                               stageHookRan  = false;
    bool                               stageTimedOut = false; // the stage's wait ended at kStageWait
    bool                               sourceHeld    = false;
    std::array<std::atomic<bool>, 8UZ> inWork{};
    std::atomic<std::size_t>           nHooksDuringWork{0UZ};

    void hookRan(std::size_t index) {
        if (inWork[index].load(std::memory_order_acquire)) {
            nHooksDuringWork.fetch_add(1UZ, std::memory_order_relaxed);
        }
    }

    void endStageWait() {
        {
            std::lock_guard lock(mutex);
            stageHookRan = true;
        }
        changed.notify_all();
    }

    void stageWait() {
        std::unique_lock lock(mutex);
        if (stageHookRan || stageTimedOut) {
            return;
        }
        stageWaits    = true;
        stageTimedOut = !changed.wait_for(lock, kStageWait, [this] { return stageHookRan; });
    }

    void holdSourceWhileStageWaits() {
        std::unique_lock lock(mutex);
        if (!stageWaits || stageHookRan || sourceHeld) {
            return;
        }
        sourceHeld  = true;
        std::ignore = changed.wait_for(lock, kStageWait, [this] { return stageHookRan; });
    }

    [[nodiscard]] bool stageWaiting(bool withSourceHeld) {
        std::lock_guard lock(mutex);
        return stageWaits && !stageHookRan && (sourceHeld || !withSourceHeld);
    }
};

// one sample per work() call
struct HookOrderSource : gr::Block<HookOrderSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HookOrderSource, out);

    HookOrderProbe* _probe = nullptr;

    void stop() { _probe->hookRan(0UZ); }
    void pause() { _probe->hookRan(0UZ); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        _probe->inWork[0UZ].store(true, std::memory_order_release);
        _probe->holdSourceWhileStageWaits();
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        _probe->inWork[0UZ].store(false, std::memory_order_release);
        return gr::work::Status::OK;
    }
};

template<bool kBlocking>
struct HookOrderStage : gr::Block<HookOrderStage<kBlocking>> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HookOrderStage, in, out);

    HookOrderProbe* _probe = nullptr;
    std::size_t     _index = 0UZ;

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return kBlocking; }

    void stop() { onHook(); }
    void pause() { onHook(); }

    [[nodiscard]] float processOne(float sample) const {
        if constexpr (kBlocking) {
            _probe->stageWait();
        } else {
            _probe->inWork[_index].store(true, std::memory_order_release);
            _probe->inWork[_index].store(false, std::memory_order_release);
        }
        return sample;
    }

    void onHook() const {
        if constexpr (kBlocking) {
            _probe->endStageWait();
        } else {
            _probe->hookRan(_index);
        }
    }
};

struct HookOrderSink : gr::Block<HookOrderSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(HookOrderSink, in);

    HookOrderProbe* _probe = nullptr;

    void stop() { _probe->hookRan(7UZ); }
    void pause() { _probe->hookRan(7UZ); }

    void processOne(float) const {
        _probe->inWork[7UZ].store(true, std::memory_order_release);
        _probe->inWork[7UZ].store(false, std::memory_order_release);
    }
};

// runs the chain source, two stages, the blocking stage, three stages and the sink under TScheduler, and sends
// `request` while the blocking stage waits. Under multiThreaded every block has a worker of its own, and the source is
// inside a held work() call when the request arrives. A pause is followed by a stop that ends the run.
template<typename TScheduler>
void expectBlockingHooksFirst(gr::lifecycle::State request) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    constexpr bool kWorkerPerBlock = TScheduler::executionPolicy() == gr::scheduler::ExecutionPolicy::multiThreaded;

    auto           pool = fixedPool(kHookOrderPoolName, 8U);
    HookOrderProbe probe;
    gr::Graph      flow;
    auto&          source   = flow.emplaceBlock<HookOrderSource>();
    auto&          first    = flow.emplaceBlock<HookOrderStage<false>>();
    auto&          second   = flow.emplaceBlock<HookOrderStage<false>>();
    auto&          blocking = flow.emplaceBlock<HookOrderStage<true>>();
    auto&          fourth   = flow.emplaceBlock<HookOrderStage<false>>();
    auto&          fifth    = flow.emplaceBlock<HookOrderStage<false>>();
    auto&          sixth    = flow.emplaceBlock<HookOrderStage<false>>();
    auto&          sink     = flow.emplaceBlock<HookOrderSink>();
    source._probe           = &probe;
    sink._probe             = &probe;
    blocking._probe         = &probe;
    blocking._index         = 3UZ;
    for (auto [stage, index] : {std::pair{&first, 1UZ}, std::pair{&second, 2UZ}, std::pair{&fourth, 4UZ}, std::pair{&fifth, 5UZ}, std::pair{&sixth, 6UZ}}) {
        stage->_probe = &probe;
        stage->_index = index;
    }
    expect(flow.connect<"out", "in">(source, first).has_value());
    expect(flow.connect<"out", "in">(first, second).has_value());
    expect(flow.connect<"out", "in">(second, blocking).has_value());
    expect(flow.connect<"out", "in">(blocking, fourth).has_value());
    expect(flow.connect<"out", "in">(fourth, fifth).has_value());
    expect(flow.connect<"out", "in">(fifth, sixth).has_value());
    expect(flow.connect<"out", "in">(sixth, sink).has_value());

    TScheduler scheduler({{"poolName", std::string(kHookOrderPoolName)}});
    expect(scheduler.exchange(std::move(flow)).has_value());

    std::chrono::microseconds      requestTime{0};
    std::expected<void, gr::Error> result;
    const bool                     ended = runAndWaitWithin(
        scheduler, kEventBound,
        [&] {
            expect(awaitCondition([&probe] { return probe.stageWaiting(kWorkerPerBlock); })) << "the blocking stage did not wait, or the source made no work() call during the wait";
            const auto requested = std::chrono::steady_clock::now();
            expect(scheduler.changeStateTo(request).has_value());
            requestTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - requested);
            if (request == REQUESTED_PAUSE) {
                expect(awaitCondition([&scheduler] { return scheduler.state() == PAUSED; })) << "the scheduler did not pause";
                expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            }
        },
        &result);
    expect(ended) << "the run did not end on the stop";
    expect(result.has_value()) << std::format("a stopped run must succeed, and the run under {} failed: {}", gr::meta::enumName(TScheduler::executionPolicy()).value_or(""), result.has_value() ? std::string() : result.error().message);
    expect(lt(requestTime.count(), std::chrono::microseconds(kStageWait).count() / 10)) << "the request must not wait out the blocking stage's wait";
    expect(!probe.stageTimedOut) << "the blocking stage's hook must end its wait";
    expect(eq(probe.nHooksDuringWork.load(), 0UZ)) << "a hook of a block that does not block ran during its own work() call";
}

} // namespace qa_sched

const boost::ut::suite<"the order of a stop or a pause"> hookOrderTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop ends a blocking block's wait first and stops the other blocks between their work() calls"_test = [] {
        qa_sched::expectBlockingHooksFirst<qa_sched::TestScheduler>(REQUESTED_STOP);
        qa_sched::expectBlockingHooksFirst<qa_sched::SerialScheduler>(REQUESTED_STOP);
        qa_sched::expectBlockingHooksFirst<qa_sched::BlockingScheduler>(REQUESTED_STOP);
    };

    "a pause ends a blocking block's wait first and pauses the other blocks between their work() calls"_test = [] {
        qa_sched::expectBlockingHooksFirst<qa_sched::TestScheduler>(REQUESTED_PAUSE);
        qa_sched::expectBlockingHooksFirst<qa_sched::SerialScheduler>(REQUESTED_PAUSE);
        qa_sched::expectBlockingHooksFirst<qa_sched::BlockingScheduler>(REQUESTED_PAUSE);
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

        expect(inner->start().has_value()) << "the start is accepted before it runs on the sub-scheduler's thread";
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

    "a graph whose edge cannot connect fails runAndWait with the edge's reason"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::Int16Sink>();
        expect(flow.connect(source, gr::PortDefinition{"out"}, sink, gr::PortDefinition{"in"}).has_value()) << "an edge given by port names is recorded before its types are compared";

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::expected<void, gr::Error> result;
        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [] {}, &result)) << "a run whose edge did not connect never ended";
        expect(!result.has_value()) << "a run whose edge did not connect must not report success";
        expect(scheduler.state() == ERROR) << "a start whose edge did not connect must end the scheduler in ERROR";
        if (!result.has_value()) {
            const std::string& reason = result.error().message;
            expect(reason.find(std::string(source.unique_name)) != std::string::npos) << "the error must name the edge's source: " << reason;
            expect(reason.find(std::string(sink.unique_name)) != std::string::npos) << "the error must name the edge's destination: " << reason;
            expect(reason.find("IncompatiblePorts") != std::string::npos) << "the error must say why the edge did not connect: " << reason;
        }
        expect(eq(sink._nReceived, 0UZ));
    };

    "a start that one block refuses reaches a runAndWait caller as one error"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ThrowingStartSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        gr::MsgPortIn             fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(!scheduler.runAndWait().has_value());

        std::vector<std::string> errors;
        auto                     messages = fromScheduler.streamReader().get();
        for (const gr::Message& message : messages) {
            if (!message.data.has_value()) {
                errors.push_back(std::format("{}: {}", message.endpoint, message.data.error().message));
            }
        }
        std::ignore = messages.consume(messages.size());
        expect(eq(errors.size(), 1UZ)) << std::format("the caller received {} errors: {}", errors.size(), errors);
        expect(!errors.empty() && errors.front().find("the device refused to open") != std::string::npos) << "the one error must carry the start's reason";
    };

    // the test records the edge while the run is paused. The resume tries to connect it.
    "a resume whose new edge cannot connect ends the run in ERROR with the edge's reason"_test = [] {
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        gr::Graph flow;
        auto&     source    = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink      = flow.emplaceBlock<qa_sched::ObservedSink>();
        auto&     int16Sink = flow.emplaceBlock<qa_sched::Int16Sink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::expected<void, gr::Error> result;
        const bool                     ended = qa_sched::runAndWaitWithin(
            scheduler, qa_sched::kEventBound,
            [&] {
                expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the run moved no samples";
                expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
                expect(qa_sched::awaitState(scheduler, PAUSED)) << "the scheduler did not pause";
                expect(scheduler.graph().connect(source, gr::PortDefinition{"out"}, int16Sink, gr::PortDefinition{"in"}).has_value()) << "an edge given by port names is recorded before its types are compared";
                std::ignore = scheduler.changeStateTo(RUNNING);
            },
            &result);
        expect(ended) << "a run whose new edge did not connect at the resume never ended";
        expect(!result.has_value()) << "a run whose new edge did not connect must not report success";
        expect(scheduler.state() == ERROR) << "a resume whose edge did not connect must end the scheduler in ERROR";
        const std::optional<gr::Error> reason = scheduler.startError();
        expect(reason.has_value()) << "startError() must keep why the resume failed";
        if (reason.has_value()) {
            expect(reason->message.find(std::string(source.unique_name)) != std::string::npos) << "the error must name the edge's source: " << reason->message;
            expect(reason->message.find(std::string(int16Sink.unique_name)) != std::string::npos) << "the error must name the edge's destination: " << reason->message;
            expect(reason->message.find("IncompatiblePorts") != std::string::npos) << "the error must say why the edge did not connect: " << reason->message;
        }
        expect(eq(int16Sink._nReceived, 0UZ));
    };
};

const boost::ut::suite<"a scheduler started on its own thread"> ownThreadStartTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stopped scheduler started on its own thread runs again"_test = [] {
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        gr::SchedulerWrapper<qa_sched::TestScheduler> wrapper;
        wrapper.setGraph(std::move(flow));

        expect(wrapper.start().has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the first start did not run the graph";
        wrapper.stop();
        wrapper.blockRef().waitDone(); // the count holds still once the first run's workers have left
        expect(wrapper.blockRef().state() == STOPPED);

        const std::size_t nFirstRun = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(wrapper.start().has_value()) << "a stopped scheduler must accept a start";
        expect(qa_sched::awaitObservedSamplesAbove(nFirstRun)) << "the second start did not run the graph";
        expect(wrapper.blockRef().state() == RUNNING);
        wrapper.stop();
    };

    "a scheduler whose start failed on its own thread starts again once its block can start"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::PluggableSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        gr::SchedulerWrapper<qa_sched::TestScheduler> wrapper;
        wrapper.setGraph(std::move(flow));

        expect(wrapper.start().has_value()) << "the start fails on the scheduler's thread, after it was accepted";
        if (wrapper._schedulerThread.joinable()) {
            wrapper._schedulerThread.join(); // the start runs on this thread and has ended with it
        }
        expect(wrapper.blockRef().state() == ERROR) << "the first start could not complete";

        source._pluggedIn = true;
        expect(wrapper.start().has_value()) << "a scheduler in ERROR must accept a start";
        if (wrapper._schedulerThread.joinable()) {
            wrapper._schedulerThread.join(); // under multiThreaded the start returns once the workers are counted
        }
        wrapper.blockRef().waitDone(); // the workers end once the bounded stream has passed
        expect(ge(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the stream must pass once the source can start";
        wrapper.stop();
    };

    "a start of a running scheduler succeeds, reports nothing and leaves the run going"_test = [] {
        gr::SchedulerWrapper<qa_sched::TestScheduler> wrapper;
        wrapper.setGraph(qa_sched::makeEndlessGraph());
        gr::MsgPortIn fromScheduler;
        expect(wrapper.blockRef().msgOut.connect(fromScheduler).has_value());

        expect(wrapper.start().has_value());
        expect(qa_sched::awaitCondition([&wrapper] { return wrapper.blockRef().workerStarted(); })) << "the first start did not run the graph";
        expect(wrapper.start().has_value()) << "a start of a running scheduler must succeed";
        expect(wrapper.blockRef().state() == RUNNING) << "the second start must leave the run going";

        std::vector<std::string> errors;
        auto                     messages = fromScheduler.streamReader().get();
        for (const gr::Message& message : messages) {
            if (!message.data.has_value()) {
                errors.push_back(std::format("{}: {}", message.endpoint, message.data.error().message));
            }
        }
        std::ignore = messages.consume(messages.size());
        expect(errors.empty()) << std::format("a start of a running scheduler must report nothing: {}", errors);
        wrapper.stop();
    };

    // a paused scheduler cannot reach RUNNING through a start. It refuses the start.
    "a sub-scheduler that refuses its start fails its parent's start with the refusal"_test = [] {
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::ObservedSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>();
        inner->setGraph(std::move(innerFlow));
        expect(inner->start().has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the sub-scheduler did not run on its own";
        expect(inner->blockRef().changeStateTo(REQUESTED_PAUSE).has_value());
        expect(qa_sched::awaitCondition([&inner] { return inner->blockRef().state() == PAUSED; })) << "the sub-scheduler did not pause";

        gr::Graph                             flow       = qa_sched::makeEndlessGraph();
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        std::ignore = scheduler.changeStateTo(RUNNING); // the start's outcome is read from the state

        expect(qa_sched::awaitCondition([&scheduler] { return !gr::lifecycle::isActive(scheduler.state()); }, qa_sched::kRunBound)) << "a parent whose sub-scheduler refused its start still reads active";
        expect(scheduler.state() == ERROR) << "a sub-scheduler's refusal must end its parent's start in ERROR";

        const std::optional<gr::Error> reason = scheduler.startError();
        expect(reason.has_value()) << "the parent must keep the refusal";
        if (reason.has_value()) {
            expect(reason->message.find(innerName) != std::string::npos) << "the refusal must name the sub-scheduler: " << reason->message;
            expect(reason->message.find("PAUSED") != std::string::npos) << "the refusal must name the state that cannot start: " << reason->message;
        }
        if (gr::lifecycle::isActive(scheduler.state())) {
            scheduler.requestStop();
        }
        inner->stop();
    };

    // a single-threaded sub-scheduler runs its graph on its own thread, and a block that fails ends that run in ERROR.
    // A subscriber reads the parent's message port. A parent with a reader does not turn a child's error message into
    // its own.
    "a sub-scheduler whose run fails on its own thread fails its parent's run with the reason"_test = [] {
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::FailingSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
        const std::string failingName(innerSource.unique_name);

        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::SerialScheduler>>();
        inner->setGraph(std::move(innerFlow));

        gr::Graph                             flow       = qa_sched::makeEndlessGraph();
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler;
        gr::MsgPortIn           fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::expected<void, gr::Error> result;
        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [] {}, &result)) << "the parent ran on after its sub-scheduler failed";
        expect(!result.has_value()) << "a sub-scheduler that failed while it ran must fail its parent's run";
        if (!result.has_value()) {
            expect(result.error().message.find(innerName) != std::string::npos) << "the error must name the sub-scheduler: " << result.error().message;
            expect(result.error().message.find(failingName) != std::string::npos) << "the error must carry the sub-scheduler's reason: " << result.error().message;
        }
        expect(scheduler.state() == ERROR) << "the parent's run must end in ERROR";
        expect(inner->blockRef().state() == ERROR) << "the sub-scheduler's run must end in ERROR";
        inner->stop();
    };

    // the parent's pool has a thread for each block of its graph. The sub-scheduler shares a job list with a block that
    // works, and its source fails only after a call of the parent to the sub-scheduler has returned
    "a sub-scheduler that fails after its parent's first call to it fails its parent's run"_test = [] {
        constexpr std::string_view kParentPoolName = "qa_late_failure_cpu";
        auto                       pool            = qa_sched::fixedPool(kParentPoolName, 3U);

        std::atomic<bool>                 failureGate{false};
        const qa_sched::GatedSubScheduler inner = qa_sched::makeGatedSubScheduler(failureGate);

        gr::Graph                             flow       = qa_sched::makeEndlessGraph();
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner.scheduler));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler({{"poolName", std::string(kParentPoolName)}});
        gr::MsgPortIn           fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        const auto                                     holdsSubScheduler = [&innerName](const std::vector<std::shared_ptr<gr::BlockModel>>& jobList) { return std::ranges::any_of(jobList, [&innerName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == innerName; }); };
        const auto                                     worksOnItsOwn     = [](const std::shared_ptr<gr::BlockModel>& block) { return block->blockCategory() == gr::block::Category::NormalBlock; };
        const std::shared_ptr<gr::scheduler::JobLists> jobLists          = scheduler.jobs();
        const auto                                     ownList           = std::ranges::find_if(*jobLists, holdsSubScheduler);
        expect(ownList != jobLists->end()) << "a job list must hold the sub-scheduler";
        if (ownList != jobLists->end()) {
            expect(std::ranges::any_of(*ownList, worksOnItsOwn)) << "the sub-scheduler's job list must hold a block that works";
        }

        std::size_t                    nReturnedBeforeFailure = 0UZ;
        std::expected<void, gr::Error> result;
        const bool                     ended = qa_sched::runAndWaitWithin(
            scheduler, qa_sched::kEventBound,
            [&inner, &failureGate, &nReturnedBeforeFailure] {
                std::ignore            = qa_sched::awaitCondition([&inner] { return inner.scheduler->_nReturnedWorkCalls.load(std::memory_order_acquire) > 0UZ; });
                nReturnedBeforeFailure = inner.scheduler->_nReturnedWorkCalls.load(std::memory_order_acquire);
                failureGate.store(true, std::memory_order_release);
            },
            &result);
        expect(gt(nReturnedBeforeFailure, 0UZ)) << "a call of the parent to the sub-scheduler must return before its source fails";
        qa_sched::expectFailedBySubScheduler(scheduler, ended, result, innerName, inner);
        inner.scheduler->stop();
    };

    // the parent's pool has two threads, and its job lists hold an endless chain and a finite chain. The sub-scheduler
    // shares its job list with the finite chain, and its source fails only after that chain has ended
    "a sub-scheduler that fails after the other blocks of its job list end fails its parent's run"_test = [] {
        constexpr std::string_view kParentPoolName = "qa_ended_list_cpu";
        auto                       pool            = qa_sched::fixedPool(kParentPoolName, 2U);

        std::atomic<bool>                 failureGate{false};
        const qa_sched::GatedSubScheduler inner = qa_sched::makeGatedSubScheduler(failureGate);

        gr::Graph                             flow       = qa_sched::makeEndlessGraph();
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner.scheduler));
        const std::string                     innerName(innerBlock->uniqueName());
        auto&                                 finiteSource = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&                                 finiteSink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(finiteSource, finiteSink).has_value());
        const std::array<std::string_view, 3UZ> finiteListNames{innerName, finiteSource.unique_name, finiteSink.unique_name};

        qa_sched::TestScheduler scheduler({{"poolName", std::string(kParentPoolName)}});
        gr::MsgPortIn           fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());

        const std::shared_ptr<gr::scheduler::JobLists> jobLists  = scheduler.jobs();
        const auto                                     blockName = [](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName(); };
        expect(eq(jobLists->size(), 2UZ)) << "the parent must have a job list for each thread of its pool";
        expect(!jobLists->empty() && std::ranges::equal(jobLists->back(), finiteListNames, std::ranges::equal_to{}, blockName)) << "the sub-scheduler must share its job list with the finite chain alone";

        std::expected<void, gr::Error> result;
        const bool                     ended = qa_sched::runAndWaitWithin(
            scheduler, qa_sched::kEventBound,
            [&finiteSink, &failureGate] {
                expect(qa_sched::awaitCondition([&finiteSink] { return finiteSink.state() == STOPPED; })) << "the finite chain must end";
                failureGate.store(true, std::memory_order_release);
            },
            &result);
        expect(eq(finiteSink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the finite chain must deliver each of its samples";
        qa_sched::expectFailedBySubScheduler(scheduler, ended, result, innerName, inner);
        inner.scheduler->stop();
    };

    // the parent's graph holds the sub-scheduler alone. The sub-scheduler's source fails only after a call of the parent
    // to the sub-scheduler has returned
    "a parent that holds only a sub-scheduler fails its run when the sub-scheduler fails"_test = [] {
        std::atomic<bool>                 failureGate{false};
        const qa_sched::GatedSubScheduler inner = qa_sched::makeGatedSubScheduler(failureGate);

        gr::Graph                             flow;
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner.scheduler));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler;
        gr::MsgPortIn           fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::expected<void, gr::Error> result;
        const bool                     ended = qa_sched::runAndWaitWithin(
            scheduler, qa_sched::kEventBound,
            [&inner, &failureGate] {
                expect(qa_sched::awaitCondition([&inner] { return inner.scheduler->_nReturnedWorkCalls.load(std::memory_order_acquire) > 0UZ; })) << "the parent must call the sub-scheduler";
                failureGate.store(true, std::memory_order_release);
            },
            &result);
        qa_sched::expectFailedBySubScheduler(scheduler, ended, result, innerName, inner);
        inner.scheduler->stop();
    };

    "a parent that holds only a sub-scheduler ends its run when the sub-scheduler's graph ends"_test = [] {
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::DoneSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::SerialScheduler>>();
        inner->setGraph(std::move(innerFlow));

        gr::Graph flow;
        std::ignore = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::expected<void, gr::Error> result;
        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [] {}, &result)) << "the parent's run must end once the sub-scheduler's graph ends";
        expect(result.has_value()) << "the parent's run must succeed";
        expect(eq(innerSink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the parent's run must last until the sub-scheduler's graph ends";
        inner->stop();
    };
};

const boost::ut::suite<"the pool of a scheduler started on its own thread"> ownThreadPoolTests = [] {
    using namespace boost::ut;

    // a scheduler started on its own thread holds pool threads for as long as it runs. One that names the default CPU
    // pool runs on the default IO pool instead.
    "a scheduler started on its own thread runs its workers on the pool its setting names"_test = [] {
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        gr::SchedulerWrapper<qa_sched::PoolProbe<qa_sched::TestScheduler>> wrapper;
        wrapper.setGraph(std::move(flow));
        expect(eq(std::string(wrapper.blockRef().poolName.value), std::string(gr::thread_pool::kDefaultCpuPoolId)));

        expect(wrapper.start().has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the scheduler did not run its graph";
        expect(eq(std::string(wrapper.blockRef().poolName.value), std::string(gr::thread_pool::kDefaultIoPoolId))) << "the setting must name the IO pool";
        expect(eq(wrapper.blockRef().poolInUse(), std::string(gr::thread_pool::kDefaultIoPoolId))) << "the workers must run on the pool the setting names";
        wrapper.stop();
    };

    "a scheduler started on its own thread runs its workers on the pool staged for it"_test = [] {
        constexpr std::string_view kStagedPoolName = "qa_own_thread_staged_cpu";
        auto                       pool            = qa_sched::fixedPool(kStagedPoolName, 2U);
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        gr::SchedulerWrapper<qa_sched::PoolProbe<qa_sched::TestScheduler>> wrapper;
        wrapper.setGraph(std::move(flow));
        expect(wrapper.blockRef().settings().setStaged({{"poolName", std::string(kStagedPoolName)}}).empty()) << "the scheduler must stage poolName";

        expect(wrapper.start().has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the scheduler did not run its graph";
        expect(eq(std::string(wrapper.blockRef().poolName.value), std::string(kStagedPoolName))) << "the setting must name the staged pool";
        expect(eq(wrapper.blockRef().poolInUse(), std::string(kStagedPoolName))) << "the workers must run on the staged pool";
        wrapper.stop();
    };
};

const boost::ut::suite<"settings staged on a scheduler"> stagedSchedulerSettingsTests = [] {
    using namespace boost::ut;

    // under singleThreadedBlocking the worker parks after every pass without progress. The timeout_ms given to the
    // constructor outlasts the run bound, and the timeout_ms staged after the construction does not. Opening the gate
    // moves no sequence. The run ends in time only if its parks last the staged timeout_ms.
    "a timeout_ms staged after the construction rules the next run"_test = [] {
        qa_sched::gSourceGate.store(false, std::memory_order_release);
        qa_sched::gGatedSourceCalls.store(0UZ, std::memory_order_release);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::GatedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler({{"timeout_ms", gr::Size_t(60'000)}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(60'000)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.settings().setStaged({{"timeout_ms", gr::Size_t(25)}}).empty()) << "the scheduler must stage timeout_ms";

        const bool completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [] {
            for (std::size_t seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire); seen == 0UZ; seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire)) {
                qa_sched::gGatedSourceCalls.wait(seen);
            }
            qa_sched::gSourceGate.store(true, std::memory_order_release);
        });
        expect(completed) << "the run parked for the timeout_ms given to the constructor";
        expect(eq(scheduler.timeout_ms.value, gr::Size_t(25))) << "the staged timeout_ms must be in force";
        expect(eq(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };

    // the scheduler sizes its job lists on its pool when it initializes. A pool staged after that sizes the lists of the
    // run that the next start begins
    "a pool staged on an initialized scheduler sizes the next run's job lists"_test = [] {
        constexpr std::string_view kInitialPoolName = "qa_initial_cpu";
        constexpr std::string_view kStagedPoolName  = "qa_staged_cpu";
        auto                       initialPool      = qa_sched::fixedPool(kInitialPoolName, 2U);
        auto                       stagedPool       = qa_sched::fixedPool(kStagedPoolName, 1U);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler({{"poolName", std::string(kInitialPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(gr::lifecycle::State::INITIALISED).has_value());
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "the pool of two threads must size a job list for each of the two blocks";
        expect(scheduler.settings().setStaged({{"poolName", std::string(kStagedPoolName)}}).empty()) << "the scheduler must stage poolName";

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "the run on the staged pool did not end";
        expect(eq(std::string(scheduler.poolName.value), std::string(kStagedPoolName))) << "the staged poolName must be in force";
        expect(eq(scheduler.jobs()->size(), 1UZ)) << "the job lists must be sized on the staged pool's one thread";
        expect(eq(sink._nReceived, qa_sched::kSamplesBeforeTerminal));
    };
};

const boost::ut::suite<"the calling thread's name"> callerThreadNameTests = [] {
    using namespace boost::ut;

    "a single-threaded run leaves the calling thread's name as it found it"_test = [] {
        const std::string originalName = gr::thread_pool::thread::getThreadName();
        gr::thread_pool::thread::setThreadName("qa-caller");
        const std::string nameBeforeRun = gr::thread_pool::thread::getThreadName();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::SerialScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        const std::string nameAfterRun = gr::thread_pool::thread::getThreadName();
        gr::thread_pool::thread::setThreadName(originalName);
        expect(eq(nameAfterRun, nameBeforeRun)) << "the run on the calling thread must leave its name as it found it";
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

    "a stop requested while IDLE leaves the next runAndWait free to run"_test = [] { qa_sched::expectRunAfterStopBeforeEntry([](qa_sched::BlockingScheduler& scheduler) { scheduler.requestStop(); }); };

    "a stop requested while initialized leaves the next runAndWait free to run"_test = [] {
        qa_sched::expectRunAfterStopBeforeEntry([](qa_sched::BlockingScheduler& scheduler) {
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            scheduler.requestStop();
        });
    };

    // the scheduler's stop() moves REQUESTED_STOP on to STOPPED. A stop straight to STOPPED leaves the same state
    "a stop straight to STOPPED while IDLE leaves the next runAndWait free to run"_test = [] { qa_sched::expectRunAfterStopBeforeEntry([](qa_sched::BlockingScheduler& scheduler) { expect(scheduler.changeStateTo(STOPPED).has_value()); }); };

    "a stop racing the startup transient after runAndWait is entered always releases runAndWait"_test = [] {
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

            // runAndWait() is entered once it has moved the scheduler out of IDLE. The stop lands wherever the runner
            // is after that
            if (!qa_sched::runAndWaitWithin(scheduler, qa_sched::kRunBound, [&scheduler] {
                    scheduler.waitOnState(IDLE);
                    scheduler.requestStop();
                })) {
                nCyclesBlocked++;
            }
            if (gr::lifecycle::isActive(scheduler.state())) {
                nCyclesLeftActive++;
            }
        }

        expect(eq(nCyclesBlocked, 0UZ)) << "cycles in which runAndWait() did not return within the deadline";
        expect(eq(nCyclesLeftActive, 0UZ)) << "cycles that left the scheduler in an active state";
    };

    "a stop of a run started through changeStateTo() leaves the next runAndWait free to run"_test = [] {
        qa_sched::gSourceGate.store(false, std::memory_order_release);
        qa_sched::gGatedSourceCalls.store(0UZ, std::memory_order_release);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::GatedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        // the closed gate keeps the first run going until the stop below ends it
        for (std::size_t seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire); seen == 0UZ; seen = qa_sched::gGatedSourceCalls.load(std::memory_order_acquire)) {
            qa_sched::gGatedSourceCalls.wait(seen);
        }
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the first run did not stop";
        scheduler.waitDone();

        qa_sched::gSourceGate.store(true, std::memory_order_release);
        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "runAndWait() did not finish the finite graph";
        expect(eq(source._nEmitted, qa_sched::kSamplesBeforeTerminal)) << "runAndWait() returned without running the graph";
        expect(eq(sink._nReceived, qa_sched::kSamplesBeforeTerminal)) << "the sink did not receive the second run's samples";
        qa_sched::gSourceGate.store(false, std::memory_order_release);
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

    // an empty graph gets one job list, as under a single-threaded policy. Its worker finds no block to run and ends the run
    "a multi-threaded scheduler runs an empty graph to its end"_test = [] {
        using enum gr::lifecycle::State;
        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(gr::Graph{}).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(eq(scheduler.jobs()->size(), 1UZ)) << "an empty graph must get one job list";

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "the run of an empty graph did not end";
        expect(scheduler.state() == STOPPED) << "the run of an empty graph must end STOPPED";
    };

    // the first scheduler's workers are queued and no thread has taken them when the second scheduler builds its job
    // lists. The second claims only the threads the first one's workers leave
    "a second scheduler started at once on a growable pool builds no more job lists than the free threads"_test = [] {
        using enum gr::lifecycle::State;
        constexpr std::uint32_t kThreads = 4U;

        qa_sched::NamedPool pool(qa_sched::kDeferringPoolName, std::make_shared<qa_sched::DeferringPool>(kThreads));

        auto endlessChains = [](std::size_t nChains) {
            gr::Graph flow;
            for (std::size_t i = 0UZ; i < nChains; ++i) {
                auto& source = flow.emplaceBlock<qa_sched::EndlessSource>();
                auto& sink   = flow.emplaceBlock<qa_sched::CountingSink>();
                expect(flow.connect<"out", "in">(source, sink).has_value());
            }
            return flow;
        };

        qa_sched::TestScheduler first({{"poolName", std::string(qa_sched::kDeferringPoolName)}});
        qa_sched::TestScheduler second({{"poolName", std::string(qa_sched::kDeferringPoolName)}});
        expect(first.exchange(endlessChains(1UZ)).has_value());
        expect(second.exchange(endlessChains(2UZ)).has_value());

        expect(first.changeStateTo(INITIALISED).has_value());
        expect(first.changeStateTo(RUNNING).has_value());
        const std::size_t nFirstJobLists = first.jobs()->size();
        expect(eq(pool->numTasksQueued(), nFirstJobLists)) << "the first scheduler's workers must still be queued";

        expect(second.changeStateTo(INITIALISED).has_value());
        expect(le(second.jobs()->size(), std::size_t(kThreads) - nFirstJobLists)) << std::format("the second scheduler built {} job lists while {} of {} threads were claimed", second.jobs()->size(), nFirstJobLists, kThreads);
        expect(second.changeStateTo(RUNNING).has_value());

        pool->release();
        expect(qa_sched::awaitCondition([&] { return first.workerStarted() && second.workerStarted(); })) << "the workers did not start";
        expect(first.changeStateTo(REQUESTED_STOP).has_value());
        expect(second.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitCondition([&] { return !first.isProcessing() && !second.isProcessing(); })) << "the workers did not leave";
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

namespace qa_sched {

// sends a stop, a reset and a start in one span to the scheduler's message port. One worker handles all three
void sendRestartBatch(gr::MsgPortOut& toScheduler, std::string_view schedulerName) {
    using enum gr::lifecycle::State;
    constexpr std::array kCommands{REQUESTED_STOP, INITIALISED, RUNNING};

    auto span = toScheduler.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(kCommands.size());
    for (std::size_t i = 0UZ; i < kCommands.size(); ++i) {
        span[i].cmd         = gr::message::Command::Set;
        span[i].serviceName = schedulerName;
        span[i].endpoint    = gr::block::property::kLifeCycleState;
        span[i].data        = gr::property_map{{"state", std::string(gr::meta::enumName(kCommands[i]).value_or(""))}};
    }
    span.publish(kCommands.size());
}

// reads the scheduler's count of workers that have not yet left their loop
struct WorkerCountProbe : TestScheduler {
    using TestScheduler::TestScheduler;

    [[nodiscard]] std::size_t nRunningJobs() const { return this->_nRunningJobs->value(); }
};

// handlers that took the probe message, handlers that returned from it, and handlers whose wait ran out
inline std::atomic<int>  gProbeTakers{0};
inline std::atomic<int>  gProbeReturns{0};
inline std::atomic<bool> gProbeWaitExpired{false};

// posts a probe message to its own msgIn from its second start() hook. The message is then waiting when the restarted
// run begins. The handler holds each taker until a second taker arrives or _workersSettled() reports that only the
// restarted run's workers remain. A single taker means a single worker handled the message.
struct SelfMessagingSource : gr::Block<SelfMessagingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SelfMessagingSource, out);

    std::function<bool()> _workersSettled;

    void start() {
        if (gStartHooks.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
            gr::Message probe;
            probe.cmd      = gr::message::Command::Set;
            probe.endpoint = "probe";
            probe.data     = gr::property_map{};

            auto writer = this->msgIn.buffer().streamBuffer.new_writer();
            auto span   = writer.tryReserve<gr::SpanReleasePolicy::ProcessAll>(1UZ);
            if (!span.empty()) {
                span[0] = std::move(probe);
                span.publish(1UZ);
            }
        }
        gStartHooks.notify_all();
    }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }

    void processMessages(gr::MsgPortInBuiltin&, auto&& messages) {
        for (const gr::Message& message : messages) {
            if (message.endpoint != "probe") {
                continue;
            }
            gProbeTakers.fetch_add(1, std::memory_order_acq_rel);
            if (!awaitCondition([this] { return gProbeTakers.load(std::memory_order_acquire) >= 2 || _workersSettled(); })) {
                gProbeWaitExpired.store(true, std::memory_order_relaxed);
            }
            gProbeReturns.fetch_add(1, std::memory_order_release);
        }
    }
};

} // namespace qa_sched

const boost::ut::suite<"lifecycle commands on the scheduler's message port"> messageLifecycleTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop, a reset and a start in one message batch restart the run"_test = [] {
        qa_sched::gStartHooks.store(0, std::memory_order_release);
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        auto scheduler = std::make_unique<qa_sched::TestScheduler>();
        expect(scheduler->exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(toScheduler.connect(scheduler->msgIn).has_value());
        expect(scheduler->changeStateTo(INITIALISED).has_value());
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the first run moved no samples";

        qa_sched::sendRestartBatch(toScheduler, scheduler->unique_name);

        const bool restarted = qa_sched::awaitCondition([] { return qa_sched::gStartHooks.load(std::memory_order_acquire) >= 2; });
        expect(restarted) << "the start in the batch never started the blocks";
        if (!restarted) {
            std::ignore = scheduler.release(); // its worker waits on itself, so destroying the scheduler would hang the suite
            return;
        }
        const std::size_t afterRestart = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(qa_sched::awaitObservedSamplesAbove(afterRestart)) << "the restarted run moved no samples";
        expect(scheduler->state() == RUNNING) << "the restarted run left RUNNING";

        expect(scheduler->changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(*scheduler, STOPPED)) << "the restarted run did not stop";
    };

    // the worker that handled the batch leaves before it touches a block again, so it never handles a block's message
    // at the same time as the restarted run's worker
    "a block message waiting when a restart by message begins is handled once"_test = [] {
        qa_sched::gStartHooks.store(0, std::memory_order_release);
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);
        qa_sched::gProbeTakers.store(0, std::memory_order_release);
        qa_sched::gProbeReturns.store(0, std::memory_order_release);
        qa_sched::gProbeWaitExpired.store(false, std::memory_order_relaxed);

        auto      scheduler = std::make_unique<qa_sched::WorkerCountProbe>();
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::SelfMessagingSource>(); // the first block is in the first job list
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        source._workersSettled = [probe = scheduler.get()] { return probe->nRunningJobs() <= probe->jobs()->size(); };

        expect(scheduler->exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(toScheduler.connect(scheduler->msgIn).has_value());
        expect(scheduler->changeStateTo(INITIALISED).has_value());
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the first run moved no samples";

        qa_sched::sendRestartBatch(toScheduler, scheduler->unique_name);

        const bool handled = qa_sched::awaitCondition([] { return qa_sched::gProbeReturns.load(std::memory_order_acquire) >= 1; });
        expect(handled) << "no worker handled the message posted during the restart";
        if (!handled) {
            std::ignore = scheduler.release(); // a worker may still wait on itself, so destroying the scheduler would hang the suite
            return;
        }
        expect(eq(qa_sched::gProbeTakers.load(std::memory_order_acquire), 1)) << "more than one worker handled the message";
        expect(!qa_sched::gProbeWaitExpired.load(std::memory_order_relaxed)) << "the worker that handled the batch never left";

        expect(scheduler->changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(*scheduler, STOPPED)) << "the restarted run did not stop";
    };

    // the worker that handled the batch still occupies its pool thread while the reset sizes the job lists. It leaves
    // once the restarted run is dispatched, so the restart claims that thread as free
    "a restart by message builds as many job lists as the first start"_test = [] {
        qa_sched::gStartHooks.store(0, std::memory_order_release);
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        auto pool = qa_sched::twoThreadPool();

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::RaceSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler({{"poolName", std::string(qa_sched::kOccupiedPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the first run moved no samples";
        const std::size_t nFirstJobLists = scheduler.jobs()->size();
        expect(eq(nFirstJobLists, 2UZ)) << "the first start did not give each block its own pool thread";

        qa_sched::sendRestartBatch(toScheduler, scheduler.unique_name);

        expect(qa_sched::awaitCondition([] { return qa_sched::gStartHooks.load(std::memory_order_acquire) >= 2; })) << "the start in the batch never started the blocks";
        expect(eq(scheduler.jobs()->size(), nFirstJobLists)) << "the restart by message built fewer job lists than the first start";
        const std::size_t afterRestart = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(qa_sched::awaitObservedSamplesAbove(afterRestart)) << "the restarted run moved no samples";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted run did not stop";
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

    "a blocking block that no run started since a reset stays initialized at a stop and runs after a restart"_test = [] {
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
        expect(source.state() == INITIALISED) << "a blocking block that no run has started since the reset must stay initialized";

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

    // the stop runs on the test's thread while the workers are inside their loop. The sink's stop hook waits, with a
    // deadline, for the job count that waitDone() waits for, and then calls waitDone(). The stop reaches the source
    // before the sink. The last worker to leave or the stop itself settles the blocking source before the wait ends
    "a stop hook on the caller's thread that waits for the workers returns, and the graph runs again after a restart"_test = [] {
        constexpr int kRuns = 2;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StopActionSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());

        int nHooksTimedOut          = 0;
        int nHooksWithSourceStopped = 0;
        sink._onStop                = [&scheduler, &source, &nHooksTimedOut, &nHooksWithSourceStopped] {
            if (!qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) {
                nHooksTimedOut++; // a timed-out hook returns, and the stop continues
                return;
            }
            scheduler.waitDone();
            nHooksWithSourceStopped += source.state() == STOPPED ? 1 : 0;
        };

        for (int run = 0; run < kRuns && nHooksTimedOut == 0; ++run) {
            const std::size_t nBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());
            expect(qa_sched::awaitObservedSamplesAbove(nBefore)) << "the run did not move a sample";
            expect(scheduler.isProcessing()) << "the stop must land on a run whose workers are inside their loop";

            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";
            expect(qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); })) << "the stopped run's workers did not leave";
            expect(source.state() == STOPPED) << "the blocking source must reach STOPPED when the scheduler stops";
        }

        expect(eq(nHooksTimedOut, 0)) << "the workers did not leave while a stop hook on the caller's thread waited for them";
        expect(eq(sink._nStopCalls, kRuns)) << "every stop must run the sink's stop hook once";
        expect(eq(nHooksWithSourceStopped, kRuns)) << "the blocking source must be STOPPED when waitDone() returns in the hook";
    };

    // stop() retires the workers before its sweep. The first block's stop hook holds the sweep until every worker has
    // left, so the sweep reaches the blocking source after the last worker. The stop hook of the block after the source
    // reads the source's state
    "a blocking block that the stop reaches after the last worker left is STOPPED before the next block's stop hook"_test = [] {
        gr::Graph flow;
        auto&     waiter   = flow.emplaceBlock<qa_sched::StopActionSink>();
        auto&     blocking = flow.emplaceBlock<qa_sched::BlockingSource>();
        auto&     checker  = flow.emplaceBlock<qa_sched::StopActionSink>();
        auto&     source   = flow.emplaceBlock<qa_sched::EndlessSource>();
        expect(flow.connect<"out", "in">(source, waiter).has_value());
        expect(flow.connect<"out", "in">(blocking, checker).has_value());
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::TestScheduler scheduler;
        bool                    workersLeft     = false;
        gr::lifecycle::State    stateAtNextHook = IDLE;
        waiter._onStop                          = [&scheduler, &workersLeft] { workersLeft = qa_sched::awaitCondition([&scheduler] { return !scheduler.isProcessing(); }); };
        checker._onStop                         = [&blocking, &stateAtNextHook] { stateAtNextHook = blocking.state(); };
        expect(scheduler.exchange(std::move(flow)).has_value());

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the run did not move a sample";
        expect(scheduler.isProcessing()) << "the stop must land on a run whose workers are inside their loop";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the scheduler did not stop";

        expect(workersLeft) << "the workers did not leave while the first stop hook held the sweep";
        expect(stateAtNextHook == STOPPED) << std::format("the blocking source reads {} when the next block's stop hook runs", gr::meta::enumName(stateAtNextHook).value_or(""));
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

        // each run is held until every counted watchdog sleeps. Each restart then meets the earlier watchdog asleep in
        // its period. A restart that does not retire it leaves two watchdogs asleep, and the final wait for one fails.
        // The cycles take far less than one 5 s period, and no earlier watchdog wakes on its own meanwhile.
        qa_sched::WatchdogProbe scheduler({{"watchdog_timeout", gr::Size_t(5000)}});
        expect(scheduler.exchange(qa_sched::makeEndlessGraph()).has_value());
        auto everyWatchdogAsleep = [&scheduler] {
            const std::size_t nSleeping = scheduler.nSleepingWatchdogs();
            return nSleeping != 0UZ && nSleeping == scheduler.nWatchdogsRunning();
        };

        for (std::size_t cycle = 0UZ; cycle < kCycles; ++cycle) {
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());
            expect(qa_sched::awaitState(scheduler, RUNNING)) << std::format("cycle {} did not reach RUNNING", cycle);
            expect(qa_sched::awaitCondition(everyWatchdogAsleep)) << std::format("cycle {}: {} watchdogs counted, {} asleep", cycle, scheduler.nWatchdogsRunning(), scheduler.nSleepingWatchdogs());
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(scheduler, STOPPED)) << std::format("cycle {} did not reach STOPPED", cycle);
        }

        // a watchdog left behind by an earlier cycle keeps going for as long as some run has jobs
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitState(scheduler, RUNNING)) << "the final run did not reach RUNNING";

        // a retired watchdog stays counted and asleep until its thread runs again
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.nWatchdogsRunning() == 1UZ && scheduler.nSleepingWatchdogs() == 1UZ; })) << std::format("{} watchdogs counted and {} asleep after {} restarts", scheduler.nWatchdogsRunning(), scheduler.nSleepingWatchdogs(), kCycles);

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the final run did not stop";
    };

    "a retired watchdog returns without waiting out its period"_test = [] {
        // timed on the wall clock, from the retirement to the watchdog's return, without the graph's teardown. Each
        // retirement meets a watchdog asleep in a 60 s period after the run has stopped. Each run is held until its
        // watchdog sleeps. A watchdog that polls for its retirement between plain sleeps never counts as asleep and
        // fails that wait. The ten retirements together return within 100 ms. The bound leaves room for the scheduling
        // delays of a loaded runner. A watchdog that leaves its sleep in slices of 100 ms instead of on the retirement
        // spends about 500 ms over the ten. A timeout_ms of 1 keeps every interval derived from timeout_ms short. The
        // destructor and a graph swap retire the watchdog the same way.
        constexpr std::size_t kCycles  = 10UZ;
        constexpr std::size_t kSamples = 4096UZ;
        constexpr auto        kBound   = std::chrono::milliseconds(100);

        std::chrono::steady_clock::duration exitTime{};
        std::size_t                         nRetired = 0UZ;
        for (std::size_t cycle = 0UZ; cycle < kCycles && exitTime < kBound; ++cycle) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());
            qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

            qa_sched::WatchdogProbe scheduler(gr::property_map{{"watchdog_timeout", gr::Size_t(60'000)}, {"timeout_ms", gr::Size_t(1)}});
            expect(scheduler.exchange(std::move(flow)).has_value());
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());
            expect(qa_sched::awaitObservedSamplesAbove(kSamples)) << std::format("cycle {} moved no samples", cycle);
            expect(qa_sched::awaitCondition([&scheduler] { return scheduler.nSleepingWatchdogs() == 1UZ; })) << std::format("cycle {}: the watchdog never slept in its period", cycle);
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(scheduler, STOPPED)) << std::format("cycle {} did not reach STOPPED", cycle);
            expect(eq(scheduler.nWatchdogsRunning(), 1UZ)) << std::format("cycle {} has no watchdog to retire", cycle);
            expect(eq(scheduler.nSleepingWatchdogs(), 1UZ)) << std::format("cycle {}: the watchdog left its sleep before the retirement", cycle);

            const auto retiredAt = std::chrono::steady_clock::now();
            scheduler.retireWatchdogs();
            exitTime += std::chrono::steady_clock::now() - retiredAt;
            ++nRetired;
        }

        expect(exitTime < kBound) << std::format("{} retired watchdogs took {} to return", nRetired, std::chrono::duration_cast<std::chrono::microseconds>(exitTime));
    };
};

namespace qa_sched {

// a parent graph of a finite chain and a single-threaded sub-scheduler. The sub-scheduler's source moves one sample for
// each release
struct ParentOfReleasedSubScheduler {
    gr::Graph                                              flow;
    CountingSink*                                          finiteSink = nullptr;
    std::shared_ptr<gr::SchedulerWrapper<SerialScheduler>> inner      = std::make_shared<gr::SchedulerWrapper<SerialScheduler>>();
};

[[nodiscard]] ParentOfReleasedSubScheduler makeParentOfReleasedSubScheduler(std::atomic<std::size_t>& nReleased) {
    using namespace boost::ut;

    gr::Graph innerFlow;
    auto&     innerSource = innerFlow.emplaceBlock<ReleasedSource>();
    auto&     innerSink   = innerFlow.emplaceBlock<CountingSink>();
    expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
    innerSource._nReleased = &nReleased;

    ParentOfReleasedSubScheduler parent;
    parent.inner->setGraph(std::move(innerFlow));
    auto& finiteSource = parent.flow.emplaceBlock<DoneSource>();
    auto& finiteSink   = parent.flow.emplaceBlock<CountingSink>();
    expect(parent.flow.connect<"out", "in">(finiteSource, finiteSink).has_value());
    parent.finiteSink = &finiteSink;
    std::ignore       = parent.flow.addBlock(gr::SchedulerModel::asBlockModelPtr(parent.inner));
    return parent;
}

} // namespace qa_sched

const boost::ut::suite<"watchdog stall report"> watchdogStallTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stalled graph is reported once per stall on the scheduler's message port"_test = [] {
        constexpr std::size_t kStalledPeriods = 2UZ;

        std::atomic<std::size_t> nReleased{0UZ};
        gr::Graph                flow;
        auto&                    source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                    sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        source._nReleased = &nReleased; // the wrapper holding the block lives on the heap. The pointer stays valid after the move.

        qa_sched::WakeProbe<qa_sched::TestScheduler> scheduler({{"watchdog_timeout", gr::Size_t(10)}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
        gr::MsgPortIn                                fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        // while the graph stalls, each watchdog period advances the wake count by exactly one
        qa_sched::StallReports reports;
        auto                   awaitReports     = [&](std::size_t expected) { return qa_sched::awaitCondition([&] { return (reports.take(fromScheduler), reports.count >= expected); }); };
        auto                   awaitMorePeriods = [&scheduler] {
            const std::size_t from = scheduler.nWakes();
            return qa_sched::awaitCondition([&scheduler, from] { return scheduler.nWakes() >= from + 4UZ * kStalledPeriods; });
        };

        expect(awaitReports(1UZ)) << "the stall produced no report";
        expect(eq(reports.lastPeriods, kStalledPeriods)) << "the report names the stalled periods";
        expect(awaitMorePeriods()) << "the watchdog stopped observing the stall";
        reports.take(fromScheduler);
        expect(eq(reports.count, 1UZ)) << "a stall that continues is reported once";

        nReleased.store(1UZ, std::memory_order_release); // one sample moves, which ends the first stall and starts the second
        expect(awaitReports(2UZ)) << "the stall after progress produced no second report";
        expect(awaitMorePeriods()) << "the watchdog stopped observing the second stall";
        reports.take(fromScheduler);
        expect(eq(reports.count, 2UZ)) << "each stall is reported once";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the stalled graph did not stop";
    };

    "a paused graph is not reported as stalled"_test = [] {
        constexpr std::size_t kStalledPeriods = 2UZ;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::WakeProbe<qa_sched::TestScheduler> scheduler({{"watchdog_timeout", gr::Size_t(10)}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
        gr::MsgPortIn                                fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        // a run that makes no progress in its first two periods is reported as stalled while it is RUNNING. The case
        // pauses a graph that has moved samples, and it ignores reports from before the pause.
        expect(qa_sched::awaitObservedSamplesAbove(0UZ)) << "the graph moved no sample before the pause";
        expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
        expect(qa_sched::awaitState(scheduler, PAUSED)) << "scheduler did not reach PAUSED";
        qa_sched::StallReports reports;
        reports.take(fromScheduler);
        const std::size_t nBeforePause = reports.count;

        // nothing moves while paused: each watchdog period advances the wake count by exactly one
        const std::size_t from = scheduler.nWakes();
        expect(qa_sched::awaitCondition([&scheduler, from] { return scheduler.nWakes() >= from + 4UZ * kStalledPeriods; })) << "the watchdog stopped observing the paused graph";
        reports.take(fromScheduler);
        expect(eq(reports.count, nBeforePause)) << "a paused graph was reported as stalled";

        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the resumed graph did not stop";
    };

    "a nested scheduler's stall report leaves a parent without a message reader running"_test = [] {
        constexpr std::size_t kStalledPeriods = 2UZ;
        qa_sched::gParentSamples              = 0UZ;

        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::ReleasedSource>(); // never released: the nested graph stalls
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::WakeProbe<qa_sched::TestScheduler>>>(gr::property_map{{"watchdog_timeout", gr::Size_t(10)}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
        inner->setGraph(std::move(innerFlow));

        gr::Graph outerFlow;
        auto&     outerSource = outerFlow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     outerSink   = outerFlow.emplaceBlock<qa_sched::ParentCountingSink>();
        expect(outerFlow.connect<"out", "in">(outerSource, outerSink).has_value());
        outerFlow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));

        qa_sched::TestScheduler parent; // no reader on its msgOut
        expect(parent.exchange(std::move(outerFlow)).has_value());
        expect(parent.changeStateTo(INITIALISED).has_value());
        expect(parent.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([] { return qa_sched::gParentSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the parent's graph never ran";

        // the nested graph stalls while its scheduler is RUNNING: after this many periods its report has been sent
        auto&             innerScheduler = inner->blockRef();
        const std::size_t from           = innerScheduler.nWakes();
        expect(qa_sched::awaitCondition([&innerScheduler, from] { return innerScheduler.nWakes() >= from + 4UZ * kStalledPeriods; })) << "the nested watchdog never observed the stall";
        expect(inner->blockRef().state() == RUNNING) << "the nested scheduler was not RUNNING during its stall";

        const std::size_t nBefore = qa_sched::gParentSamples.load(std::memory_order_relaxed);
        expect(qa_sched::awaitCondition([nBefore] { return qa_sched::gParentSamples.load(std::memory_order_relaxed) > nBefore; })) << "the parent's graph stopped moving samples";
        expect(parent.state() == RUNNING) << "the parent left RUNNING";

        expect(parent.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(parent, STOPPED)) << "the parent did not stop";
    };

    // the parent's own chain ends, and a sub-scheduler then keeps its run going. The test releases one sample to the
    // sub-scheduler's graph after each period that the parent's watchdog finds idle, which advances the parent's wake
    // count. The sub-scheduler exports no port, so only its own graph moves
    "a parent whose own blocks have ended reports no stall while its sub-scheduler's graph moves"_test = [] {
        constexpr std::size_t kStalledPeriods = 4UZ;
        constexpr std::size_t kReleases       = 3UZ * kStalledPeriods;

        std::atomic<std::size_t>                     nReleased{0UZ};
        qa_sched::ParentOfReleasedSubScheduler       parent = qa_sched::makeParentOfReleasedSubScheduler(nReleased);
        qa_sched::WakeProbe<qa_sched::TestScheduler> scheduler({{"watchdog_timeout", gr::Size_t(20)}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
        gr::MsgPortIn                                fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(parent.flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([&parent] { return parent.finiteSink->state() == STOPPED; })) << "the parent's own chain must end";

        qa_sched::StallReports reports{.serviceName = std::string(scheduler.unique_name)};
        for (std::size_t release = 0UZ; release < kReleases; ++release) {
            const std::size_t nWakes = scheduler.nWakes();
            expect(qa_sched::awaitCondition([&scheduler, nWakes] { return scheduler.nWakes() > nWakes; })) << "the parent's watchdog stopped observing its graph";
            nReleased.fetch_add(1UZ, std::memory_order_acq_rel);
        }
        expect(qa_sched::awaitCondition([&nReleased] { return nReleased.load(std::memory_order_acquire) == 0UZ; })) << "the sub-scheduler's graph must move each released sample";
        reports.take(fromScheduler);
        expect(eq(reports.count, 0UZ)) << "the parent reported a stall while its sub-scheduler's graph moved";

        // no sample moves from here on, and the same watchdog reports the stall
        expect(qa_sched::awaitCondition([&reports, &fromScheduler] { return (reports.take(fromScheduler), reports.count >= 1UZ); })) << "the parent did not report the stall once its sub-scheduler's graph stopped";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the parent did not stop";
        parent.inner->stop();
    };

    "a parent whose own blocks have ended reports the stall of its sub-scheduler's graph"_test = [] {
        constexpr std::size_t kStalledPeriods = 2UZ;

        std::atomic<std::size_t>                     nReleased{0UZ}; // never released: the sub-scheduler's graph stalls
        qa_sched::ParentOfReleasedSubScheduler       parent = qa_sched::makeParentOfReleasedSubScheduler(nReleased);
        qa_sched::WakeProbe<qa_sched::TestScheduler> scheduler({{"watchdog_timeout", gr::Size_t(10)}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
        gr::MsgPortIn                                fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(parent.flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitCondition([&parent] { return parent.finiteSink->state() == STOPPED; })) << "the parent's own chain must end";

        qa_sched::StallReports reports{.serviceName = std::string(scheduler.unique_name)};
        expect(qa_sched::awaitCondition([&reports, &fromScheduler] { return (reports.take(fromScheduler), reports.count >= 1UZ); })) << "the stall of the sub-scheduler's graph produced no report";
        expect(eq(reports.lastPeriods, kStalledPeriods)) << "the report names the stalled periods";
        expect(scheduler.state() == RUNNING) << "the sub-scheduler must keep the parent's run going";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the parent did not stop";
        parent.inner->stop();
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

    // The sink comes first in the worker's block list, so the source's last call is the only work of its pass. That
    // call publishes the last samples and returns DONE. With timeout_inactivity_count 0, a pass that leaves the
    // progress sequence unchanged parks the worker, and the park outlasts the run bound. The run ends in time only if
    // the DONE call advances the sequence.
    "a work call that returns DONE moves the progress sequence"_test = [] {
        gr::Graph flow;
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        auto&     source = flow.emplaceBlock<qa_sched::EndingSource>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler({{"timeout_ms", gr::Size_t(60'000)}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(60'000)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound)) << "the worker parked after the source's DONE call";
        expect(eq(source._nCalls, 3UZ)) << "the source ended on its third call";
        expect(eq(sink._nReceived, 2UZ * qa_sched::kEndingSamples)) << "every sample at the sink";
    };

    // One chain ends while an idle source beside it keeps the worker in its loop. The worker calls the stopped
    // blocks on every pass, and each such call returns DONE. With nothing published, the progress sequence must
    // stay where it is from one call of the idle source to the next, or the worker never parks.
    "a stopped block called again leaves the progress sequence unchanged"_test = [] {
        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_sched::EndingSource>();
        auto&     sink    = flow.emplaceBlock<qa_sched::CountingSink>();
        auto&     idle    = flow.emplaceBlock<qa_sched::IdleSource>();
        auto&     idleOut = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        expect(flow.connect<"out", "in">(idle, idleOut).has_value());
        idle._finished = &sink;

        qa_sched::BlockingScheduler scheduler({{"timeout_ms", gr::Size_t(1)}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(60'000)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool recorded = false;
        expect(qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            recorded = qa_sched::awaitCondition([&idle] { return idle._nRecorded.load(std::memory_order_acquire) == qa_sched::kIdleRecords; });
            scheduler.requestStop();
        })) << "the run did not end";
        expect(fatal(recorded)) << "the idle source was not called after the chain beside it stopped";
        expect(eq(sink._nReceived, 2UZ * qa_sched::kEndingSamples)) << "every sample at the sink";
        expect(std::ranges::all_of(idle._progressSeen, [&idle](std::size_t value) { return value == idle._progressSeen.front(); })) << std::format("the progress sequence moved between calls of the idle source: {}", idle._progressSeen);
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

namespace qa_sched {

using Millis = std::chrono::duration<double, std::milli>;

// A blocking run of a graph that never moves parks for kParkTimeoutMs after every pass. A watchdog period far beyond
// the test keeps the watchdog from ending a park. A message is sent kParkSettle after the previous one was answered;
// the worker parks right after its message pass, well inside that.
constexpr gr::Size_t  kParkTimeoutMs = 200U;
constexpr std::size_t kWakeTrials    = 5UZ;
constexpr auto        kParkSettle    = std::chrono::milliseconds(5);
constexpr double      kWakeBoundMs   = static_cast<double>(kParkTimeoutMs) / 4.0;

[[nodiscard]] inline gr::property_map parkingSettings() { return {{"timeout_ms", kParkTimeoutMs}, {"timeout_inactivity_count", gr::Size_t(0)}, {"watchdog_timeout", gr::Size_t(60'000)}}; }

// work() calls of the SilentSource blocks that count them
inline std::atomic<std::size_t> gCountedSourceCalls{0UZ};

// publishes nothing and never finishes. A message with the endpoint "swapGraph" calls _onSwapRequest on the worker
struct SilentSource : gr::Block<SilentSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SilentSource, out);

    std::function<void()> _onSwapRequest;
    bool                  _countCalls = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_countCalls) {
            gCountedSourceCalls.fetch_add(1UZ, std::memory_order_release);
        }
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message> messages) {
        for (const gr::Message& message : messages) {
            if (message.endpoint == "swapGraph" && _onSwapRequest) {
                _onSwapRequest();
            }
        }
    }
};

// sends a notification from the thread that calls notifyFromOutside(), the way a block's own I/O thread does
struct OutsideNotifier : gr::Block<OutsideNotifier> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(OutsideNotifier, in);

    void processOne(float) {}

    void notifyFromOutside(std::string_view requestId) { this->emitMessage("outside", gr::property_map{}, requestId); }
};

[[nodiscard]] inline gr::Graph makeReplacementGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source   = flow.emplaceBlock<SilentSource>();
    auto&     sink     = flow.emplaceBlock<CountingSink>();
    source._countCalls = true;
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

// consumes every message waiting on the port and reports whether one carries the request ID
[[nodiscard]] inline bool takeReply(gr::MsgPortIn& port, std::string_view requestId) {
    auto messages = port.streamReader().get();
    bool found    = std::ranges::any_of(messages, [requestId](const gr::Message& message) { return message.clientRequestID == requestId; });
    std::ignore   = messages.consume(messages.size());
    return found;
}

// the time from send() to a message on fromScheduler with the request ID, for each of kWakeTrials requests
template<typename TSend>
[[nodiscard]] std::vector<double> replyTimes(gr::MsgPortIn& fromScheduler, std::string_view prefix, TSend send) {
    std::vector<double> times;
    for (std::size_t trial = 0UZ; trial < kWakeTrials; ++trial) {
        std::this_thread::sleep_for(kParkSettle);
        const std::string requestId = std::format("{}-{}", prefix, trial);
        const auto        sentAt    = std::chrono::steady_clock::now();
        send(requestId);
        bool replied = false;
        while (!replied && std::chrono::steady_clock::now() - sentAt < kEventBound) {
            replied = takeReply(fromScheduler, requestId);
            if (!replied) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        boost::ut::expect(replied) << std::format("no reply to {}", requestId);
        times.push_back(Millis(std::chrono::steady_clock::now() - sentAt).count());
    }
    return times;
}

// consumes every message waiting on the port and returns the number of heartbeats among them
[[nodiscard]] inline std::size_t takeHeartbeats(gr::MsgPortIn& port) {
    auto        messages   = port.streamReader().get();
    std::size_t heartbeats = static_cast<std::size_t>(std::ranges::count(messages, std::string_view(gr::block::property::kHeartbeat), &gr::Message::endpoint));
    std::ignore            = messages.consume(messages.size());
    return heartbeats;
}

// runs a graph that never moves under singleThreadedBlocking with a 10 ms watchdog. A client sends a settings request
// to the scheduler, or to the sink with pollBlock, about once a millisecond, well inside the watchdog period. Each
// request ends the worker's park. It returns whether the watchdog reported the stall, and the requests sent
[[nodiscard]] inline std::pair<bool, std::size_t> stallReportedWhilePolling(bool pollBlock) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<ReleasedSource>(); // never released
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    const std::string sinkName(sink.unique_name);

    BlockingScheduler scheduler({{"timeout_ms", kParkTimeoutMs}, {"timeout_inactivity_count", gr::Size_t(2)}, {"watchdog_timeout", gr::Size_t(10)}});
    gr::MsgPortOut    toScheduler;
    gr::MsgPortIn     fromScheduler;
    expect(toScheduler.connect(scheduler.msgIn).has_value());
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());
    const std::string target = pollBlock ? sinkName : std::string(scheduler.unique_name);

    std::thread  runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
    StallReports reports;
    std::size_t  nPolls   = 0UZ;
    const bool   reported = awaitCondition([&] {
        gr::sendMessage<gr::message::Command::Get>(toScheduler, target, gr::block::property::kSetting, gr::property_map{}, "poll");
        ++nPolls;
        reports.take(fromScheduler);
        return reports.count > 0UZ;
    });
    scheduler.requestStop();
    runner.join();
    return {reported, nPolls};
}

#ifdef GR_TEST_WITH_BLOCK_LIBRARY
// a graph whose sink comes from the shared object. A counting SilentSource feeds it, or with sourceFromLibrary the
// object's own source does. It returns the sink's unique name
[[nodiscard]] inline std::pair<gr::Graph, std::string> libraryBlockGraph(bool sourceFromLibrary) {
    using namespace boost::ut;
    gr::Graph                       flow;
    std::shared_ptr<gr::BlockModel> source;
    if (sourceFromLibrary) {
        source = flow.addBlock(gr::globalBlockRegistry().create(kLibrarySource, {}));
    } else {
        auto& counting       = flow.emplaceBlock<SilentSource>();
        counting._countCalls = true;
        source               = flow.blocks().back();
    }
    std::shared_ptr<gr::BlockModel> sink = flow.addBlock(gr::globalBlockRegistry().create(kLibrarySink, {}));
    expect(fatal(source != nullptr && sink != nullptr));
    expect(flow.connect(source, gr::PortDefinition("out"), sink, gr::PortDefinition("in")).has_value());
    return {std::move(flow), std::string(sink->uniqueName())};
}
#endif

} // namespace qa_sched

const boost::ut::suite<"a message to a parked run"> parkedMessageTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a settings request to a parked run is answered before the park times out"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::SilentSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortOut              toScheduler;
        gr::MsgPortIn               fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));

        const std::vector<double> times = qa_sched::replyTimes(fromScheduler, "settings", [&](std::string_view requestId) { gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, gr::block::property::kSetting, gr::property_map{}, requestId); });
        scheduler.requestStop();
        runner.join();

        const double medianMs = qa_sched::median(times);
        std::println("settings request to a parked run (timeout_ms {}): reply after median {:.2f} ms, min {:.2f}, max {:.2f}", qa_sched::kParkTimeoutMs, medianMs, std::ranges::min(times), std::ranges::max(times));
        expect(lt(medianMs, qa_sched::kWakeBoundMs)) << std::format("the reply waited {:.1f} ms of a {} ms park", medianMs, qa_sched::kParkTimeoutMs);
    };

    // a block handles the request on the worker and writes its reply into the ring that the scheduler forwards from.
    // The worker forwards the reply on its next message pass, before it parks again
    "a settings request to a block of a parked run is answered within a tenth of the park"_test = [] {
        constexpr double kBlockReplyBoundMs = static_cast<double>(qa_sched::kParkTimeoutMs) / 10.0;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::SilentSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string sinkName(sink.unique_name);

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortOut              toScheduler;
        gr::MsgPortIn               fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));

        const std::vector<double> times = qa_sched::replyTimes(fromScheduler, "block-settings", [&](std::string_view requestId) { gr::sendMessage<gr::message::Command::Get>(toScheduler, sinkName, gr::block::property::kSetting, gr::property_map{}, requestId); });
        scheduler.requestStop();
        runner.join();

        const double medianMs = qa_sched::median(times);
        std::println("settings request to a block of a parked run (timeout_ms {}): reply after median {:.2f} ms, min {:.2f}, max {:.2f}", qa_sched::kParkTimeoutMs, medianMs, std::ranges::min(times), std::ranges::max(times));
        expect(lt(medianMs, kBlockReplyBoundMs)) << std::format("the block's reply waited {:.1f} ms of a {} ms park", medianMs, qa_sched::kParkTimeoutMs);
    };

    "a block's message sent from outside the worker reaches the scheduler's subscriber before the park times out"_test = [] {
        gr::Graph flow;
        auto&     source   = flow.emplaceBlock<qa_sched::SilentSource>();
        auto&     notifier = flow.emplaceBlock<qa_sched::OutsideNotifier>();
        expect(flow.connect<"out", "in">(source, notifier).has_value());

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortIn               fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));

        const std::vector<double> times = qa_sched::replyTimes(fromScheduler, "outside", [&notifier](std::string_view requestId) { notifier.notifyFromOutside(requestId); });
        scheduler.requestStop();
        runner.join();

        const double medianMs = qa_sched::median(times);
        std::println("block message from outside the worker of a parked run (timeout_ms {}): forwarded after median {:.2f} ms, min {:.2f}, max {:.2f}", qa_sched::kParkTimeoutMs, medianMs, std::ranges::min(times), std::ranges::max(times));
        expect(lt(medianMs, qa_sched::kWakeBoundMs)) << std::format("the message waited {:.1f} ms of a {} ms park", medianMs, qa_sched::kParkTimeoutMs);
    };

    // The requests around the swap request land before, during and after the graph exchange. After the swap the
    // retired graph's progress sequence is kept alive here, and a message must advance only the running graph's.
    "a message sent while a parked run replaces its graph is answered, and the retired graph's progress stays still"_test = [] {
        constexpr std::size_t kAroundSwap = 40UZ;
        qa_sched::gCountedSourceCalls.store(0UZ);

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::Graph                   flow;
        auto&                       source = flow.emplaceBlock<qa_sched::SilentSource>();
        auto&                       sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        source._onSwapRequest                               = [&scheduler] { std::ignore = scheduler.exchange(qa_sched::makeReplacementGraph()); };
        const std::shared_ptr<gr::Sequence> retiredProgress = source.progress;

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));

        std::vector<std::string> pending;
        for (std::size_t i = 0UZ; i < kAroundSwap; ++i) {
            if (i == kAroundSwap / 2UZ) {
                gr::sendMessage<gr::message::Command::Set>(toScheduler, source.unique_name, "swapGraph", gr::property_map{});
            }
            pending.push_back(std::format("around-swap-{}", i));
            gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, gr::block::property::kSetting, gr::property_map{}, pending.back());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(qa_sched::awaitCondition([] { return qa_sched::gCountedSourceCalls.load(std::memory_order_acquire) > 0UZ; })) << "the replacement graph never ran";
        const bool allAnswered = qa_sched::awaitCondition([&] {
            auto messages = fromScheduler.streamReader().get();
            for (const gr::Message& message : messages) {
                std::erase(pending, message.clientRequestID);
            }
            std::ignore = messages.consume(messages.size());
            return pending.empty();
        });
        expect(allAnswered) << std::format("{} requests sent around the swap were never answered", pending.size());

        const std::size_t         retiredValue = retiredProgress->value();
        const std::vector<double> times        = qa_sched::replyTimes(fromScheduler, "after-swap", [&](std::string_view requestId) { gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, gr::block::property::kSetting, gr::property_map{}, requestId); });
        scheduler.requestStop();
        runner.join();

        const double medianMs = qa_sched::median(times);
        std::println("settings request to a parked run after a graph swap (timeout_ms {}): reply after median {:.2f} ms, min {:.2f}, max {:.2f}", qa_sched::kParkTimeoutMs, medianMs, std::ranges::min(times), std::ranges::max(times));
        expect(lt(medianMs, qa_sched::kWakeBoundMs)) << std::format("after the swap the reply waited {:.1f} ms of a {} ms park", medianMs, qa_sched::kParkTimeoutMs);
        expect(eq(retiredProgress->value(), retiredValue)) << "a message advanced the retired graph's progress sequence";
    };

    // Every message pass publishes a heartbeat from the worker's own thread. The heartbeat waits for the next pass, and
    // the run parks after each pass. The idle window is wall-clock time.
    "a heartbeat subscriber on a parked run leaves the run parked"_test = [] {
        constexpr auto        kIdleWindow = std::chrono::milliseconds(2 * qa_sched::kParkTimeoutMs);
        constexpr std::size_t kCallBound  = 4UZ * static_cast<std::size_t>(kIdleWindow.count()) / qa_sched::kParkTimeoutMs; // four times one call per park
        qa_sched::gCountedSourceCalls.store(0UZ);

        gr::Graph flow;
        auto&     source   = flow.emplaceBlock<qa_sched::SilentSource>();
        auto&     sink     = flow.emplaceBlock<qa_sched::CountingSink>();
        source._countCalls = true;
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string sinkName(sink.unique_name);

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortOut              toScheduler;
        gr::MsgPortIn               fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));
        gr::sendMessage<gr::message::Command::Subscribe>(toScheduler, sinkName, gr::block::property::kHeartbeat, gr::property_map{}, "heartbeat-client");
        expect(qa_sched::awaitCondition([&fromScheduler] { return qa_sched::takeHeartbeats(fromScheduler) > 0UZ; })) << "no heartbeat reached the subscriber";

        const std::size_t callsBefore = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire);
        std::this_thread::sleep_for(kIdleWindow);
        const std::size_t calls      = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire) - callsBefore;
        const std::size_t heartbeats = qa_sched::takeHeartbeats(fromScheduler);
        scheduler.requestStop();
        runner.join();

        std::println("parked run with a heartbeat subscriber over {} ms (timeout_ms {}): {} work() calls, {} heartbeats", kIdleWindow.count(), qa_sched::kParkTimeoutMs, calls, heartbeats);
        expect(gt(heartbeats, 0UZ)) << "the heartbeats stopped during the idle window";
        expect(lt(calls, kCallBound)) << std::format("{} work() calls in {} ms idle: the run did not park", calls, kIdleWindow.count());
    };

#ifdef GR_TEST_WITH_BLOCK_LIBRARY
    // The sink comes from a shared object that the test loads, the way a graph file's blocks arrive. It publishes its
    // heartbeat on the worker's thread with code from that object. The heartbeat waits for the next pass, as an
    // in-program block's does. The idle windows are wall-clock time
    "a heartbeat subscriber on a block from a shared object leaves the parked run parked"_test = [] {
        constexpr auto        kIdleWindow = std::chrono::milliseconds(2 * qa_sched::kParkTimeoutMs);
        constexpr std::size_t kCallBound  = 4UZ * static_cast<std::size_t>(kIdleWindow.count()) / qa_sched::kParkTimeoutMs; // four times one call per park
        expect(fatal(qa_sched::loadCrossObjectLibrary())) << "the shared object did not load";
        qa_sched::gCountedSourceCalls.store(0UZ);
        auto [flow, sinkName] = qa_sched::libraryBlockGraph(false);

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortOut              toScheduler;
        gr::MsgPortIn               fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));
        const std::size_t idleBefore = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire);
        std::this_thread::sleep_for(kIdleWindow);
        const std::size_t callsWithout = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire) - idleBefore;

        gr::sendMessage<gr::message::Command::Subscribe>(toScheduler, sinkName, gr::block::property::kHeartbeat, gr::property_map{}, "heartbeat-client");
        expect(qa_sched::awaitCondition([&fromScheduler] { return qa_sched::takeHeartbeats(fromScheduler) > 0UZ; })) << "no heartbeat reached the subscriber";
        const std::size_t callsBefore = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire);
        std::this_thread::sleep_for(kIdleWindow);
        const std::size_t callsWith  = qa_sched::gCountedSourceCalls.load(std::memory_order_acquire) - callsBefore;
        const std::size_t heartbeats = qa_sched::takeHeartbeats(fromScheduler);
        scheduler.requestStop();
        runner.join();

        std::println("parked run with a block from a shared object over {} ms (timeout_ms {}): {} work() calls without a heartbeat subscriber, {} with one and {} heartbeats", kIdleWindow.count(), qa_sched::kParkTimeoutMs, callsWithout, callsWith, heartbeats);
        expect(lt(callsWithout, kCallBound)) << std::format("{} work() calls in {} ms idle without a subscriber: the run did not park", callsWithout, kIdleWindow.count());
        expect(gt(heartbeats, 0UZ)) << "the heartbeats stopped during the idle window";
        expect(lt(callsWith, kCallBound)) << std::format("{} work() calls in {} ms idle with a heartbeat subscriber, {} without: the run did not park", callsWith, kIdleWindow.count(), callsWithout);
    };

    // The scheduler forwards a request to every block of the graph, and each block counts the message pass in which
    // it handled one. Both blocks come from the shared object, which counts with code of its own. The pass that
    // handled the request wakes the worker, and the next pass forwards the reply
    "a settings request to a block from a shared object in a parked run is answered within a tenth of the park"_test = [] {
        constexpr double kBlockReplyBoundMs = static_cast<double>(qa_sched::kParkTimeoutMs) / 10.0;
        expect(fatal(qa_sched::loadCrossObjectLibrary())) << "the shared object did not load";
        auto [flow, sinkName] = qa_sched::libraryBlockGraph(true);

        qa_sched::BlockingScheduler scheduler(qa_sched::parkingSettings());
        gr::MsgPortOut              toScheduler;
        gr::MsgPortIn               fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
        expect(qa_sched::awaitCondition([&scheduler] { return scheduler.state() == RUNNING; }));

        const std::vector<double> times = qa_sched::replyTimes(fromScheduler, "library-settings", [&](std::string_view requestId) { gr::sendMessage<gr::message::Command::Get>(toScheduler, sinkName, gr::block::property::kSetting, gr::property_map{}, requestId); });
        scheduler.requestStop();
        runner.join();

        const double medianMs = qa_sched::median(times);
        std::println("settings request to a block from a shared object in a parked run (timeout_ms {}): reply after median {:.2f} ms, min {:.2f}, max {:.2f}", qa_sched::kParkTimeoutMs, medianMs, std::ranges::min(times), std::ranges::max(times));
        expect(lt(medianMs, kBlockReplyBoundMs)) << std::format("the reply of a block from a shared object waited {:.1f} ms of a {} ms park", medianMs, qa_sched::kParkTimeoutMs);
    };
#endif

    // the graph stalls. A request ends the worker's park and leaves the graph's progress unchanged
    "a client polling a stalled parked run leaves the stall visible to the watchdog"_test = [] {
        const auto [reported, nPolls] = qa_sched::stallReportedWhilePolling(false);
        expect(reported) << std::format("{} requests hid the stalled graph from the watchdog", nPolls);
    };

    // the scheduler forwards each request to the block, and the block handles it on the worker
    "a client polling a block of a stalled parked run leaves the stall visible to the watchdog"_test = [] {
        const auto [reported, nPolls] = qa_sched::stallReportedWhilePolling(true);
        expect(reported) << std::format("{} requests to a block hid the stalled graph from the watchdog", nPolls);
    };
};

const boost::ut::suite<"workers that park"> parkingWorkerTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // Every pool case runs on a pool of its own with a thread for each job list the case needs. The default CPU pool
    // follows the host's core count and may hold fewer.
    constexpr std::uint32_t kParkingThreads = 4U;

    // A park lasts up to kParkTimeout. A worker that returns within kWakeBound, a third of the park, was therefore
    // woken. A message, an edit, a handoff or a pause must reach a parked worker within kPromptBound, a tenth of the
    // park. The watchdog's period lies beyond both.
    constexpr auto         kParkTimeout = std::chrono::milliseconds(3000);
    constexpr auto         kWakeBound   = kParkTimeout / 3;
    constexpr auto         kPromptBound = kParkTimeout / 10;
    const gr::property_map parkingSettings{{"timeout_ms", static_cast<gr::Size_t>(kParkTimeout.count())}, {"watchdog_timeout", gr::Size_t(60'000)}};
    const gr::property_map poolParkingSettings{{"timeout_ms", static_cast<gr::Size_t>(kParkTimeout.count())}, {"watchdog_timeout", gr::Size_t(60'000)}, {"poolName", std::string(qa_sched::kParkingPoolName)}};

    // Each block of the chain gets a job list of its own. The three workers downstream of the source park while the
    // source's worker waits inside work() for the gate. The test opens the gate without a progress notify. The source's
    // publication is then the only wake the parked workers can receive before their parks time out.
    auto deliversEverySample = [&]<typename TScheduler>(std::string_view schedulerName) {
        auto              pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<bool> open{false};

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::GatedFiniteSource>();
        auto&     first  = flow.emplaceBlock<qa_sched::PassThrough>();
        auto&     second = flow.emplaceBlock<qa_sched::PassThrough>();
        auto&     sink   = flow.emplaceBlock<qa_sched::OrderedSink>();
        source._open     = &open;
        expect(flow.connect<"out", "in">(source, first).has_value());
        expect(flow.connect<"out", "in">(first, second).has_value());
        expect(flow.connect<"out", "in">(second, sink).has_value());

        TScheduler scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool       allParked = false;
        const bool completed = qa_sched::runAndWaitWithin(scheduler, kWakeBound, [&] {
            allParked = qa_sched::awaitParkedWorkers(scheduler, std::size_t{kParkingThreads} - 1UZ);
            open.store(true, std::memory_order_release);
        });
        expect(eq(scheduler.jobs()->size(), std::size_t{kParkingThreads})) << schedulerName << "one job list per block";
        expect(allParked) << schedulerName << "the three workers downstream of the source did not all park";
        expect(completed) << schedulerName << "the finite graph waited for a park to time out";
        expect(eq(sink._nInOrder, qa_sched::kFiniteSamples)) << schedulerName << "samples delivered in order";
    };

    "a chain of parked workers delivers every sample under multiThreadedBlocking on Simple"_test       = [&] { deliversEverySample.operator()<qa_sched::PoolParkingScheduler>("Simple"); };
    "a chain of parked workers delivers every sample under multiThreadedBlocking on BreadthFirst"_test = [&] { deliversEverySample.operator()<qa_sched::PoolParkingBreadthFirst>("BreadthFirst"); };
    "a chain of parked workers delivers every sample under multiThreadedBlocking on DepthFirst"_test   = [&] { deliversEverySample.operator()<qa_sched::PoolParkingDepthFirst>("DepthFirst"); };

    // A pool worker at the idle back-off's longest sleep calls its blocks every few hundred microseconds. A parked
    // one calls them only when the progress sequence moves, a wake arrives or the park times out, and none of these
    // happens here.
    "an idle pool worker parks and calls no work while nothing moves"_test = [&] {
        auto                     pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&     sink    = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased = &nReleased;
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::PoolParkingScheduler scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        constexpr auto kQuietInterval = std::chrono::milliseconds(100);
        bool           allParked      = false;
        std::size_t    nCallsParked   = 0UZ;
        std::size_t    nCallsLater    = 0UZ;
        const bool     completed      = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            allParked    = qa_sched::awaitParkedWorkers(scheduler, 2UZ);
            nCallsParked = source._nWorkCalls.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(kQuietInterval);
            nCallsLater = source._nWorkCalls.load(std::memory_order_relaxed);
            scheduler.requestStop();
        });
        expect(completed);
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "source and sink on two workers";
        expect(allParked) << "both workers park while nothing moves";
        expect(gt(nCallsParked, 0UZ)) << "the source ran before its worker parked";
        expect(eq(nCallsLater, nCallsParked)) << std::format("the parked source's worker called work() {} times in {} ms", nCallsLater - nCallsParked, kQuietInterval.count());
        expect(eq(sink._nReceived, 0UZ));
    };

    "a stop while every pool worker is parked returns before any park times out"_test = [&] {
        auto                     pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&     middle  = flow.emplaceBlock<qa_sched::PassThrough>();
        auto&     sink    = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased = &nReleased;
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        qa_sched::PoolParkingScheduler scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool       allParked = false;
        const bool stopped   = qa_sched::runAndWaitWithin(scheduler, kWakeBound, [&] {
            allParked = qa_sched::awaitParkedWorkers(scheduler, 3UZ);
            scheduler.requestStop();
        });
        expect(eq(scheduler.jobs()->size(), 3UZ)) << "one job list per block";
        expect(allParked) << "the three workers did not all park";
        expect(stopped) << "a parked worker held the run after the stop";
        expect(eq(sink._nReceived, 0UZ));
    };

    // The source's worker waits inside work() for a release while the sink's worker parks. The test releases a
    // sample without a progress notify. The source's publication must wake the sink's worker, since no park times out
    // within kWakeBound.
    "a source that resumes while the sink's pool worker is parked has its samples at the sink before the park times out"_test = [&] {
        auto                     pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<std::size_t> nReleased{0UZ};
        std::atomic<bool>        holding{true};

        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<qa_sched::HeldSource>();
        auto&     sink    = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased = &nReleased;
        source._holding   = &holding;
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::PoolParkingScheduler scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        constexpr std::size_t kRounds   = 3UZ;
        std::size_t           nArrived  = 0UZ;
        bool                  allParked = true;
        std::string           misses;
        const bool            completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            for (std::size_t round = 1UZ; round <= kRounds; ++round) {
                allParked = allParked && qa_sched::awaitParkedWorkers(scheduler, 1UZ);
                nReleased.fetch_add(1UZ, std::memory_order_acq_rel);
                if (qa_sched::awaitCondition([&sink, round] { return gr::atomic_ref(sink._nReceived).load_acquire() == round; }, kWakeBound)) {
                    ++nArrived;
                } else {
                    misses += std::format(" round {}: {} samples at the sink, {} releases unpublished, {} workers parked;", round, gr::atomic_ref(sink._nReceived).load_acquire(), nReleased.load(), scheduler.graph().progress().nTimedWaiters());
                }
            }
            holding.store(false, std::memory_order_release);
            scheduler.requestStop();
        });
        expect(completed);
        expect(allParked) << "the sink's worker parks before each release";
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "source and sink on two workers";
        expect(eq(nArrived, kRounds)) << "a released sample waited for a park to time out:" << misses;
    };

    // The first job list ends at once, and its worker leaves. A request to the scheduler then arrives from the test's
    // thread while the two remaining workers are parked. Its publication into msgIn wakes them, a worker answers, and
    // both workers park again.
    "a request sent while the remaining pool workers are parked is answered within a tenth of the park, and the workers park again"_test = [&] {
        auto                     pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph flow;
        auto&     ending     = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     endingSink = flow.emplaceBlock<qa_sched::CountingSink>();
        auto&     source     = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&     sink       = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased    = &nReleased;
        expect(flow.connect<"out", "in">(ending, endingSink).has_value());
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string endingName{ending.unique_name};

        qa_sched::PoolParkingProbe scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());

        bool       firstListEnded  = false;
        bool       remainingParked = false;
        bool       answered        = false;
        bool       parkedAgain     = false;
        double     answerMs        = 0.0;
        const bool completed       = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            firstListEnded    = qa_sched::awaitCondition([&scheduler] { return scheduler.nRunningJobs() == 2UZ; });
            remainingParked   = qa_sched::awaitParkedWorkers(scheduler, 2UZ);
            const auto sentAt = std::chrono::steady_clock::now();
            gr::sendMessage<gr::message::Command::Set>(toScheduler, "", gr::scheduler::property::kSchedulerInspect, gr::property_map{});
            answered    = qa_sched::awaitReply(fromScheduler, gr::scheduler::property::kSchedulerInspected, kWakeBound);
            answerMs    = qa_sched::Millis(std::chrono::steady_clock::now() - sentAt).count();
            parkedAgain = qa_sched::awaitParkedWorkers(scheduler, 2UZ);
            scheduler.requestStop();
        });
        std::println("request to a pool of parked workers (timeout_ms {}): answered after {:.2f} ms", kParkTimeout.count(), answerMs);
        expect(completed);
        expect(fatal(eq(scheduler.jobs()->size(), 4UZ))) << "one job list per block";
        expect(std::ranges::any_of(scheduler.jobs()->front(), [&endingName](const std::shared_ptr<gr::BlockModel>& block) { return block->uniqueName() == endingName; })) << "the ending source is not in the first job list";
        expect(firstListEnded) << "two workers did not leave";
        expect(remainingParked) << "the two remaining workers did not park";
        expect(answered) << "no worker answered the request";
        expect(lt(answerMs, static_cast<double>(kPromptBound.count()))) << std::format("the answer waited {:.1f} ms of a {} ms park", answerMs, kParkTimeout.count());
        expect(parkedAgain) << "the remaining workers did not park again after the answer";
    };

    // A source and a sink on two job lists park both workers. The test's thread then hands a block to a job list, the
    // way an EmplaceBlock message does or a worker that leaves its loop with the block still queued for its list. The
    // block comes from a graph of its own, and its calls move nothing. The hand-over is then the only wake the parked
    // worker that owns it can receive.
    auto handedBlockRuns = [&](bool viaLeavingWorker) {
        auto                     pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph                       holder;
        auto&                           handed      = holder.emplaceBlock<qa_sched::FirstCallSource>();
        std::shared_ptr<gr::BlockModel> handedModel = holder.blocks().front();
        gr::Graph                       flow;
        auto&                           source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                           sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased                      = &nReleased;
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::PoolParkingProbe scheduler(poolParkingSettings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool       allParked = false;
        bool       ran       = false;
        double     delayMs   = 0.0;
        const bool completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            allParked         = qa_sched::awaitParkedWorkers(scheduler, 2UZ);
            const auto handAt = std::chrono::steady_clock::now();
            if (viaLeavingWorker) {
                expect(handedModel->changeStateTo(INITIALISED).has_value());
                expect(handedModel->changeStateTo(RUNNING).has_value());
                scheduler.queueAndClose(0UZ, handedModel);
            } else {
                scheduler.adopt(handedModel);
            }
            ran     = qa_sched::awaitCondition([&handed] { return handed._called.load(std::memory_order_acquire); });
            delayMs = ran ? qa_sched::Millis(handed._firstCallAt - handAt).count() : 0.0;
            scheduler.requestStop();
        });
        expect(handedModel->changeStateTo(REQUESTED_STOP).has_value());
        expect(handedModel->changeStateTo(STOPPED).has_value());

        std::println("block handed to a parked pool worker {} (timeout_ms {}): first call after {:.2f} ms", viaLeavingWorker ? "by a closing job list" : "by adoption", kParkTimeout.count(), delayMs);
        expect(completed);
        expect(eq(scheduler.jobs()->size(), 2UZ)) << "source and sink on two workers";
        expect(allParked) << "both workers park while nothing moves";
        expect(ran) << "the handed block was never called";
        expect(lt(delayMs, static_cast<double>(kPromptBound.count()))) << std::format("the handed block waited {:.1f} ms of a {} ms park", delayMs, kParkTimeout.count());
    };

    "a block adopted while every pool worker is parked runs within a tenth of the park"_test                 = [&] { handedBlockRuns(false); };
    "a block that a leaving worker hands to a parked worker's job list runs within a tenth of the park"_test = [&] { handedBlockRuns(true); };

    // A parent's edit holds the work quiescence of each scheduler in its graph. The test's thread holds it here. A block
    // adopted meanwhile wakes the parked workers. The worker that owns the block takes it into its job list, calls no
    // work() while the quiescence holds, and parks again. The end of the quiescence is then the only wake that can lead
    // to the block's first call before the park times out.
    auto adoptedUnderQuiescenceRuns = [&]<typename TProbe>(std::string_view policyName, const gr::property_map& settings, std::size_t nWorkers) {
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph                       holder;
        auto&                           handed      = holder.emplaceBlock<qa_sched::FirstCallSource>();
        std::shared_ptr<gr::BlockModel> handedModel = holder.blocks().front();
        gr::Graph                       flow;
        auto&                           source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                           sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        source._nReleased                      = &nReleased;
        expect(flow.connect<"out", "in">(source, sink).has_value());

        TProbe scheduler(settings);
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool                  allParked       = false;
        bool                  taken           = false;
        bool                  parkedAgain     = false;
        bool                  calledWhileHeld = false;
        std::optional<double> delayMs;
        const bool            completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            allParked = qa_sched::awaitParkedWorkers(scheduler, nWorkers);
            scheduler.requestWorkQuiescence();
            scheduler.adopt(handedModel);
            taken                 = qa_sched::awaitCondition([&scheduler] { return scheduler.adoptionQueuesEmpty(); });
            parkedAgain           = qa_sched::awaitParkedWorkers(scheduler, nWorkers);
            calledWhileHeld       = handed._called.load(std::memory_order_acquire);
            const auto releasedAt = std::chrono::steady_clock::now();
            scheduler.releaseWorkQuiescence();
            if (qa_sched::awaitCondition([&handed] { return handed._called.load(std::memory_order_acquire); })) {
                delayMs = qa_sched::Millis(handed._firstCallAt - releasedAt).count();
            }
            scheduler.requestStop();
        });
        expect(handedModel->changeStateTo(REQUESTED_STOP).has_value());
        expect(handedModel->changeStateTo(STOPPED).has_value());

        std::println("block adopted under a held work quiescence, {} (timeout_ms {}): {}", policyName, kParkTimeout.count(), delayMs.has_value() ? std::format("first call {:.2f} ms after the release", *delayMs) : std::string("never called"));
        expect(completed) << policyName;
        expect(allParked) << policyName << "the workers did not all park";
        expect(taken) << policyName << "no worker took the adopted block";
        expect(parkedAgain) << policyName << "the woken worker did not park again";
        expect(!calledWhileHeld) << policyName << "the adopted block ran while the quiescence held";
        expect(lt(delayMs.value_or(static_cast<double>(kParkTimeout.count())), static_cast<double>(kPromptBound.count()))) << policyName << "the end of the quiescence left the worker parked";
    };

    "a block adopted during a held work quiescence runs within a tenth of the park after the quiescence ends under singleThreadedBlocking"_test = [&] { adoptedUnderQuiescenceRuns.operator()<qa_sched::BlockingProbe>("singleThreadedBlocking", parkingSettings, 1UZ); };
    "a block adopted during a held work quiescence runs within a tenth of the park after the quiescence ends under multiThreadedBlocking"_test  = [&] {
        auto pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        adoptedUnderQuiescenceRuns.operator()<qa_sched::PoolParkingProbe>("multiThreadedBlocking", poolParkingSettings, 2UZ);
    };

    // The test's thread runs the scheduler's message service, and the message reaches msgIn without a wake (see
    // serveOnCallingThread()). The handler's own wake is then the only one the parked worker that owns the affected
    // block can receive before its park times out. Each case times the block's response from the serve.
    auto respondsAfterServe = [&]<typename TProbe>(std::string_view what, const gr::property_map& settings, gr::Graph flow, std::size_t nWorkers, auto serve, auto respondedAt) {
        TProbe         scheduler(settings);
        gr::MsgPortOut toScheduler;
        gr::MsgPortIn  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool                  allParked = false;
        std::optional<double> delayMs;
        const bool            completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            allParked           = qa_sched::awaitParkedWorkers(scheduler, nWorkers);
            const auto servedAt = std::chrono::steady_clock::now();
            serve(scheduler, toScheduler);
            std::optional<std::chrono::steady_clock::time_point> at;
            if (qa_sched::awaitCondition([&] { return (at = respondedAt(scheduler)).has_value(); })) {
                delayMs = qa_sched::Millis(*at - servedAt).count();
            }
            scheduler.requestStop();
        });
        std::println("{} on a parked run (timeout_ms {}): {}", what, kParkTimeout.count(), delayMs.has_value() ? std::format("after {:.2f} ms", *delayMs) : std::string("no response"));
        expect(completed) << what;
        expect(eq(scheduler.jobs()->size(), nWorkers)) << what << "one job list per block";
        expect(allParked) << what << "the workers did not all park";
        expect(delayMs.has_value()) << what << "the block did not respond";
        expect(lt(delayMs.value_or(static_cast<double>(kParkTimeout.count())), static_cast<double>(kPromptBound.count()))) << what << std::format("waited of a {} ms park", kParkTimeout.count());
    };
    auto poolRespondsAfterServe = [&](std::string_view what, gr::Graph flow, std::size_t nWorkers, auto serve, auto respondedAt) {
        auto pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        respondsAfterServe.operator()<qa_sched::PoolParkingProbe>(what, poolParkingSettings, std::move(flow), nWorkers, serve, respondedAt);
    };
    auto stoppedAt = [](const gr::BlockModel* block) -> std::optional<std::chrono::steady_clock::time_point> { return block != nullptr && block->state() == STOPPED ? std::optional(std::chrono::steady_clock::now()) : std::nullopt; };

    "a message forwarded to a block from a thread that runs none of the pool's blocks reaches it within a tenth of the park"_test = [&] {
        gr::Graph         flow;
        auto&             source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&             sink   = flow.emplaceBlock<qa_sched::MessageStampSink>();
        const std::string sinkName(sink.unique_name);
        expect(flow.connect<"out", "in">(source, sink).has_value());
        poolRespondsAfterServe(
            "message forwarded to a block", std::move(flow), 2UZ, //
            [&](qa_sched::PoolParkingProbe& scheduler, gr::MsgPortOut& toScheduler) { qa_sched::serveOnCallingThread(scheduler, [&] { gr::sendMessage<gr::message::Command::Set>(toScheduler, sinkName, "stamp", gr::property_map{}); }); }, [&sink](auto&) { return sink._stamped.load(std::memory_order_acquire) ? std::optional(sink._stampedAt) : std::nullopt; });
    };

    // a source whose output loses its last reader requests its stop in its next work() call
    "a source whose only edge a message removes stops within a tenth of the park"_test = [&] {
        gr::Graph             flow;
        auto&                 source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                 sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        const std::string     sourceName(source.unique_name);
        const gr::BlockModel* sourceModel = flow.blocks().front().get();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        poolRespondsAfterServe(
            "source whose only edge is removed", std::move(flow), 2UZ, //
            [&](qa_sched::PoolParkingProbe& scheduler, gr::MsgPortOut& toScheduler) {
                using namespace gr::serialization_fields;
                qa_sched::serveOnCallingThread(scheduler, [&] { gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, gr::scheduler::property::kRemoveEdge, gr::property_map{{std::pmr::string(EDGE_SOURCE_BLOCK), sourceName}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}}); });
            },
            [&](auto&) { return stoppedAt(sourceModel); });
    };

    // the edge from the second source to the first sink displaces the first source's only edge
    "a source whose only edge an emplaced edge displaces stops within a tenth of the park"_test = [&] {
        gr::Graph             flow;
        auto&                 first      = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                 firstSink  = flow.emplaceBlock<qa_sched::CountingSink>();
        auto&                 second     = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                 secondSink = flow.emplaceBlock<qa_sched::CountingSink>();
        const gr::BlockModel* firstModel = flow.blocks().front().get();
        const std::string     secondName(second.unique_name);
        const std::string     firstSinkName(firstSink.unique_name);
        expect(flow.connect<"out", "in">(first, firstSink).has_value());
        expect(flow.connect<"out", "in">(second, secondSink).has_value());
        poolRespondsAfterServe(
            "source whose only edge is displaced", std::move(flow), 4UZ, //
            [&](qa_sched::PoolParkingProbe& scheduler, gr::MsgPortOut& toScheduler) {
                using namespace gr::serialization_fields;
                const gr::property_map edge{{std::pmr::string(EDGE_SOURCE_BLOCK), secondName}, {std::pmr::string(EDGE_SOURCE_PORT), std::string("out")}, {std::pmr::string(EDGE_DESTINATION_BLOCK), firstSinkName}, {std::pmr::string(EDGE_DESTINATION_PORT), std::string("in")}, //
                    {std::pmr::string(EDGE_MIN_BUFFER_SIZE), gr::undefined_Size}, {std::pmr::string(EDGE_WEIGHT), std::int32_t{0}}, {std::pmr::string(EDGE_NAME), std::string("displacing")}};
                qa_sched::serveOnCallingThread(scheduler, [&] { gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, gr::scheduler::property::kEmplaceEdge, edge); });
            },
            [&](auto&) { return stoppedAt(firstModel); });
    };

    // the removed sink reaches STOPPED in its own worker's next work() call (see BlockingSink)
    auto removedBlockStops = [&]<typename TProbe>(std::string_view what, const gr::property_map& settings, std::size_t nWorkers) {
        gr::Graph                       flow;
        auto&                           source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                           sink   = flow.emplaceBlock<qa_sched::BlockingSink>();
        const std::string               sinkName(sink.unique_name);
        std::shared_ptr<gr::BlockModel> sinkModel = flow.blocks().back();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        respondsAfterServe.operator()<TProbe>(
            what, settings, std::move(flow), nWorkers, //
            [&](TProbe& scheduler, gr::MsgPortOut& toScheduler) { qa_sched::serveOnCallingThread(scheduler, [&] { gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, gr::scheduler::property::kRemoveBlock, gr::property_map{{"uniqueName", sinkName}}); }); }, [&](auto&) { return stoppedAt(sinkModel.get()); });
    };

    "a block that a message removes stops within a tenth of the park"_test = [&] {
        auto pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, kParkingThreads);
        removedBlockStops.operator()<qa_sched::PoolParkingProbe>("removed block", poolParkingSettings, 2UZ);
    };
    "a block that a message served on another thread removes from a parked singleThreadedBlocking run stops within a tenth of the park"_test = [&] { removedBlockStops.operator()<qa_sched::BlockingProbe>("removed block, singleThreadedBlocking", parkingSettings, 1UZ); };

    // the scheduler starts the added scheduler on a pool of its own and queues it for a job list. The added
    // scheduler's graph moves nothing. The queueing is then the only wake
    "a sub-scheduler adopted while every pool worker is parked is called within a tenth of the park"_test = [&] {
        auto      innerPool = qa_sched::fixedPool(qa_sched::kAdoptionPoolName, 2U);
        gr::Graph innerFlow;
        auto&     innerSource = innerFlow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&     innerSink   = innerFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::CalledSubScheduler>>(gr::property_map{{"poolName", std::string(qa_sched::kAdoptionPoolName)}, {"watchdog_timeout", gr::Size_t(60'000)}});
        inner->setGraph(std::move(innerFlow));
        const std::shared_ptr<gr::BlockModel> innerBlock = gr::SchedulerModel::asBlockModelPtr(inner);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        poolRespondsAfterServe(
            "sub-scheduler adopted", std::move(flow), 2UZ, //
            [&](qa_sched::PoolParkingProbe& scheduler, gr::MsgPortOut&) { scheduler.adopt(innerBlock); }, [&inner](auto&) { return inner->blockRef()._called.load(std::memory_order_acquire) ? std::optional(inner->blockRef()._firstCallAt) : std::nullopt; });
        inner->stop();
    };

    // The graph's plugin loader creates the replacement from the global registry. The replaced sink reaches STOPPED in
    // its own worker's next work() call (see BlockingSink).
    "a block that a message replaces stops within a tenth of the park"_test = [&] {
        std::ignore = gr::globalBlockRegistry().insert<qa_sched::CountingSink>();
        const std::string replacementType(gr::meta::type_name<qa_sched::CountingSink>());

        gr::Graph                       flow;
        auto&                           source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                           sink   = flow.emplaceBlock<qa_sched::BlockingSink>();
        const std::string               sinkName(sink.unique_name);
        std::shared_ptr<gr::BlockModel> sinkModel = flow.blocks().back();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        poolRespondsAfterServe(
            "replaced block", std::move(flow), 2UZ, //
            [&](qa_sched::PoolParkingProbe& scheduler, gr::MsgPortOut& toScheduler) { qa_sched::serveOnCallingThread(scheduler, [&] { gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler.unique_name, gr::scheduler::property::kReplaceBlock, gr::property_map{{"uniqueName", sinkName}, {"type", replacementType}}); }); }, [&](auto&) { return stoppedAt(sinkModel.get()); });
    };

    // The source's first work() call holds the run's one worker until the pause reaches the source's pause() hook, or,
    // with `spansPause`, until pause() waits for the work() calls in progress. The scheduler reads REQUESTED_PAUSE before
    // pause() runs. A single-threaded worker reads the state in a message pass once per process_stream_to_message_ratio
    // passes. Its park falls due a few passes after the release, before its next message pass. The state read at the
    // park keeps it from parking on the RUNNING it read before the pause: in the sweep, where the hook would see the
    // park, and after PAUSED, where no wake follows.
    auto pauseParksNoWorker = [&]<typename TScheduler>(std::string_view caseName, bool spansPause, const gr::property_map& settings) {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::PauseProbeSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        source._releaseInPause = !spansPause;

        TScheduler scheduler(settings);
        expect(scheduler.exchange(std::move(flow)).has_value());
        source._progress = std::addressof(scheduler.graph().progress());
        if (spansPause) {
            source._pauseWaits = [&scheduler] { return scheduler.state() == REQUESTED_PAUSE && scheduler.workQuiescenceRequested(); };
        }

        bool       paused           = false;
        bool       parkedAfterPause = false;
        const bool completed        = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            std::ignore = qa_sched::awaitCondition([&source] { return source._inside.load(std::memory_order_acquire); });
            expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
            paused = scheduler.state() == PAUSED;
            source.release();
            parkedAfterPause = qa_sched::awaitCondition([&scheduler] { return scheduler.graph().progress().nTimedWaiters() > 0U; }, kPromptBound);
            scheduler.requestStop();
        });
        const bool parkedInSweep    = source._parkedInSweep.load(std::memory_order_acquire);
        std::println("pause, {} (timeout_ms {}): parked during the sweep {}, parked within {} ms of PAUSED {}", caseName, kParkTimeout.count(), parkedInSweep, kPromptBound.count(), parkedAfterPause);
        expect(completed) << caseName;
        expect(paused) << caseName << "the pause did not publish PAUSED";
        expect(!parkedInSweep) << caseName << "a worker parked while the pause swept the blocks";
        expect(!parkedAfterPause) << caseName << "a worker parked after the scheduler read PAUSED";
    };

    "a singleThreadedBlocking worker does not park while a pause sweeps the blocks"_test                              = [&] { pauseParksNoWorker.operator()<qa_sched::BlockingProbe>("singleThreadedBlocking, released in the sweep", false, parkingSettings); };
    "a singleThreadedBlocking worker whose work() call spans the start of a pause does not park after the pause"_test = [&] { pauseParksNoWorker.operator()<qa_sched::BlockingProbe>("singleThreadedBlocking, work() spanning the start of the pause", true, parkingSettings); };
    // no pool worker parks in the sweep or within a tenth of the park after PAUSED
    "no pool worker parks while a pause sweeps the blocks under multiThreadedBlocking"_test = [&] {
        auto pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, 1U);
        pauseParksNoWorker.operator()<qa_sched::PoolParkingProbe>("multiThreadedBlocking, released in the sweep", false, poolParkingSettings);
    };
    "a pool worker whose work() call spans the start of a pause does not park after the pause under multiThreadedBlocking"_test = [&] {
        auto pool = qa_sched::fixedPool(qa_sched::kParkingPoolName, 1U);
        pauseParksNoWorker.operator()<qa_sched::PoolParkingProbe>("multiThreadedBlocking, work() spanning the start of the pause", true, poolParkingSettings);
    };

    // Each block of a chain has a worker of its own, and the sink has a heartbeat subscriber. The sink's worker
    // publishes a heartbeat in each of its message passes, and a later message pass forwards it. Nothing advances the
    // progress sequence. A parked worker returns only when its park times out and then calls its one block once. The
    // subscription restarts the idle back-off of the worker that reads it. The idle window opens once every worker has
    // parked, and it is wall-clock time.
    auto heartbeatLeavesPoolParked = [&](std::uint32_t nThreads) {
        constexpr auto           kShortPark  = std::chrono::milliseconds(200);
        constexpr auto           kIdleWindow = std::chrono::milliseconds(600);
        const std::size_t        callBound   = 4UZ * nThreads * static_cast<std::size_t>(kIdleWindow / kShortPark); // four times one call per worker and park
        auto                     pool        = qa_sched::fixedPool(qa_sched::kParkingPoolName, nThreads);
        std::atomic<std::size_t> nReleased{0UZ};

        gr::Graph                           flow;
        auto&                               source = flow.emplaceBlock<qa_sched::ReleasedSource>();
        auto&                               sink   = flow.emplaceBlock<qa_sched::CallCountingSink>();
        std::vector<qa_sched::PassThrough*> middle;
        for (std::uint32_t i = 2U; i < nThreads; ++i) {
            middle.push_back(std::addressof(flow.emplaceBlock<qa_sched::PassThrough>()));
        }
        source._nReleased = &nReleased;
        if (middle.empty()) {
            expect(flow.connect<"out", "in">(source, sink).has_value());
        } else {
            expect(flow.connect<"out", "in">(source, *middle.front()).has_value());
            for (std::size_t i = 1UZ; i < middle.size(); ++i) {
                expect(flow.connect<"out", "in">(*middle[i - 1UZ], *middle[i]).has_value());
            }
            expect(flow.connect<"out", "in">(*middle.back(), sink).has_value());
        }
        const std::string sinkName(sink.unique_name);
        auto              chainWorkCalls = [&source, &sink, &middle] {
            std::size_t sum = source._nWorkCalls.load(std::memory_order_relaxed) + sink._nWorkCalls.load(std::memory_order_relaxed);
            for (const qa_sched::PassThrough* block : middle) {
                sum += block->_nWorkCalls.load(std::memory_order_relaxed);
            }
            return sum;
        };

        gr::property_map settings = poolParkingSettings;
        settings.insert_or_assign("timeout_ms", static_cast<gr::Size_t>(kShortPark.count()));
        qa_sched::PoolParkingScheduler scheduler(settings);
        gr::MsgPortOut                 toScheduler;
        gr::MsgPortIn                  fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        bool        heartbeatArrived = false;
        bool        allParked        = false;
        std::size_t calls            = 0UZ;
        std::size_t heartbeats       = 0UZ;
        std::size_t nSamples         = 0UZ;
        std::size_t nParkedSamples   = 0UZ;
        const bool  completed        = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound, [&] {
            gr::sendMessage<gr::message::Command::Subscribe>(toScheduler, sinkName, gr::block::property::kHeartbeat, gr::property_map{}, "heartbeat-client");
            heartbeatArrived = qa_sched::awaitCondition([&] { return qa_sched::takeHeartbeats(fromScheduler) > 0UZ; });
            allParked        = qa_sched::awaitParkedWorkers(scheduler, std::size_t{nThreads});
            std::ignore      = qa_sched::takeHeartbeats(fromScheduler); // the window counts only its own heartbeats

            const std::size_t callsBefore = chainWorkCalls();
            const auto        windowEnd   = std::chrono::steady_clock::now() + kIdleWindow;
            while (std::chrono::steady_clock::now() < windowEnd) {
                ++nSamples;
                nParkedSamples += scheduler.graph().progress().nTimedWaiters() == nThreads ? 1UZ : 0UZ;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            calls      = chainWorkCalls() - callsBefore;
            heartbeats = qa_sched::takeHeartbeats(fromScheduler);
            scheduler.requestStop();
        });

        std::println("parked pool of {} workers with a heartbeat subscriber over {} ms (timeout_ms {}): {} work() calls of the chain, bound {}, {} heartbeats, every worker parked in {} of {} samples", nThreads, kIdleWindow.count(), kShortPark.count(), calls, callBound, heartbeats, nParkedSamples, nSamples);
        expect(completed);
        expect(eq(scheduler.jobs()->size(), std::size_t{nThreads})) << "one job list per block";
        expect(heartbeatArrived) << "no heartbeat reached the subscriber";
        expect(allParked) << "the workers did not all park after the first heartbeat";
        expect(gt(heartbeats, 0UZ)) << "the heartbeats stopped during the idle window";
        expect(lt(calls, callBound)) << std::format("{} work() calls of the chain in {} ms idle: a worker did not park", calls, kIdleWindow.count());
    };

    "a heartbeat subscriber on a parked multiThreadedBlocking run of two workers leaves the run parked"_test  = [&] { heartbeatLeavesPoolParked(2U); };
    "a heartbeat subscriber on a parked multiThreadedBlocking run of four workers leaves the run parked"_test = [&] { heartbeatLeavesPoolParked(4U); };

    // The source ends its stream behind items that the relay never takes. The relay then drains, and it ends after a
    // bound of calls in which nothing it waits on moved. Nothing else moves. A worker parked between those calls would
    // therefore call the relay once per park. Under multiThreaded the bound passes at the idle back-off's pace.
    auto untakenRemainderEnds = [&]<typename TScheduler>(std::string_view policyName) {
        constexpr std::string_view kDrainPoolName = "qa_drain_cpu";
        auto                       pool           = qa_sched::fixedPool(kDrainPoolName, 3U);

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::DoneSource>();
        auto&     relay  = flow.emplaceBlock<qa_sched::UntakenRemainderRelay>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        expect(flow.connect<"out", "in">(source, relay).has_value());
        expect(flow.connect<"out", "in">(relay, sink).has_value());

        TScheduler scheduler({{"timeout_ms", static_cast<gr::Size_t>(kParkTimeout.count())}, {"watchdog_timeout", gr::Size_t(60'000)}, {"poolName", std::string(kDrainPoolName)}});
        expect(scheduler.exchange(std::move(flow)).has_value());

        const auto startedAt = std::chrono::steady_clock::now();
        const bool completed = qa_sched::runAndWaitWithin(scheduler, qa_sched::kEventBound);
        const auto elapsedMs = qa_sched::Millis(std::chrono::steady_clock::now() - startedAt).count();
        std::println("untaken remainder, {} (timeout_ms {}): ended {} after {:.0f} ms, {} calls of the relay", policyName, kParkTimeout.count(), completed, elapsedMs, relay._nCalls);
        expect(completed) << policyName << "the drain waited for parks to time out";
        expect(eq(source._nEmitted, qa_sched::kSamplesBeforeTerminal)) << policyName;
        expect(gt(relay._nCalls, 1UZ)) << policyName << "the relay was not offered its remainder";
        expect(eq(sink._nReceived, 0UZ)) << policyName;
    };

    "a block that never takes its remainder ends the graph under multiThreaded"_test                         = [&] { untakenRemainderEnds.operator()<qa_sched::TestScheduler>("multiThreaded"); };
    "a block that never takes its remainder keeps its worker from parking under singleThreadedBlocking"_test = [&] { untakenRemainderEnds.operator()<qa_sched::BlockingScheduler>("singleThreadedBlocking"); };
    "a block that never takes its remainder keeps its worker from parking under multiThreadedBlocking"_test  = [&] { untakenRemainderEnds.operator()<qa_sched::PoolParkingScheduler>("multiThreadedBlocking"); };
};

namespace qa_sched {

// moves nothing, and each of its work() calls takes about a millisecond. The calls are counted in gCountedSourceCalls
struct SlowIdleSource : gr::Block<SlowIdleSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SlowIdleSource, out);

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        gCountedSourceCalls.fetch_add(1UZ, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        outSpan.publish(0UZ);
        return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
    }
};

using BlockingWakeProbe = WakeProbe<BlockingScheduler>;

// runs an idle graph under singleThreadedBlocking with a park that outlasts the case. With kWakes set, it waits until
// the watchdog has woken the worker that many times. Without, it waits until the worker has parked and `window` has
// passed. It returns the source's work() calls, the wakes, and the time it waited
struct IdleRun {
    std::size_t                         nCalls = 0UZ;
    std::size_t                         nWakes = 0UZ;
    std::chrono::steady_clock::duration elapsed{};
};

constexpr gr::Size_t kIdleInactivityCount = 20U;

[[nodiscard]] inline IdleRun runIdle(gr::Size_t watchdogMs, std::size_t nWakesToAwait, std::chrono::steady_clock::duration window) {
    using namespace boost::ut;
    gCountedSourceCalls.store(0UZ);

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SlowIdleSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    BlockingWakeProbe scheduler({{"timeout_ms", gr::Size_t(60'000)}, {"timeout_inactivity_count", kIdleInactivityCount}, {"watchdog_timeout", watchdogMs}});
    expect(scheduler.exchange(std::move(flow)).has_value());

    std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
    const auto  startedAt = std::chrono::steady_clock::now();
    if (nWakesToAwait > 0UZ) {
        expect(awaitCondition([&scheduler, nWakesToAwait] { return scheduler.nWakes() >= nWakesToAwait; })) << "the watchdog did not wake the idle run";
    } else {
        expect(awaitParkedWorkers(scheduler, 1UZ)) << "the idle run's worker did not park";
        std::this_thread::sleep_until(startedAt + window);
    }
    IdleRun result{gCountedSourceCalls.load(std::memory_order_acquire), scheduler.nWakes(), std::chrono::steady_clock::now() - startedAt};
    scheduler.requestStop();
    runner.join();
    return result;
}

} // namespace qa_sched

const boost::ut::suite<"a watchdog wake"> watchdogWakeTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // The watchdog wakes a parked worker once per period without progress. The worker calls every block once and parks
    // again, because the wake leaves the inactivity count where it was. The control runs with a watchdog period beyond
    // its window: its worker parks once after the inactivity count and stays parked
    "a watchdog wake does not reset the inactivity count"_test = [] {
        constexpr std::size_t kWakes = 10UZ;

        const qa_sched::IdleRun woken   = qa_sched::runIdle(gr::Size_t(10), kWakes, {});
        const qa_sched::IdleRun control = qa_sched::runIdle(gr::Size_t(60'000), 0UZ, woken.elapsed);

        std::println("idle singleThreadedBlocking run: {} work() calls and {} wakes with a 10 ms watchdog, {} calls and {} wakes without one", woken.nCalls, woken.nWakes, control.nCalls, control.nWakes);
        expect(eq(control.nWakes, 0UZ)) << "the control's watchdog woke its worker";
        expect(le(control.nCalls, std::size_t(qa_sched::kIdleInactivityCount) + 2UZ)) << "the control's worker did not park after the inactivity count";
        // a wake that lands during a pass ends the next park at once, so each wake adds at most two passes
        expect(le(woken.nCalls, control.nCalls + 2UZ * woken.nWakes)) << std::format("{} work() calls for {} watchdog wakes: a wake restarted the inactivity count", woken.nCalls, woken.nWakes);
    };

    // The watchdog reads the state once per period and reports a stall after that read. Its next read comes at least
    // one period after the report. A pause and a resume between the two leave the stall marked as reported. The graph
    // makes no progress throughout, and the watchdog reports the stall only once. The report reaches the test after
    // the last poll that found none. An attempt whose resume ends a period or more after that poll may have met the
    // next read. The case then repeats the attempt on a new run. After three such attempts the case prints that it
    // skipped its check.
    "a pause and a resume during a stall leave the stall report intact once RUNNING again"_test = [] {
        constexpr std::size_t kStalledPeriods = 2UZ;
        constexpr std::size_t kAttempts       = 3UZ;
        constexpr auto        kPeriod         = std::chrono::milliseconds(100);

        auto attempt = [&](std::size_t nAttempt) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::ReleasedSource>(); // never released: the graph stalls
            auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            qa_sched::WakeProbe<qa_sched::TestScheduler> scheduler({{"watchdog_timeout", static_cast<gr::Size_t>(kPeriod.count())}, {"timeout_inactivity_count", gr::Size_t(kStalledPeriods)}});
            gr::MsgPortIn                                fromScheduler;
            expect(scheduler.msgOut.connect(fromScheduler).has_value());
            expect(scheduler.exchange(std::move(flow)).has_value());
            expect(scheduler.changeStateTo(INITIALISED).has_value());
            expect(scheduler.changeStateTo(RUNNING).has_value());

            qa_sched::StallReports                               reports;
            std::optional<std::chrono::steady_clock::time_point> lastEmptyPoll;
            expect(qa_sched::awaitCondition([&] {
                const auto polledAt = std::chrono::steady_clock::now();
                reports.take(fromScheduler);
                if (reports.count == 0UZ) {
                    lastEmptyPoll = polledAt;
                }
                return reports.count >= 1UZ;
            })) << "the stall produced no report";

            const std::size_t progressBefore = scheduler.graph().progress().value();
            expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
            expect(qa_sched::awaitState(scheduler, PAUSED)) << "the scheduler did not pause";
            expect(scheduler.changeStateTo(RUNNING).has_value());
            const auto resumedAt = std::chrono::steady_clock::now();
            expect(eq(scheduler.graph().progress().value(), progressBefore)) << "the pause and the resume moved the progress sequence";

            const bool beforeNextRead = lastEmptyPoll.has_value() && resumedAt - *lastEmptyPoll < kPeriod;
            if (beforeNextRead) {
                const std::size_t nWakesToAwait = scheduler.nWakes() + 2UZ * kStalledPeriods + 2UZ;
                expect(qa_sched::awaitCondition([&scheduler, nWakesToAwait] { return scheduler.nWakes() >= nWakesToAwait; })) << "the watchdog stopped observing the stall";
                reports.take(fromScheduler);
                expect(eq(reports.count, 1UZ)) << "the stall was reported again after the pause and the resume";
            } else {
                std::println("attempt {}: the resume did not end within one watchdog period of the last poll before the report", nAttempt + 1UZ);
            }

            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(scheduler, STOPPED)) << "the stalled graph did not stop";
            return beforeNextRead;
        };

        bool tested = false;
        for (std::size_t nAttempt = 0UZ; nAttempt < kAttempts && !tested; ++nAttempt) {
            tested = attempt(nAttempt);
        }
        if (!tested) {
            std::println("a pause and a resume during a stall: skipped, no attempt of {} resumed within one watchdog period of the last poll before the report", kAttempts);
        }
    };
};

namespace qa_sched {

constexpr std::size_t kSamplesPerRun = 64UZ;

// publishes kSamplesPerRun samples after each start and then idles without ending the run
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    std::size_t _nEmitted = 0UZ;

    void start() { _nEmitted = 0UZ; }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const std::size_t nPublish = std::min(outSpan.size(), kSamplesPerRun - _nEmitted);
        _nEmitted += nPublish;
        outSpan.publish(nPublish);
        return nPublish == 0UZ ? gr::work::Status::INSUFFICIENT_INPUT_ITEMS : gr::work::Status::OK;
    }
};

struct Forwarder : gr::Block<Forwarder> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(Forwarder, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

} // namespace qa_sched

const boost::ut::suite<"a sub-scheduler's exported stream ports"> exportedPortTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // The parent connects the edges into and out of the sub-scheduler's exported ports before it starts the
    // sub-scheduler on its own thread. The sub-scheduler's start reconnects its own graph and must leave those
    // two connections in place.
    "a sub-scheduler's exported ports carry every sample of the parent's edges, run after run"_test = [] {
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        gr::Graph innerFlow;
        auto&     first  = innerFlow.emplaceBlock<qa_sched::Forwarder>();
        auto&     second = innerFlow.emplaceBlock<qa_sched::Forwarder>();
        expect(innerFlow.connect<"out", "in">(first, second).has_value());
        auto inner = std::make_shared<gr::SchedulerWrapper<qa_sched::TestScheduler>>();
        inner->setGraph(std::move(innerFlow));
        expect(inner->exportPort(true, std::string(first.unique_name), gr::PortDirection::INPUT, "in", "in").has_value());
        expect(inner->exportPort(true, std::string(second.unique_name), gr::PortDirection::OUTPUT, "out", "out").has_value());

        gr::Graph                             outerFlow;
        auto&                                 source  = outerFlow.emplaceBlock<qa_sched::CountedSource>();
        auto&                                 sink    = outerFlow.emplaceBlock<qa_sched::ObservedSink>();
        const std::shared_ptr<gr::BlockModel> managed = outerFlow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        expect(outerFlow.connect(outerFlow.blocks()[0], gr::PortDefinition{"out"}, managed, gr::PortDefinition{"in"}).has_value());
        expect(outerFlow.connect(managed, gr::PortDefinition{"out"}, outerFlow.blocks()[1], gr::PortDefinition{"in"}).has_value());

        qa_sched::TestScheduler parent;
        expect(parent.exchange(std::move(outerFlow)).has_value());

        for (std::size_t run = 1UZ; run <= 2UZ; ++run) {
            expect(parent.changeStateTo(INITIALISED).has_value()) << std::format("run {} did not initialize", run);
            expect(parent.changeStateTo(RUNNING).has_value()) << std::format("run {} did not start", run);

            const std::size_t nExpected = run * qa_sched::kSamplesPerRun;
            expect(qa_sched::awaitCondition([nExpected] { return qa_sched::gObservedSamples.load(std::memory_order_relaxed) >= nExpected; })) << std::format("run {} did not deliver its samples", run);
            expect(eq(qa_sched::gObservedSamples.load(std::memory_order_relaxed), nExpected)) << std::format("run {}: the sink count differs from the source count", run);
            expect(eq(source.out.nReaders(), 1UZ)) << std::format("run {}: the edge into the exported input has no reader", run);
            expect(eq(second.out.nReaders(), 1UZ)) << std::format("run {}: the edge out of the exported output has no reader", run);
            expect(sink.in.isConnected()) << std::format("run {}: the sink lost its input", run);

            expect(parent.changeStateTo(REQUESTED_STOP).has_value());
            expect(qa_sched::awaitState(parent, STOPPED)) << std::format("run {} did not stop", run);
        }
    };
};

namespace qa_sched {

using BreadthFirstScheduler = gr::scheduler::BreadthFirst<gr::scheduler::ExecutionPolicy::multiThreaded>;
using DepthFirstScheduler   = gr::scheduler::DepthFirst<gr::scheduler::ExecutionPolicy::multiThreaded>;

// the threads that ran each block's processOne(), keyed by the block
inline std::mutex                                       gProcessingThreadsMutex;
inline std::map<const void*, std::set<std::thread::id>> gProcessingThreads;

struct ThreadRecordingForwarder : gr::Block<ThreadRecordingForwarder> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ThreadRecordingForwarder, in, out);

    float processOne(float value) {
        std::lock_guard lock(gProcessingThreadsMutex);
        gProcessingThreads[this].insert(std::this_thread::get_id());
        return value;
    }
};

[[nodiscard]] std::size_t nProcessingThreads(const void* block) {
    std::lock_guard lock(gProcessingThreadsMutex);
    const auto      found = gProcessingThreads.find(block);
    return found == gProcessingThreads.end() ? 0UZ : found->second.size();
}

// The parent's source feeds a sub-scheduler whose graph is a chain of three blocks, and the chain feeds the parent's
// sink. The sub-scheduler runs the chain on its own thread. The parent's job lists hold the sub-scheduler once and none
// of the chain's blocks, in every order.
template<typename TParent>
void checkSubSchedulerChainStaysInside() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    {
        std::lock_guard lock(gProcessingThreadsMutex);
        gProcessingThreads.clear();
    }

    gr::Graph innerFlow;
    auto&     first  = innerFlow.emplaceBlock<ThreadRecordingForwarder>();
    auto&     middle = innerFlow.emplaceBlock<ThreadRecordingForwarder>();
    auto&     last   = innerFlow.emplaceBlock<ThreadRecordingForwarder>();
    expect(innerFlow.connect<"out", "in">(first, middle).has_value());
    expect(innerFlow.connect<"out", "in">(middle, last).has_value());
    auto inner = std::make_shared<gr::SchedulerWrapper<SerialScheduler>>();
    inner->setGraph(std::move(innerFlow));
    expect(inner->exportPort(true, std::string(first.unique_name), gr::PortDirection::INPUT, "in", "in").has_value());
    expect(inner->exportPort(true, std::string(last.unique_name), gr::PortDirection::OUTPUT, "out", "out").has_value());

    gr::Graph outerFlow;
    std::ignore                                 = outerFlow.emplaceBlock<DoneSource>();
    auto&                                 sink  = outerFlow.emplaceBlock<CountingSink>();
    const std::shared_ptr<gr::BlockModel> child = outerFlow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
    expect(outerFlow.connect(outerFlow.blocks()[0], gr::PortDefinition{"out"}, child, gr::PortDefinition{"in"}).has_value());
    expect(outerFlow.connect(child, gr::PortDefinition{"out"}, outerFlow.blocks()[1], gr::PortDefinition{"in"}).has_value());

    TParent parent;
    expect(parent.exchange(std::move(outerFlow)).has_value());
    expect(parent.changeStateTo(INITIALISED).has_value());

    const std::array<std::string, 3UZ> chainNames{std::string(first.unique_name), std::string(middle.unique_name), std::string(last.unique_name)};
    std::size_t                        nChainBlocksListed = 0UZ;
    std::size_t                        nChildListed       = 0UZ;
    for (const auto& jobList : *parent.jobs()) {
        for (const auto& block : jobList) {
            nChainBlocksListed += static_cast<std::size_t>(std::ranges::count(chainNames, block->uniqueName()));
            nChildListed += block == child ? 1UZ : 0UZ;
        }
    }
    expect(eq(nChainBlocksListed, 0UZ)) << "the parent's job lists hold blocks of the sub-scheduler's chain";
    expect(eq(nChildListed, 1UZ)) << "the parent's job lists must hold the sub-scheduler once";
    if (nChainBlocksListed != 0UZ) {
        return; // the run would call work() on the chain's blocks from two threads at once
    }

    expect(runAndWaitWithin(parent, kEventBound)) << "the run did not complete";
    expect(eq(sink._nReceived, kSamplesBeforeTerminal)) << "the sink count differs from the source count";
    for (const ThreadRecordingForwarder* block : {&first, &middle, &last}) {
        expect(eq(nProcessingThreads(block), 1UZ)) << std::format("{} must run on the sub-scheduler's thread alone", block->unique_name);
    }
}

// The parent's graph is one sub-scheduler and holds no edge. The sub-scheduler's graph is a finite source and a sink.
// The parent's job lists hold the sub-scheduler once and no block of its graph. The sub-scheduler runs its stream to
// the end.
template<typename TParent>
void checkUnconnectedSubSchedulerRuns() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    gSubSchedulerSamples.store(0UZ, std::memory_order_relaxed);

    gr::Graph innerFlow;
    auto&     innerSource = innerFlow.emplaceBlock<DoneSource>();
    auto&     innerSink   = innerFlow.emplaceBlock<SharedCountingSink>();
    expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
    auto inner = std::make_shared<gr::SchedulerWrapper<SerialScheduler>>();
    inner->setGraph(std::move(innerFlow));

    gr::Graph                             outerFlow;
    const std::shared_ptr<gr::BlockModel> child = outerFlow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));

    TParent parent;
    expect(parent.exchange(std::move(outerFlow)).has_value());
    expect(parent.changeStateTo(INITIALISED).has_value());

    const std::array<std::string, 2UZ> innerNames{std::string(innerSource.unique_name), std::string(innerSink.unique_name)};
    std::size_t                        nInnerBlocksListed = 0UZ;
    std::size_t                        nChildListed       = 0UZ;
    for (const auto& jobList : *parent.jobs()) {
        for (const auto& block : jobList) {
            nInnerBlocksListed += static_cast<std::size_t>(std::ranges::count(innerNames, block->uniqueName()));
            nChildListed += block == child ? 1UZ : 0UZ;
        }
    }
    expect(gt(parent.jobs()->size(), 0UZ)) << "the parent holds no job list";
    expect(eq(nInnerBlocksListed, 0UZ)) << "the parent's job lists hold blocks of the sub-scheduler's graph";
    expect(eq(nChildListed, 1UZ)) << "the parent's job lists must hold the sub-scheduler once";
    if (parent.jobs()->empty()) {
        return; // the start requires at least one job list
    }

    expect(parent.changeStateTo(RUNNING).has_value());
    expect(awaitCondition([] { return gSubSchedulerSamples.load(std::memory_order_relaxed) >= kSamplesBeforeTerminal; })) << "the sub-scheduler's stream did not reach its end";
    expect(parent.changeStateTo(REQUESTED_STOP).has_value());
    expect(awaitCondition([&parent] { return parent.state() == STOPPED; })) << "the parent did not stop";
    expect(eq(gSubSchedulerSamples.load(std::memory_order_relaxed), kSamplesBeforeTerminal)) << "the sink count differs from the source count";
}

} // namespace qa_sched

const boost::ut::suite<"a sub-scheduler in its parent's execution order"> subSchedulerOrderTests = [] {
    using namespace boost::ut;

    "a simple parent leaves the sub-scheduler's chain to the sub-scheduler"_test = [] { qa_sched::checkSubSchedulerChainStaysInside<qa_sched::TestScheduler>(); };

    "a breadth-first parent leaves the sub-scheduler's chain to the sub-scheduler"_test = [] { qa_sched::checkSubSchedulerChainStaysInside<qa_sched::BreadthFirstScheduler>(); };

    "a depth-first parent leaves the sub-scheduler's chain to the sub-scheduler"_test = [] { qa_sched::checkSubSchedulerChainStaysInside<qa_sched::DepthFirstScheduler>(); };

    "a simple parent runs a sub-scheduler that has no edge in the parent's graph"_test = [] { qa_sched::checkUnconnectedSubSchedulerRuns<qa_sched::TestScheduler>(); };

    "a breadth-first parent runs a sub-scheduler that has no edge in the parent's graph"_test = [] { qa_sched::checkUnconnectedSubSchedulerRuns<qa_sched::BreadthFirstScheduler>(); };

    "a depth-first parent runs a sub-scheduler that has no edge in the parent's graph"_test = [] { qa_sched::checkUnconnectedSubSchedulerRuns<qa_sched::DepthFirstScheduler>(); };
};

namespace qa_sched {

// the number of samples the loop's blocks found at their inputs. Each block consumes what it finds and publishes nothing.
// The count therefore holds the samples primed on the loop's edges.
inline std::atomic<std::size_t> gLoopSamples{0UZ};

struct LoopAbsorber : gr::Block<LoopAbsorber> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LoopAbsorber, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        gLoopSamples.fetch_add(inSpan.size(), std::memory_order_relaxed);
        std::ignore = inSpan.consume(inSpan.size());
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

struct Loop {
    LoopAbsorber* front;
    LoopAbsorber* back;
};

[[nodiscard]] Loop addLoop(gr::Graph& flow) {
    using namespace boost::ut;

    auto& front = flow.emplaceBlock<LoopAbsorber>();
    auto& back  = flow.emplaceBlock<LoopAbsorber>();
    expect(flow.connect<"out", "in">(front, back).has_value());
    expect(flow.connect<"out", "in">(back, front).has_value());
    return {&front, &back};
}

struct LoopStart {
    std::size_t nPrimedSamples     = 0UZ;
    std::size_t nPrimingErrors     = 0UZ;
    std::size_t nPrimedAfterResume = 0UZ;
};

// the start primes before the first work() call on the loop. The count is final once a sample has arrived and both
// inputs are empty.
[[nodiscard]] LoopStart startLoop(gr::Graph flow, Loop loop) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    gLoopSamples.store(0UZ, std::memory_order_relaxed);
    TestScheduler scheduler;
    gr::MsgPortIn fromScheduler;
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.changeStateTo(INITIALISED).has_value());
    expect(scheduler.changeStateTo(RUNNING).has_value());
    expect(awaitCondition([&loop] { return gLoopSamples.load(std::memory_order_relaxed) > 0UZ && loop.front->in.available() == 0UZ && loop.back->in.available() == 0UZ; })) << "the primed samples did not drain";

    LoopStart result{.nPrimedSamples = gLoopSamples.load(std::memory_order_relaxed)};
    auto      messages    = fromScheduler.streamReader().get();
    result.nPrimingErrors = static_cast<std::size_t>(std::ranges::count_if(messages, [](const gr::Message& message) { return message.endpoint == "connectPendingEdges()" && !message.data.has_value(); }));
    std::ignore           = messages.consume(messages.size());

    // the resume primes before it returns. Both inputs empty after it means the loop consumed what the resume primed.
    expect(scheduler.changeStateTo(REQUESTED_PAUSE).has_value());
    expect(awaitState(scheduler, PAUSED)) << "the loop's scheduler did not pause";
    expect(scheduler.changeStateTo(RUNNING).has_value());
    expect(awaitCondition([&loop] { return loop.front->in.available() == 0UZ && loop.back->in.available() == 0UZ; })) << "the samples primed at the resume did not drain";
    result.nPrimedAfterResume = gLoopSamples.load(std::memory_order_relaxed);

    expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
    expect(awaitState(scheduler, STOPPED)) << "the loop's scheduler did not stop";
    return result;
}

} // namespace qa_sched

const boost::ut::suite<"a feedback loop inside a group"> nestedLoopPrimingTests = [] {
    using namespace boost::ut;

    "a loop inside a transparent subgraph or a sub-scheduler is primed as often as one at the top level"_test = [] {
        gr::Graph                 topFlow;
        const qa_sched::Loop      topLoop = qa_sched::addLoop(topFlow);
        const qa_sched::LoopStart top     = qa_sched::startLoop(std::move(topFlow), topLoop);
        expect(gt(top.nPrimedSamples, 0UZ)) << "a loop at the top level must be primed";

        auto                 wrapper   = std::make_shared<gr::GraphWrapper<gr::Graph>>();
        const qa_sched::Loop groupLoop = qa_sched::addLoop(*wrapper->graph());
        gr::Graph            groupFlow;
        std::ignore                     = groupFlow.addBlock(wrapper);
        const qa_sched::LoopStart group = qa_sched::startLoop(std::move(groupFlow), groupLoop);
        expect(eq(group.nPrimedSamples, top.nPrimedSamples)) << "a loop inside a transparent subgraph";
        expect(eq(group.nPrimingErrors, top.nPrimingErrors)) << "priming errors for a loop inside a transparent subgraph";
        expect(eq(group.nPrimedAfterResume, top.nPrimedAfterResume)) << "a loop inside a transparent subgraph after a pause and resume";

        gr::Graph            innerFlow;
        const qa_sched::Loop childLoop = qa_sched::addLoop(innerFlow);
        auto                 inner     = std::make_shared<gr::SchedulerWrapper<qa_sched::SerialScheduler>>();
        inner->setGraph(std::move(innerFlow));
        gr::Graph childFlow;
        std::ignore                     = childFlow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const qa_sched::LoopStart child = qa_sched::startLoop(std::move(childFlow), childLoop);
        expect(eq(child.nPrimedSamples, top.nPrimedSamples)) << "a loop inside a sub-scheduler";
        expect(eq(child.nPrimingErrors, top.nPrimingErrors)) << "priming errors for a loop inside a sub-scheduler";
        expect(eq(child.nPrimedAfterResume, top.nPrimedAfterResume)) << "a loop inside a sub-scheduler after a pause and resume";
    };
};

namespace qa_sched {

// requests a graph swap from its message handler, which runs on a worker of the scheduler. Its reset() hook runs
// inside that swap while the scheduler reads STOPPED, after the swap's check of the destruction flag and before the swap restarts
// the scheduler
struct SwapRequestingSource : gr::Block<SwapRequestingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SwapRequestingSource, out);

    std::function<void()> _onSwapRequest;
    std::function<void()> _onReset;

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }

    void reset() {
        if (_onReset) {
            _onReset();
        }
    }

    void processMessages(const gr::MsgPortInBuiltin&, std::span<const gr::Message> messages) {
        for (const gr::Message& message : messages) {
            if (message.endpoint == "swapGraph" && _onSwapRequest) {
                _onSwapRequest();
            }
        }
    }
};

// publishes samples until the test sets its end flag, then ends its stream. Its first work() call runs a test-supplied
// action
struct EndableSource : gr::Block<EndableSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndableSource, out);

    std::atomic<bool>*    _end = nullptr;
    std::function<void()> _onFirstWork;
    bool                  _worked = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_worked) {
            _worked = true;
            if (_onFirstWork) {
                _onFirstWork();
            }
        }
        if (_end != nullptr && _end->load(std::memory_order_acquire)) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        outSpan.publish(std::min(outSpan.size(), 8UZ));
        return gr::work::Status::OK;
    }
};

[[nodiscard]] gr::Graph makeEndableGraph(std::atomic<bool>* end, std::function<void()> onFirstWork = {}) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<EndableSource>();
    auto&     sink      = flow.emplaceBlock<CountingSink>();
    source._end         = end;
    source._onFirstWork = std::move(onFirstWork);
    expect(flow.connect<"out", "in">(source, sink).has_value());
    return flow;
}

// exposes the flag that the destructor sets under the swap's lock, and the count of swaps that run outside the job
// count. The flag is read through a pointer, because the destructor of the derived probe has run when the scheduler's
// own destructor sets it
template<typename TScheduler>
struct DestructionProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] bool*       destroyingFlag() noexcept { return &this->_run.destroying; }
    [[nodiscard]] std::size_t deferredSwaps() { return gr::atomic_ref(this->_nDeferredExchanges).load_acquire(); }
};

[[nodiscard]] bool readsDestroying(bool* flag) { return gr::atomic_ref(*flag).load_acquire(); }

void requestSwap(SwapRequestingSource& block) {
    gr::Message request;
    request.cmd      = gr::message::Command::Set;
    request.endpoint = "swapGraph";
    request.data     = gr::property_map{};

    auto writer = block.msgIn.buffer().streamBuffer.new_writer();
    auto span   = writer.tryReserve<gr::SpanReleasePolicy::ProcessAll>(1UZ);
    boost::ut::expect(!span.empty()) << "could not queue the swap request";
    span[0] = std::move(request);
    span.publish(1UZ);
}

struct TimedDestruction {
    bool                      inTime = false;
    std::chrono::milliseconds elapsed{0};
};

// destroys the scheduler on its own thread. On a miss it sets the end flag, which ends the stream of the endable
// source that the destruction waits for. The case then fails instead of hanging, and no call reaches the scheduler
template<typename TScheduler>
[[nodiscard]] TimedDestruction destroyWithin(std::optional<TScheduler>& scheduler, std::atomic<bool>& end, std::chrono::milliseconds bound) {
    std::mutex              mutex;
    std::condition_variable finished;
    bool                    destroyed = false;
    const auto              begin     = std::chrono::steady_clock::now();
    std::thread             destroyer([&scheduler, &mutex, &finished, &destroyed] {
        scheduler.reset();
        {
            std::lock_guard lock(mutex);
            destroyed = true;
        }
        finished.notify_one();
    });

    TimedDestruction result;
    {
        std::unique_lock lock(mutex);
        result.inTime = finished.wait_for(lock, bound, [&destroyed] { return destroyed; });
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    if (!result.inTime) {
        end.store(true, std::memory_order_release);
    }
    destroyer.join();
    return result;
}

} // namespace qa_sched

const boost::ut::suite<"a stop or a destruction during a deferred graph swap"> deferredSwapDestructionTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop requested while a deferred swap holds the scheduler stopped keeps the swap from restarting it"_test = []<typename TPolicy>() {
        using TScheduler = typename TPolicy::type;

        // timed on the wall clock. A worker requests a swap to a graph whose source runs until the test ends it. The
        // old graph's reset() hook holds the swap while the scheduler reads STOPPED, until the test has requested a
        // stop. That request finds the scheduler stopped and changes no state. A single-threaded scheduler runs the
        // swap and a restarted run inside the runner's call. Within the bound the runner's call returns, the swap
        // finishes and the scheduler reads STOPPED only when the swap honors the stop.
        constexpr auto kBound = std::chrono::milliseconds(1000);

        std::atomic<bool>                                     end{false};
        std::atomic<bool>                                     inSwap{false};
        std::atomic<bool>                                     stopRequested{false};
        std::atomic<bool>                                     runnerReturned{false};
        std::optional<qa_sched::DestructionProbe<TScheduler>> scheduler;
        scheduler.emplace();
        qa_sched::DestructionProbe<TScheduler>* probe     = &*scheduler;
        qa_sched::SwapRequestingSource*         requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::SwapRequestingSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [probe, &end] { std::ignore = probe->exchange(qa_sched::makeEndableGraph(&end)); };
            source._onReset       = [&inSwap, &stopRequested] {
                if (inSwap.exchange(true)) {
                    return;
                }
                std::ignore = qa_sched::awaitCondition([&stopRequested] { return stopRequested.load(); });
            };
            requester = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler->exchange(std::move(flow)).has_value());
        }

        expect(scheduler->changeStateTo(INITIALISED).has_value());
        const std::size_t observedBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        std::thread       runner([probe, &runnerReturned] {
            std::ignore = probe->changeStateTo(RUNNING);
            runnerReturned.store(true);
        });
        expect(qa_sched::awaitObservedSamplesAbove(observedBefore)) << "the first graph never ran";

        qa_sched::requestSwap(*requester);
        expect(qa_sched::awaitCondition([&inSwap] { return inSwap.load(); })) << "the deferred swap never reset the old graph";
        expect(probe->state() == STOPPED) << "the deferred swap does not hold the scheduler stopped";

        const auto stopStart = std::chrono::steady_clock::now();
        probe->requestStop();
        stopRequested.store(true);
        const bool stopped  = qa_sched::awaitCondition([probe, &runnerReturned] { return runnerReturned.load() && probe->deferredSwaps() == 0UZ && probe->state() == STOPPED; }, kBound);
        const auto stopTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopStart);
        if (!stopped) {
            end.store(true, std::memory_order_release); // ends the restarted run, so the case fails instead of hanging
        }
        runner.join();

        expect(stopped) << std::format("the scheduler did not read STOPPED with its runner returned within {} of the stop", stopTime);
        scheduler.reset(); // after the runner's call returned
    } | std::tuple<std::type_identity<qa_sched::SerialScheduler>, std::type_identity<qa_sched::BlockingScheduler>, std::type_identity<qa_sched::TestScheduler>>{};

    "a destruction that begins inside a deferred swap refuses the restart"_test = [] {
        using TScheduler = qa_sched::TestScheduler;

        // timed on the wall clock. A worker requests a swap to a graph whose source runs until the test ends it. The
        // old graph's reset() hook holds the swap after its check of the destruction flag until the destruction has set
        // the flag and has read the state. The restart then reads the flag as set and does not run. Under a single-threaded
        // policy the swap runs inside the runner's call, and a destruction during that call is outside the scheduler's
        // contract. The case therefore runs under multiThreaded alone, where the runner's call has returned before the
        // swap.
        constexpr auto kBound = std::chrono::milliseconds(1000);

        std::atomic<bool>                                     end{false};
        std::atomic<bool>                                     resetEntered{false};
        std::optional<qa_sched::DestructionProbe<TScheduler>> scheduler;
        scheduler.emplace();
        qa_sched::DestructionProbe<TScheduler>* probe      = &*scheduler;
        bool*                                   destroying = probe->destroyingFlag();
        qa_sched::SwapRequestingSource*         requester  = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::SwapRequestingSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [probe, &end] { std::ignore = probe->exchange(qa_sched::makeEndableGraph(&end)); };
            source._onReset       = [destroying, &resetEntered] {
                if (resetEntered.exchange(true)) {
                    return;
                }
                std::ignore = qa_sched::awaitCondition([destroying] { return qa_sched::readsDestroying(destroying); });
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the destructor reads the state meanwhile
            };
            requester = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler->exchange(std::move(flow)).has_value());
        }

        expect(scheduler->changeStateTo(INITIALISED).has_value());
        const std::size_t observedBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(scheduler->changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(observedBefore)) << "the first graph never ran";

        qa_sched::requestSwap(*requester);
        expect(qa_sched::awaitCondition([&resetEntered] { return resetEntered.load(); })) << "the deferred swap never reset the old graph";

        const qa_sched::TimedDestruction destruction = qa_sched::destroyWithin(scheduler, end, kBound);
        expect(destruction.inTime) << std::format("the destruction took {} or more", destruction.elapsed);
    };

    "a destruction stops a run that a deferred swap restarted before the destruction began"_test = []<typename TPolicy>() {
        using TScheduler         = typename TPolicy::type;
        constexpr bool kOnRunner = std::is_same_v<TScheduler, qa_sched::SerialScheduler> || std::is_same_v<TScheduler, qa_sched::BlockingScheduler>;

        // timed on the wall clock. A worker requests a swap to a graph whose source runs until the test ends it. The
        // swap restarts the scheduler before the destruction begins, so the restart reads the destruction flag as unset.
        // Under multiThreaded the new source holds its first work() call until the destruction has set the flag. The
        // destruction then stops the restarted run and waits for its workers. A single-threaded scheduler runs the
        // restarted run inside the runner's call, and a destruction during that call is outside the scheduler's
        // contract. Under those policies the test stops the run, and the destruction begins after the runner's call
        // returned.
        constexpr auto kBound = std::chrono::milliseconds(1000);

        std::atomic<bool>                                     end{false};
        std::atomic<bool>                                     restartWorked{false};
        std::atomic<bool>                                     runnerReturned{false};
        std::optional<qa_sched::DestructionProbe<TScheduler>> scheduler;
        scheduler.emplace();
        qa_sched::DestructionProbe<TScheduler>* probe      = &*scheduler;
        [[maybe_unused]] bool*                  destroying = probe->destroyingFlag();
        qa_sched::SwapRequestingSource*         requester  = nullptr;

        std::function<void()> onFirstWork = [&restartWorked] { restartWorked.store(true); };
        if constexpr (!kOnRunner) {
            onFirstWork = [destroying, &restartWorked] {
                restartWorked.store(true);
                std::ignore = qa_sched::awaitCondition([destroying] { return qa_sched::readsDestroying(destroying); });
            };
        }

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::SwapRequestingSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [probe, &end, onFirstWork] { std::ignore = probe->exchange(qa_sched::makeEndableGraph(&end, onFirstWork)); };
            requester             = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler->exchange(std::move(flow)).has_value());
        }

        expect(scheduler->changeStateTo(INITIALISED).has_value());
        const std::size_t observedBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        std::thread       runner([probe, &runnerReturned] {
            std::ignore = probe->changeStateTo(RUNNING);
            runnerReturned.store(true);
        });
        expect(qa_sched::awaitObservedSamplesAbove(observedBefore)) << "the first graph never ran";

        qa_sched::requestSwap(*requester);
        expect(qa_sched::awaitCondition([&restartWorked] { return restartWorked.load(); })) << "the deferred swap never restarted the scheduler";
        if constexpr (kOnRunner) {
            probe->requestStop();
        }
        if (!qa_sched::awaitCondition([&runnerReturned] { return runnerReturned.load(); })) {
            end.store(true, std::memory_order_release);
        }
        runner.join();
        expect(runnerReturned.load()) << "the runner's call did not return";

        const qa_sched::TimedDestruction destruction = qa_sched::destroyWithin(scheduler, end, kBound);
        expect(destruction.inTime) << std::format("the destruction took {} or more", destruction.elapsed);
    } | std::tuple<std::type_identity<qa_sched::SerialScheduler>, std::type_identity<qa_sched::BlockingScheduler>, std::type_identity<qa_sched::TestScheduler>>{};
};

namespace qa_sched {

// publishes samples. Its stop() hook reports that it has begun and returns only once the test releases it, which holds
// the scheduler's stop() inside its sweep while the scheduler reads REQUESTED_STOP
struct HeldStopSource : gr::Block<HeldStopSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldStopSource, out);

    std::atomic<bool>* _entered  = nullptr;
    std::atomic<bool>* _released = nullptr;

    void stop() {
        _entered->store(true);
        _entered->notify_all();
        _released->wait(false);
    }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

} // namespace qa_sched

const boost::ut::suite<"a stop requested while exchange() stops a running graph"> plainSwapStopTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // exchange() from a thread outside the scheduler stops the running graph and then restores RUNNING with the new
    // graph. The old graph's stop() hook holds that stop while the scheduler reads REQUESTED_STOP. A stop requested
    // then changes no state. The exchange must still install the new graph and leave the scheduler STOPPED.
    "a stop requested while exchange() stops a running multiThreaded graph is kept"_test = [] {
        std::atomic<bool> stopEntered{false};
        std::atomic<bool> stopReleased{false};

        gr::Graph oldFlow;
        auto&     held    = oldFlow.emplaceBlock<qa_sched::HeldStopSource>();
        auto&     oldSink = oldFlow.emplaceBlock<qa_sched::ObservedSink>();
        expect(oldFlow.connect<"out", "in">(held, oldSink).has_value());
        held._entered  = &stopEntered; // the wrapper holding the block lives on the heap. The pointers stay valid after the move.
        held._released = &stopReleased;

        gr::Graph         newFlow;
        auto&             newSource     = newFlow.emplaceBlock<qa_sched::RaceSource>();
        auto&             newSink       = newFlow.emplaceBlock<qa_sched::CountingSink>();
        const std::string newSourceName = std::string(newSource.unique_name);
        expect(newFlow.connect<"out", "in">(newSource, newSink).has_value());

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(oldFlow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        const std::size_t observedBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(observedBefore)) << "the first graph moved no samples";

        std::atomic<bool> swapSucceeded{false};
        std::thread       swapper([&scheduler, &newFlow, &swapSucceeded] { swapSucceeded.store(scheduler.exchange(std::move(newFlow)).has_value()); });
        stopEntered.wait(false);
        expect(scheduler.state() == REQUESTED_STOP) << "the exchange's stop did not hold the scheduler in REQUESTED_STOP";
        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value()) << "the stop request was refused";
        stopReleased.store(true);
        stopReleased.notify_all();
        swapper.join();

        expect(swapSucceeded.load()) << "the exchange failed";
        expect(std::ranges::any_of(scheduler.graph().blocks(), [&newSourceName](const auto& block) { return block->uniqueName() == newSourceName; })) << "the new graph is not installed";
        expect(scheduler.state() == STOPPED) << std::format("the stop requested during the exchange was lost: the scheduler reads {}", gr::meta::enumName(scheduler.state()).value_or(""));
        if (gr::lifecycle::isActive(scheduler.state())) {
            expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        }
        expect(qa_sched::awaitState(scheduler, STOPPED));
    };
};

const boost::ut::suite<"a sub-scheduler whose graph is swapped while it runs"> subSchedulerSwapTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // the parent's graph holds a multi-threaded sub-scheduler alone. A thread outside both schedulers swaps the
    // sub-scheduler's graph. The old graph's stop() hook holds the swap after the sub-scheduler's workers have left. The
    // source of the new graph fails at once
    "a sub-scheduler whose graph another thread swaps keeps its parent's run going until it fails"_test = [] {
        std::atomic<bool> stopEntered{false};
        std::atomic<bool> stopReleased{false};

        gr::Graph oldFlow;
        auto&     held    = oldFlow.emplaceBlock<qa_sched::HeldStopSource>();
        auto&     oldSink = oldFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(oldFlow.connect<"out", "in">(held, oldSink).has_value());
        held._entered  = &stopEntered;
        held._released = &stopReleased;

        const std::atomic<bool> failureGate{true};
        gr::Graph               newFlow;
        auto&                   failingSource = newFlow.emplaceBlock<qa_sched::GatedFailingSource>();
        auto&                   newSink       = newFlow.emplaceBlock<qa_sched::CountingSink>();
        expect(newFlow.connect<"out", "in">(failingSource, newSink).has_value());
        failingSource._gate = &failureGate;
        const std::string failingName(failingSource.unique_name);

        auto inner = std::make_shared<qa_sched::CountedSchedulerWrapper<qa_sched::TestScheduler>>();
        inner->setGraph(std::move(oldFlow));

        gr::Graph                             flow;
        const std::shared_ptr<gr::BlockModel> innerBlock = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));
        const std::string                     innerName(innerBlock->uniqueName());

        qa_sched::TestScheduler scheduler;
        gr::MsgPortIn           fromScheduler;
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());

        const auto                     nCalls = [&inner] { return inner->_nReturnedWorkCalls.load(std::memory_order_acquire); };
        std::atomic<bool>              swapSucceeded{false};
        bool                           calledDuringSwap = false;
        std::thread                    swapper;
        std::expected<void, gr::Error> result;
        const bool                     ended = qa_sched::runAndWaitWithin(
            scheduler, qa_sched::kEventBound,
            [&] {
                expect(qa_sched::awaitCondition([&nCalls] { return nCalls() > 0UZ; })) << "the parent must call the sub-scheduler";
                swapper = std::thread([&inner, &newFlow, &swapSucceeded] { swapSucceeded.store(inner->blockRef().exchange(std::move(newFlow)).has_value()); });
                expect(qa_sched::awaitCondition([&stopEntered] { return stopEntered.load(); })) << "the swap must stop the sub-scheduler's run";
                expect(qa_sched::awaitCondition([&inner] { return !inner->blockRef().isProcessing(); })) << "the sub-scheduler's workers must leave for the swap";
                // a parent worker that reads the run as ended returns from at most two more calls: one that began
                // before the workers left, and the call that reports the end
                const std::size_t nCallsBefore = nCalls();
                std::ignore                    = qa_sched::awaitCondition([&] { return nCalls() >= nCallsBefore + 3UZ || scheduler.state() != RUNNING; });
                calledDuringSwap               = scheduler.state() == RUNNING && nCalls() >= nCallsBefore + 3UZ;
                stopReleased.store(true);
                stopReleased.notify_all();
            },
            &result);
        if (swapper.joinable()) {
            swapper.join();
        }
        expect(calledDuringSwap) << "the parent must keep calling the sub-scheduler while its graph is swapped";
        expect(swapSucceeded.load()) << "the swap failed";
        qa_sched::expectFailedBySubScheduler(scheduler, ended, result, innerName, failingName, inner->blockRef().state());
        inner->stop();
    };
};

namespace qa_sched {

void sendLifecycleRequest(gr::MsgPortOut& toScheduler, std::string_view schedulerName, gr::lifecycle::State state) {
    auto span           = toScheduler.streamWriter().reserve<gr::SpanReleasePolicy::ProcessAll>(1UZ);
    span[0].cmd         = gr::message::Command::Set;
    span[0].serviceName = schedulerName;
    span[0].endpoint    = gr::block::property::kLifeCycleState;
    span[0].data        = gr::property_map{{"state", std::string(gr::meta::enumName(state).value_or(""))}};
    span.publish(1UZ);
}

// runs runAndWait() on its own thread. With restartFirst it sends a stop, a reset and a start in one batch and waits for
// the restarted run to move samples. It then sends a stop by message and reports whether the runner's call returned. On
// a miss it stops the scheduler from the test thread. A scheduler whose runner still does not return is leaked, so the
// case fails instead of hanging.
template<typename TScheduler>
[[nodiscard]] bool stopByMessageEndsRun(bool restartFirst) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    gStartHooks.store(0, std::memory_order_release);
    gObservedSamples.store(0UZ, std::memory_order_relaxed);

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<RaceSource>();
    auto&     sink   = flow.emplaceBlock<ObservedSink>();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    auto scheduler = std::make_unique<TScheduler>();
    expect(scheduler->exchange(std::move(flow)).has_value());
    gr::MsgPortOut toScheduler;
    expect(toScheduler.connect(scheduler->msgIn).has_value());

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    TScheduler* raw        = scheduler.get();
    std::thread runner([raw, runnerDone] {
        std::ignore = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });
    expect(awaitObservedSamplesAbove(0UZ)) << "the first run moved no samples";
    if (restartFirst) {
        sendRestartBatch(toScheduler, scheduler->unique_name);
        expect(awaitCondition([] { return gStartHooks.load(std::memory_order_acquire) >= 2; })) << "the start in the batch never started the blocks";
        expect(awaitObservedSamplesAbove(gObservedSamples.load(std::memory_order_relaxed))) << "the restarted run moved no samples";
    }

    sendLifecycleRequest(toScheduler, scheduler->unique_name, REQUESTED_STOP);
    const bool ended = awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    if (!ended) {
        std::ignore = scheduler->changeStateTo(REQUESTED_STOP);
    }
    if (!awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); })) {
        runner.detach();
        std::ignore = scheduler.release();
        return false;
    }
    runner.join();
    return ended;
}

} // namespace qa_sched

const boost::ut::suite<"a restart applied once the workers have left"> deferredRestartTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // the control: without a restart, a stop by message ends a single-threaded run
    "a stop by message ends a single-threaded run"_test = []<typename TPolicy>() { expect(qa_sched::stopByMessageEndsRun<typename TPolicy::type>(false)) << "the stop by message did not end the run"; } | std::tuple<std::type_identity<qa_sched::SerialScheduler>, std::type_identity<qa_sched::BlockingScheduler>>{};

    // the restarted run must answer the scheduler's messages as the first run did
    "a stop by message ends a single-threaded run that a message restarted"_test = []<typename TPolicy>() { expect(qa_sched::stopByMessageEndsRun<typename TPolicy::type>(true)) << "the restarted run ignored the stop by message"; } | std::tuple<std::type_identity<qa_sched::SerialScheduler>, std::type_identity<qa_sched::BlockingScheduler>>{};

    // the worker that requested the swap leaves its loop and applies the swap on its pool thread. That thread returns
    // to the pool once the restarted run is dispatched, so the swap claims it as free
    "a graph swap requested from a worker builds as many job lists as the first start"_test = [] {
        auto pool = qa_sched::twoThreadPool();

        std::atomic<bool>               end{false};
        std::atomic<bool>               restartWorked{false};
        qa_sched::TestScheduler         scheduler({{"poolName", std::string(qa_sched::kOccupiedPoolName)}});
        qa_sched::SwapRequestingSource* requester = nullptr;

        {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_sched::SwapRequestingSource>();
            auto&     sink   = flow.emplaceBlock<qa_sched::ObservedSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            source._onSwapRequest = [&scheduler, &end, &restartWorked] { std::ignore = scheduler.exchange(qa_sched::makeEndableGraph(&end, [&restartWorked] { restartWorked.store(true); })); };
            requester             = &source; // the wrapper holding it lives on the heap, so this survives the move

            expect(scheduler.exchange(std::move(flow)).has_value());
        }

        expect(scheduler.changeStateTo(INITIALISED).has_value());
        const std::size_t observedBefore = qa_sched::gObservedSamples.load(std::memory_order_relaxed);
        expect(scheduler.changeStateTo(RUNNING).has_value());
        expect(qa_sched::awaitObservedSamplesAbove(observedBefore)) << "the first graph never ran";
        const std::size_t nFirstJobLists = scheduler.jobs()->size();
        expect(eq(nFirstJobLists, 2UZ)) << "the first start did not give each block its own pool thread";

        qa_sched::requestSwap(*requester);
        expect(qa_sched::awaitCondition([&restartWorked] { return restartWorked.load(); })) << "the deferred swap never restarted the scheduler";
        expect(eq(scheduler.jobs()->size(), nFirstJobLists)) << "the swap from a worker built fewer job lists than the first start";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the restarted run did not stop";
    };

#ifdef GR_TEST_WITH_BLOCK_LIBRARY
    // The scheduler comes from the shared object, which runs its workers and applies its restarts with code of its own.
    // The probe block asks the scheduler with this program's code. The first start runs on the caller's thread, and the
    // scheduler refuses that thread a swap while the run starts. A restart by message starts the blocks on the thread
    // that applies it, and the scheduler admits a swap from that thread
    "a scheduler from a shared object reads its own worker and its restarting thread from a program's code"_test = [] {
        expect(fatal(qa_sched::loadCrossObjectLibrary())) << "the shared object did not load";
        qa_sched::gMarkWorkCalls.store(0UZ);
        qa_sched::gMarkOwnWorkerCalls.store(0UZ);
        qa_sched::gMarkStarts.store(0UZ);
        qa_sched::gMarkFirstStartAdmitted.store(false);
        qa_sched::gMarkRestartAdmitted.store(false);

        std::unique_ptr<gr::SchedulerModel> model = gr::globalSchedulerRegistry().create(qa_sched::kLibraryScheduler, gr::property_map{});
        expect(fatal(model != nullptr));
        auto* scheduler = static_cast<gr::testing::CrossObjectScheduler*>(model->asBlockModel()->raw());

        gr::Graph flow;
        auto&     probe  = flow.emplaceBlock<qa_sched::ThreadMarkProbe>();
        auto&     sink   = flow.emplaceBlock<qa_sched::CountingSink>();
        probe._scheduler = scheduler;
        expect(flow.connect<"out", "in">(probe, sink).has_value());
        model->setGraph(std::move(flow));
        gr::MsgPortOut toScheduler;
        expect(toScheduler.connect(scheduler->msgIn).has_value());

        std::thread runner([&model] { std::ignore = model->runAndWait(); });
        expect(qa_sched::awaitCondition([] { return qa_sched::gMarkWorkCalls.load() > 0UZ; })) << "the first run made no work() call";
        qa_sched::sendRestartBatch(toScheduler, scheduler->unique_name);
        expect(qa_sched::awaitCondition([] { return qa_sched::gMarkStarts.load() >= 2UZ; })) << "the start in the batch never started the blocks";
        const std::size_t callsAfterRestart = qa_sched::gMarkWorkCalls.load();
        expect(qa_sched::awaitCondition([callsAfterRestart] { return qa_sched::gMarkWorkCalls.load() > callsAfterRestart; })) << "the restarted run made no work() call";
        gr::sendMessage<gr::message::Command::Set>(toScheduler, scheduler->unique_name, gr::block::property::kLifeCycleState, gr::property_map{{"state", std::string("REQUESTED_STOP")}});
        runner.join();

        const std::size_t nWorkCalls = qa_sched::gMarkWorkCalls.load();
        const std::size_t nOwn       = qa_sched::gMarkOwnWorkerCalls.load();
        std::println("scheduler from a shared object: {} of {} work() calls on its own worker, first start admitted a swap: {}, restart admitted a swap: {}", nOwn, nWorkCalls, qa_sched::gMarkFirstStartAdmitted.load(), qa_sched::gMarkRestartAdmitted.load());
        expect(eq(nOwn, nWorkCalls)) << "a work() call on the scheduler's worker did not read as its own worker";
        expect(!qa_sched::gMarkFirstStartAdmitted.load()) << "the caller's thread was admitted to swap the starting run";
        expect(qa_sched::gMarkRestartAdmitted.load()) << "the thread that applies the restart was refused a swap";
    };
#endif
};

const boost::ut::suite<"a child's error drained after the stop"> drainedErrorAfterStopTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // the stop arrives by message, and a worker runs it. The sink's stop hook reports an error. The same message pass
    // drains that error on the worker after the stop retired the run, with no reader on msgOut
    "a child's error drained on a worker after the stop leaves STOPPED, fails runAndWait() with the child's name and lets a restart run"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::StopActionSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());
        const std::string sinkName(sink.unique_name);
        sink._onStop = [&sink] {
            if (sink._nStopCalls == 0) {
                sink.emitErrorMessage("stop", qa_sched::kFlushFailure);
            }
        };
        qa_sched::gObservedSamples.store(0UZ, std::memory_order_relaxed);

        qa_sched::TestScheduler scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        gr::MsgPortOut toScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());

        auto runOnce = [&scheduler](auto stopRun) {
            std::expected<void, gr::Error> outcome;
            std::atomic<bool>              done{false};
            std::thread                    runner([&scheduler, &outcome, &done] {
                outcome = scheduler.runAndWait();
                done.store(true, std::memory_order_release);
            });
            expect(qa_sched::awaitObservedSamplesAbove(qa_sched::gObservedSamples.load(std::memory_order_relaxed))) << "the run moved no samples";
            stopRun();
            if (!qa_sched::awaitCondition([&done] { return done.load(std::memory_order_acquire); })) {
                std::ignore = scheduler.changeStateTo(REQUESTED_STOP);
            }
            runner.join();
            return outcome;
        };

        const auto first = runOnce([&] { qa_sched::sendLifecycleRequest(toScheduler, scheduler.unique_name, REQUESTED_STOP); });
        expect(eq(sink._nStopCalls, 1)) << "the stop by message did not reach the sink";
        expect(!first.has_value()) << "a run whose child reported an error while stopping is reported as a clean stop";
        if (!first.has_value()) {
            expect(first.error().message.find(sinkName) != std::string::npos) << "the run's error does not name the child";
            expect(first.error().message.find(qa_sched::kFlushFailure) != std::string::npos) << "the run's error does not carry the child's error";
        }
        expect(scheduler.state() == STOPPED) << std::format("the stop's STOPPED must stay the scheduler's state, it reads {}", gr::meta::enumName(scheduler.state()).value_or(""));

        const auto second = runOnce([&scheduler] { std::ignore = scheduler.changeStateTo(REQUESTED_STOP); });
        expect(second.has_value()) << "the restart after the stop failed";
        expect(eq(sink._nStopCalls, 2)) << "the restarted run did not stop";
    };
};

const boost::ut::suite<"a message output that nothing drains"> undrainedOutputTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    // A reader is connected to msgOut and never consumes. The sink's notifications fill msgOut, and those that do not
    // fit are dropped. A request to the scheduler and one to the sink arrive while msgOut is full. The run keeps
    // moving samples, and both replies arrive once the reader drains msgOut.
    "a reply to a full message output waits for room and the run continues"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<qa_sched::EndlessSource>();
        auto&     sink   = flow.emplaceBlock<qa_sched::NotifyingSink>();
        expect(flow.connect<"out", "in">(source, sink).has_value());

        qa_sched::TestScheduler scheduler;
        gr::MsgPortOut          toScheduler;
        gr::MsgPortIn           fromScheduler;
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.changeStateTo(INITIALISED).has_value());
        expect(scheduler.changeStateTo(RUNNING).has_value());

        const std::size_t capacity = fromScheduler.buffer().streamBuffer.size();
        expect(qa_sched::awaitCondition([&] { return fromScheduler.streamReader().available() >= capacity; })) << "msgOut never filled";
        const std::size_t droppedAtFull = gr::message::droppedMessageCount().load();
        expect(qa_sched::awaitCondition([&] { return gr::message::droppedMessageCount().load() > droppedAtFull; })) << "no notification was dropped from a full msgOut";

        gr::sendMessage<gr::message::Command::Get>(toScheduler, scheduler.unique_name, gr::block::property::kSetting, gr::property_map{}, "scheduler-request");
        gr::sendMessage<gr::message::Command::Get>(toScheduler, sink.unique_name, gr::block::property::kSetting, gr::property_map{}, "sink-request");
        expect(qa_sched::awaitCondition([&] { return scheduler.msgIn.streamReader().available() == 0UZ; })) << "the scheduler did not take the requests";
        const std::size_t samplesAtRequests = sink._nReceived.load();
        expect(qa_sched::awaitCondition([&] { return sink._nReceived.load() > samplesAtRequests; })) << "the run stopped moving samples after the requests";
        expect(scheduler.state() == RUNNING) << "the run left RUNNING";

        bool schedulerReplied = false;
        bool sinkReplied      = false;
        expect(qa_sched::awaitCondition([&] {
            auto messages = fromScheduler.streamReader().get();
            for (const gr::Message& message : messages) {
                schedulerReplied = schedulerReplied || (message.clientRequestID == "scheduler-request" && message.cmd == gr::message::Command::Final);
                sinkReplied      = sinkReplied || (message.clientRequestID == "sink-request" && message.cmd == gr::message::Command::Final);
            }
            std::ignore = messages.consume(messages.size());
            return schedulerReplied && sinkReplied;
        }));
        expect(schedulerReplied) << "the scheduler's reply never arrived";
        expect(sinkReplied) << "the sink's reply never arrived";

        expect(scheduler.changeStateTo(REQUESTED_STOP).has_value());
        expect(qa_sched::awaitState(scheduler, STOPPED)) << "the run did not stop";
    };

#ifdef GR_TEST_WITH_BLOCK_LIBRARY
    // The sink comes from a shared object. Its code keeps the replies that find msgOut full and drops those past the
    // bound. The reader never consumes. The program's droppedMessageCount() counts every drop.
    "the program counts the replies that a block from a shared object drops"_test = [] {
        expect(fatal(qa_sched::loadCrossObjectLibrary())) << "the shared object did not load";
        std::shared_ptr<gr::BlockModel> sink = gr::globalBlockRegistry().create(qa_sched::kLibrarySink, gr::property_map{});
        expect(fatal(sink != nullptr)) << "the shared object's sink was not created";
        gr::MsgPortOut toSink;
        gr::MsgPortIn  fromSink;
        expect(fatal(toSink.connect(*sink->msgIn).has_value()));
        expect(fatal(sink->msgOut->connect(fromSink).has_value()));

        // msgOut takes the first replies, the sink keeps up to the bound, and it drops the rest
        const std::size_t bound     = sink->msgOut->bufferSize();
        const std::size_t nRequests = 2UZ * fromSink.buffer().streamBuffer.size() + 3UZ;
        const std::size_t nBefore   = gr::message::droppedMessageCount().load();
        for (std::size_t nSent = 0UZ; nSent < nRequests;) {
            const std::size_t nBatch = std::min(nRequests - nSent, toSink.streamWriter().available());
            expect(fatal(gt(nBatch, 0UZ))) << "the sink's msgIn stayed full";
            for (std::size_t i = 0UZ; i < nBatch; ++i) {
                gr::sendMessage<gr::message::Command::Get>(toSink, sink->uniqueName(), gr::block::property::kSetting, gr::property_map{});
            }
            nSent += nBatch;
            sink->processScheduledMessages();
        }
        const std::size_t nInMsgOut = fromSink.streamReader().available();
        const std::size_t nKept     = std::min(nRequests - nInMsgOut, bound);
        const std::size_t nDropped  = nRequests - nInMsgOut - nKept;
        std::println("replies of a sink from a shared object: {} requests, {} in msgOut, {} kept, {} dropped, {} counted by the program", nRequests, nInMsgOut, nKept, nDropped, gr::message::droppedMessageCount().load() - nBefore);
        expect(gt(nDropped, 0UZ)) << "the sink dropped no reply";
        expect(eq(gr::message::droppedMessageCount().load() - nBefore, nDropped)) << "the program did not count the drops of the shared object's sink";
    };
#endif
};

namespace qa_sched {

constexpr std::string_view kOwnRequestPoolName = "qa_own_request_cpu";

template<typename TScheduler>
struct WorkQuiescenceProbe : TScheduler {
    using TScheduler::TScheduler;

    [[nodiscard]] std::size_t nWorkQuiescenceRequests() { return gr::atomic_ref(this->_nWorkQuiescenceRequests).load_acquire(); }

    [[nodiscard]] bool workQuiescenceRequested() { return nWorkQuiescenceRequests() > 0UZ; }

    // a request for work quiescence is in force while a counted work() call is open
    [[nodiscard]] bool requestWaitsForWork() { return workQuiescenceRequested() && gr::atomic_ref(this->_nWorkersInWork).load_acquire() > 0UZ; }
};

// shared by a source, a stage and a sink. The stage sends a request to its own scheduler from inside its first
// processOne() call. With holdSource, the source first enters a work() call that it holds until the scheduler requests
// work quiescence, at most kStageWait. The source and the sink count the stop() and pause() hooks that ran while they
// were inside processOne() or processBulk().
struct OwnRequestProbe {
    std::function<bool()>              request;
    std::function<bool()>              workQuiescenceRequested;
    bool                               holdSource = false;
    std::atomic<bool>                  stageWaits{false};
    std::atomic<bool>                  sourceHeld{false};
    std::atomic<bool>                  requestAccepted{false};
    std::atomic<bool>                  requestReturned{false};
    std::array<std::atomic<bool>, 3UZ> inWork{};
    std::atomic<std::size_t>           nHooksDuringWork{0UZ};

    void hookRan(std::size_t index) {
        if (inWork[index].load(std::memory_order_acquire)) {
            nHooksDuringWork.fetch_add(1UZ, std::memory_order_relaxed);
        }
    }
};

// one sample per work() call
struct OwnRequestSource : gr::Block<OwnRequestSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OwnRequestSource, out);

    OwnRequestProbe* _probe = nullptr;

    void stop() { _probe->hookRan(0UZ); }
    void pause() { _probe->hookRan(0UZ); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        _probe->inWork[0UZ].store(true, std::memory_order_release);
        if (_probe->holdSource && _probe->stageWaits.load(std::memory_order_acquire) && !_probe->sourceHeld.exchange(true, std::memory_order_acq_rel)) {
            std::ignore = awaitCondition(_probe->workQuiescenceRequested, kStageWait);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        _probe->inWork[0UZ].store(false, std::memory_order_release);
        return gr::work::Status::OK;
    }
};

struct OwnRequestStage : gr::Block<OwnRequestStage> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OwnRequestStage, in, out);

    OwnRequestProbe* _probe = nullptr;

    [[nodiscard]] float processOne(float sample) {
        if (!_probe->stageWaits.exchange(true, std::memory_order_acq_rel)) {
            if (_probe->holdSource) {
                std::ignore = awaitCondition([this] { return _probe->sourceHeld.load(std::memory_order_acquire); }, kStageWait);
            }
            _probe->requestAccepted.store(_probe->request(), std::memory_order_release);
            _probe->requestReturned.store(true, std::memory_order_release);
        }
        return sample;
    }
};

struct OwnRequestSink : gr::Block<OwnRequestSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(OwnRequestSink, in);

    OwnRequestProbe* _probe = nullptr;

    void stop() { _probe->hookRan(2UZ); }
    void pause() { _probe->hookRan(2UZ); }

    void processOne(float) const {
        _probe->inWork[2UZ].store(true, std::memory_order_release);
        _probe->inWork[2UZ].store(false, std::memory_order_release);
    }
};

// runs source, stage and sink under TScheduler, and the stage sends `request` to the scheduler from inside its own
// processOne() call. Under multiThreaded the source holds a work() call of its own worker when the request arrives. A
// pause is followed by a stop from the test's thread. A scheduler whose request or run does not return is leaked with
// its probe. The case then fails instead of hanging.
template<typename TScheduler>
void expectRequestFromOwnWorkReturns(gr::lifecycle::State request) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;
    constexpr bool kWorkerPerBlock = TScheduler::executionPolicy() == gr::scheduler::ExecutionPolicy::multiThreaded;

    auto pool         = fixedPool(kOwnRequestPoolName, 4U);
    auto probe        = std::make_unique<OwnRequestProbe>();
    probe->holdSource = kWorkerPerBlock;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<OwnRequestSource>();
    auto&     stage  = flow.emplaceBlock<OwnRequestStage>();
    auto&     sink   = flow.emplaceBlock<OwnRequestSink>();
    source._probe    = probe.get();
    stage._probe     = probe.get();
    sink._probe      = probe.get();
    expect(flow.connect<"out", "in">(source, stage).has_value());
    expect(flow.connect<"out", "in">(stage, sink).has_value());

    auto scheduler = std::make_unique<WorkQuiescenceProbe<TScheduler>>(gr::property_map{{"poolName", std::string(kOwnRequestPoolName)}});
    expect(scheduler->exchange(std::move(flow)).has_value());
    WorkQuiescenceProbe<TScheduler>* raw = scheduler.get();
    probe->request                       = [raw, request] { return raw->changeStateTo(request).has_value(); };
    probe->workQuiescenceRequested       = [raw] { return raw->workQuiescenceRequested(); };

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    auto        result     = std::make_shared<std::expected<void, gr::Error>>();
    std::thread runner([raw, runnerDone, result] {
        *result = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    const bool returned = awaitCondition([&probe] { return probe->requestReturned.load(std::memory_order_acquire); });
    expect(returned) << "the request made inside a work() call waited for that call";
    if (returned && request == REQUESTED_PAUSE) {
        expect(raw->state() == PAUSED) << "the scheduler did not pause";
        expect(raw->changeStateTo(REQUESTED_STOP).has_value());
    }
    const bool ended = returned && awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        std::ignore = scheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
    expect(probe->requestAccepted.load(std::memory_order_acquire)) << "the scheduler refused the request";
    expect(result->has_value()) << "a stopped run must succeed";
    expect(raw->state() == STOPPED) << "the run must end STOPPED";
    expect(probe->sourceHeld.load(std::memory_order_acquire) == kWorkerPerBlock) << "the source made no work() call while the stage waited";
    expect(eq(probe->nHooksDuringWork.load(), 0UZ)) << "a hook of another block ran during its own work() call";
}

constexpr std::string_view kRequestPairPoolName = "qa_request_pair_cpu";

// shared by two sources on workers of their own. Each source makes its request to the scheduler from inside its first
// work() call. The second source enters that call and waits there until the scheduler reads secondAwaits. The first
// source makes its request once the second is inside its call. A source without a request returns from the call.
struct RequestPairProbe {
    std::array<std::function<bool()>, 2UZ> request;
    std::function<gr::lifecycle::State()>  schedulerState;
    gr::lifecycle::State                   secondAwaits = gr::lifecycle::State::REQUESTED_PAUSE;
    std::array<std::atomic<bool>, 2UZ>     inWork{};
    std::array<std::atomic<bool>, 2UZ>     requestAccepted{};
    std::array<std::atomic<bool>, 2UZ>     requestReturned{};
};

struct PairRequestSource : gr::Block<PairRequestSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(PairRequestSource, out);

    RequestPairProbe* _probe     = nullptr;
    std::size_t       _slot      = 0UZ;
    bool              _requested = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        if (!_requested && _probe->request[_slot]) {
            _requested = true;
            _probe->inWork[_slot].store(true, std::memory_order_release);
            const bool mayRequest = _slot == 0UZ ? awaitCondition([this] { return _probe->inWork[1UZ].load(std::memory_order_acquire); }) //
                                                 : awaitCondition([this] { return _probe->schedulerState() == _probe->secondAwaits; });
            if (mayRequest) {
                _probe->requestAccepted[_slot].store(_probe->request[_slot](), std::memory_order_release);
            }
            _probe->requestReturned[_slot].store(true, std::memory_order_release);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// runs two PairRequestSources, each feeding a CountingSink, under multiThreaded with a pool thread for each block. The
// first request comes from the first source's work() call, or with firstFromTestThread from a thread of the test once
// the second source is inside its call. The test's thread stops a run that the requests left active. A scheduler or a
// thread whose request or run does not return is leaked with the probe. The case then fails instead of hanging. A
// pause that a stop or a resume overtakes reports no error, and after a resume the scheduler and every block read
// RUNNING.
void expectRequestPairReturns(gr::lifecycle::State firstRequest, gr::lifecycle::State secondRequest, bool firstFromTestThread) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto      pool  = fixedPool(kRequestPairPoolName, 4U);
    auto      probe = std::make_unique<RequestPairProbe>();
    gr::Graph flow;
    for (std::size_t slot : {0UZ, 1UZ}) {
        auto& source  = flow.emplaceBlock<PairRequestSource>();
        auto& sink    = flow.emplaceBlock<CountingSink>();
        source._probe = probe.get();
        source._slot  = slot;
        expect(flow.connect<"out", "in">(source, sink).has_value());
    }

    auto scheduler     = std::make_unique<TestScheduler>(gr::property_map{{"poolName", std::string(kRequestPairPoolName)}});
    auto fromScheduler = std::make_unique<gr::MsgPortIn>();
    expect(scheduler->msgOut.connect(*fromScheduler).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    TestScheduler* raw    = scheduler.get();
    probe->schedulerState = [raw] { return raw->state(); };
    probe->request[1UZ]   = [raw, secondRequest] { return raw->changeStateTo(secondRequest).has_value(); };
    if (!firstFromTestThread) {
        probe->request[0UZ] = [raw, firstRequest] { return raw->changeStateTo(firstRequest).has_value(); };
    }

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    auto        result     = std::make_shared<std::expected<void, gr::Error>>();
    std::thread runner([raw, runnerDone, result] {
        *result = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });
    std::thread requester;
    if (firstFromTestThread) {
        RequestPairProbe* rawProbe = probe.get();
        requester                  = std::thread([raw, rawProbe, firstRequest] {
            if (awaitCondition([rawProbe] { return rawProbe->inWork[1UZ].load(std::memory_order_acquire); })) {
                rawProbe->requestAccepted[0UZ].store(raw->changeStateTo(firstRequest).has_value(), std::memory_order_release);
            }
            rawProbe->requestReturned[0UZ].store(true, std::memory_order_release);
        });
    }

    const bool returned = awaitCondition([&probe] { return probe->requestReturned[0UZ].load(std::memory_order_acquire) && probe->requestReturned[1UZ].load(std::memory_order_acquire); });
    expect(returned) << "a request made while the other waited did not return";
    if (returned && secondRequest == RUNNING) {
        expect(raw->state() == RUNNING) << "the resume must leave the scheduler RUNNING";
        expect(std::ranges::all_of(raw->blocks(), [](const auto& block) { return block->state() == RUNNING; })) << "the resume must leave every block RUNNING";
    }
    if (returned && !gr::lifecycle::isShuttingDown(raw->state())) {
        expect(raw->changeStateTo(REQUESTED_STOP).has_value());
    }
    const bool ended = returned && awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        if (requester.joinable()) {
            requester.detach();
        }
        std::ignore = scheduler.release();
        std::ignore = fromScheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
    if (requester.joinable()) {
        requester.join();
    }
    expect(probe->requestAccepted[0UZ].load(std::memory_order_acquire)) << "the scheduler refused the first request";
    expect(probe->requestAccepted[1UZ].load(std::memory_order_acquire)) << "the scheduler refused the second request";
    expect(result->has_value()) << "a stopped run must succeed";
    expect(raw->state() == STOPPED) << "the run must end STOPPED";
    ErrorReports reports;
    reports.take(*fromScheduler);
    expect(reports.errors.empty()) << std::format("the pause that the {} overtook reported its refused moves as errors", gr::meta::enumName(secondRequest).value_or(""));
}

constexpr std::string_view kHeldWorkPoolName = "qa_held_work_cpu";

// shared by a HeldWorkGate and the HeldWorkSource that feeds it. The source holds one work() call while mayHold()
// allows it, until endHold() reads true or the source's stop() hook runs, at most kStageWait. The gate comes first in the graph, and a sweep reaches it before the source. The gate's first stop() hook waits until the
// source holds its call or the scheduler requests work quiescence, at most kStageWait. A sweep that does not wait for
// the held call then runs the source's stop() hook during that call.
struct HeldWorkProbe {
    std::function<bool()>    workQuiescenceRequested;
    std::function<bool()>    endHold;
    std::function<bool()>    mayHold;
    std::atomic<bool>        held{false};
    std::atomic<bool>        inWork{false};
    std::atomic<bool>        gateWaited{false};
    std::atomic<std::size_t> nGateSamples{0UZ};
    std::atomic<std::size_t> nSourceHooks{0UZ};
    std::atomic<std::size_t> nHooksDuringWork{0UZ};
};

// one sample per work() call
struct HeldWorkSource : gr::Block<HeldWorkSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HeldWorkSource, out);

    HeldWorkProbe* _probe = nullptr;

    void stop() {
        if (_probe->inWork.load(std::memory_order_acquire)) {
            _probe->nHooksDuringWork.fetch_add(1UZ, std::memory_order_relaxed);
        }
        _probe->nSourceHooks.fetch_add(1UZ, std::memory_order_release);
    }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        if (_probe->mayHold() && !_probe->held.exchange(true, std::memory_order_acq_rel)) {
            _probe->inWork.store(true, std::memory_order_release);
            std::ignore = awaitCondition([this] { return _probe->endHold() || _probe->nSourceHooks.load(std::memory_order_acquire) > 0UZ; }, kStageWait);
            _probe->inWork.store(false, std::memory_order_release);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

struct HeldWorkGate : gr::Block<HeldWorkGate> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(HeldWorkGate, in);

    HeldWorkProbe* _probe = nullptr;

    void stop() {
        if (!_probe->gateWaited.exchange(true, std::memory_order_acq_rel)) {
            std::ignore = awaitCondition([this] { return _probe->inWork.load(std::memory_order_acquire) || _probe->workQuiescenceRequested(); }, kStageWait);
        }
    }

    void processOne(float) { _probe->nGateSamples.fetch_add(1UZ, std::memory_order_relaxed); }
};

// the sweep that stops the HeldWorkSource
enum class HeldWorkSweep {
    graphReplacement, // a graph replacement by message retires the running graph
    failedResume,     // a resume finds an edge that cannot connect and fails
    stop,             // a stop from the test's thread
};

// runs a HeldWorkGate, the HeldWorkSource that feeds it and an Int16Sink under TScheduler, and runs `sweep` while the
// source can hold a work() call. A failed resume follows a pause, during which the test records an edge from the
// source to the Int16Sink by port names. The source then holds only a call made during the resume. A scheduler whose
// run does not end is leaked with its probe. The case then fails instead of hanging.
template<typename TScheduler>
void expectSweepWaitsForHeldWork(HeldWorkSweep sweep) {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto      pool  = fixedPool(kHeldWorkPoolName, 4U);
    auto      probe = std::make_unique<HeldWorkProbe>();
    gr::Graph flow;
    auto&     gate      = flow.emplaceBlock<HeldWorkGate>();
    auto&     source    = flow.emplaceBlock<HeldWorkSource>();
    auto&     int16Sink = flow.emplaceBlock<Int16Sink>();
    gate._probe         = probe.get();
    source._probe       = probe.get();
    expect(flow.connect<"out", "in">(source, gate).has_value());

    auto scheduler   = std::make_unique<WorkQuiescenceProbe<TScheduler>>(gr::property_map{{"poolName", std::string(kHeldWorkPoolName)}});
    auto toScheduler = std::make_unique<gr::MsgPortOut>();
    expect(toScheduler->connect(scheduler->msgIn).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    WorkQuiescenceProbe<TScheduler>* raw   = scheduler.get();
    auto                             armed = std::make_shared<std::atomic<bool>>(sweep != HeldWorkSweep::failedResume);
    probe->workQuiescenceRequested         = [raw] { return raw->workQuiescenceRequested(); };
    probe->endHold                         = [raw] { return raw->requestWaitsForWork(); };
    probe->mayHold                         = [raw, armed] { return armed->load(std::memory_order_acquire) && raw->state() == RUNNING; };

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([raw, runnerDone] {
        std::ignore = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    if (sweep == HeldWorkSweep::failedResume) {
        expect(awaitCondition([&probe] { return probe->nGateSamples.load(std::memory_order_relaxed) > 0UZ; })) << "the run moved no samples";
        expect(raw->changeStateTo(REQUESTED_PAUSE).has_value());
        expect(awaitCondition([raw] { return raw->state() == PAUSED; })) << "the scheduler did not pause";
        expect(raw->graph().connect(source, gr::PortDefinition{"out"}, int16Sink, gr::PortDefinition{"in"}).has_value());
        armed->store(true, std::memory_order_release);
        std::ignore = raw->changeStateTo(RUNNING);
        expect(raw->state() == ERROR) << "a resume whose edge did not connect must end the scheduler in ERROR";
    } else {
        expect(awaitCondition([&probe] { return probe->inWork.load(std::memory_order_acquire); })) << "the source did not hold a work() call";
        if (sweep == HeldWorkSweep::graphReplacement) {
            gr::sendMessage<gr::message::Command::Set>(*toScheduler, raw->unique_name, gr::scheduler::property::kGraphGRC, gr::property_map{{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, "replace");
        } else {
            expect(raw->changeStateTo(REQUESTED_STOP).has_value());
        }
    }
    expect(awaitCondition([&probe] { return probe->nSourceHooks.load(std::memory_order_acquire) > 0UZ; })) << "the sweep did not reach the source";
    if (gr::lifecycle::isActive(raw->state())) {
        std::ignore = raw->changeStateTo(REQUESTED_STOP);
    }
    const bool ended = awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        std::ignore = scheduler.release();
        std::ignore = toScheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
    expect(eq(probe->nHooksDuringWork.load(), 0UZ)) << "the sweep ran the source's stop() hook during its held work() call";
}

constexpr std::string_view kSubResumePoolName      = "qa_sub_resume_cpu";
constexpr std::string_view kSubResumeInnerPoolName = "qa_sub_resume_inner_cpu";

// shared by a ParentResumeSource inside a sub-scheduler and the test. The source waits in its first work() call until
// the parent reads REQUESTED_PAUSE, at most kEventBound, and then resumes the parent from inside that call.
struct ParentResumeProbe {
    std::function<bool()> parentPausing;
    std::function<bool()> resumeParent;
    std::atomic<bool>     entered{false};
    std::atomic<bool>     resumeAccepted{false};
    std::atomic<bool>     resumeReturned{false};
};

// one sample per work() call
struct ParentResumeSource : gr::Block<ParentResumeSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ParentResumeSource, out);

    ParentResumeProbe* _probe = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        if (!_probe->entered.exchange(true, std::memory_order_acq_rel)) {
            if (awaitCondition(_probe->parentPausing)) {
                _probe->resumeAccepted.store(_probe->resumeParent(), std::memory_order_release);
            }
            _probe->resumeReturned.store(true, std::memory_order_release);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// runs a HeldWorkSource and a CountingSink beside a sub-scheduler, under TestScheduler. In the sub-scheduler a
// ParentResumeSource feeds a CountingSink. The test's thread pauses the parent while the HeldWorkSource holds a work()
// call, and that call holds until the parent's resume adds its request to the pause's. The pause waits for the call.
// The parent reads REQUESTED_PAUSE before the pause reaches the sub-scheduler, and the ParentResumeSource resumes the
// parent from inside the sub-scheduler's work() call. A scheduler or a thread whose request or run does not return is
// leaked with the probes. The case then fails instead of hanging.
void expectResumeFromSubSchedulerWorkReturns() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto pool        = fixedPool(kSubResumePoolName, 4U);
    auto innerPool   = fixedPool(kSubResumeInnerPoolName, 3U);
    auto heldProbe   = std::make_unique<HeldWorkProbe>();
    auto resumeProbe = std::make_unique<ParentResumeProbe>();

    gr::Graph innerFlow;
    auto&     innerSource = innerFlow.emplaceBlock<ParentResumeSource>();
    auto&     innerSink   = innerFlow.emplaceBlock<CountingSink>();
    innerSource._probe    = resumeProbe.get();
    expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
    auto inner = std::make_shared<gr::SchedulerWrapper<TestScheduler>>(gr::property_map{{"poolName", std::string(kSubResumeInnerPoolName)}});
    inner->setGraph(std::move(innerFlow));

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<HeldWorkSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    source._probe    = heldProbe.get();
    expect(flow.connect<"out", "in">(source, sink).has_value());
    std::ignore = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));

    auto scheduler     = std::make_unique<WorkQuiescenceProbe<TestScheduler>>(gr::property_map{{"poolName", std::string(kSubResumePoolName)}});
    auto fromScheduler = std::make_unique<gr::MsgPortIn>();
    expect(scheduler->msgOut.connect(*fromScheduler).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    WorkQuiescenceProbe<TestScheduler>* raw = scheduler.get();
    heldProbe->workQuiescenceRequested      = [raw] { return raw->workQuiescenceRequested(); };
    heldProbe->endHold                      = [raw] { return raw->nWorkQuiescenceRequests() >= 2UZ; };
    heldProbe->mayHold                      = [raw] { return raw->state() == RUNNING; };
    resumeProbe->parentPausing              = [raw] { return raw->state() == REQUESTED_PAUSE; };
    resumeProbe->resumeParent               = [raw] { return raw->changeStateTo(RUNNING).has_value(); };

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    auto        result     = std::make_shared<std::expected<void, gr::Error>>();
    std::thread runner([raw, runnerDone, result] {
        *result = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    expect(awaitCondition([&heldProbe, &resumeProbe] { return heldProbe->inWork.load(std::memory_order_acquire) && resumeProbe->entered.load(std::memory_order_acquire); })) << "the parent's source held no work() call, or the sub-scheduler's source made none";
    auto        pauseReturned = std::make_shared<std::atomic<bool>>(false);
    std::thread pauser([raw, pauseReturned] {
        std::ignore = raw->changeStateTo(REQUESTED_PAUSE);
        pauseReturned->store(true, std::memory_order_release);
    });

    const bool returned = awaitCondition([&resumeProbe, &pauseReturned] { return resumeProbe->resumeReturned.load(std::memory_order_acquire) && pauseReturned->load(std::memory_order_acquire); });
    expect(returned) << "the resume made inside the sub-scheduler's work() call, or the pause it overtook, did not return";
    if (returned) {
        expect(resumeProbe->resumeAccepted.load(std::memory_order_acquire)) << "the parent refused the resume";
        expect(raw->state() == RUNNING) << "the resume must leave the parent RUNNING";
        expect(inner->blockRef().state() == RUNNING) << "the sub-scheduler must stay RUNNING";
        expect(raw->changeStateTo(REQUESTED_STOP).has_value());
    }
    const bool ended = returned && awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        pauser.detach();
        std::ignore = scheduler.release();
        std::ignore = fromScheduler.release();
        std::ignore = heldProbe.release();
        std::ignore = resumeProbe.release();
        return;
    }
    runner.join();
    pauser.join();
    expect(result->has_value()) << "a stopped run must succeed";
    expect(raw->state() == STOPPED) << "the run must end STOPPED";
    ErrorReports reports;
    reports.take(*fromScheduler);
    expect(reports.errors.empty()) << "the pause that the resume overtook reported its refused moves as errors";
}

constexpr std::string_view kNestedBlockingPoolName      = "qa_nested_blocking_cpu";
constexpr std::string_view kNestedBlockingInnerPoolName = "qa_nested_blocking_inner_cpu";

// shared by two StopAwaitingSources, one in each slot
struct StopAwaitProbe {
    std::array<std::atomic<bool>, 2UZ> waiting{};
    std::array<std::atomic<bool>, 2UZ> stopped{};
    std::array<std::atomic<bool>, 2UZ> endedByHook{};
    std::array<std::atomic<bool>, 2UZ> waitEnded{};
};

// a device that blocks in work() until its stop() hook runs. The first work() call waits for the hook, at most
// kStageWait, and records whether the hook ended the wait. One sample per work() call
struct StopAwaitingSource : gr::Block<StopAwaitingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(StopAwaitingSource, out);

    StopAwaitProbe* _probe = nullptr;
    std::size_t     _slot  = 0UZ;

    [[nodiscard]] constexpr bool isBlocking() const noexcept { return true; }

    void stop() { _probe->stopped[_slot].store(true, std::memory_order_release); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        if (!_probe->waiting[_slot].exchange(true, std::memory_order_acq_rel)) {
            const bool byHook = awaitCondition([this] { return _probe->stopped[_slot].load(std::memory_order_acquire); }, kStageWait);
            _probe->endedByHook[_slot].store(byHook, std::memory_order_release);
            _probe->waitEnded[_slot].store(true, std::memory_order_release);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// runs a StopAwaitingSource inside a transparent subgraph and one inside a sub-scheduler, each feeding a CountingSink,
// under TestScheduler. Once both wait in work(), a message replaces the graph. A scheduler whose run does not end is
// leaked with its probe. The case then fails instead of hanging.
void expectGraphReplacementStopsNestedBlockingBlocks() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto pool      = fixedPool(kNestedBlockingPoolName, 4U);
    auto innerPool = fixedPool(kNestedBlockingInnerPoolName, 3U);
    auto probe     = std::make_unique<StopAwaitProbe>();

    auto  subgraph        = std::make_shared<gr::GraphWrapper<gr::Graph>>();
    auto& subgraphSource  = subgraph->graph()->emplaceBlock<StopAwaitingSource>();
    auto& subgraphSink    = subgraph->graph()->emplaceBlock<CountingSink>();
    subgraphSource._probe = probe.get();
    subgraphSource._slot  = 0UZ;
    expect(subgraph->graph()->connect<"out", "in">(subgraphSource, subgraphSink).has_value());

    gr::Graph innerFlow;
    auto&     innerSource = innerFlow.emplaceBlock<StopAwaitingSource>();
    auto&     innerSink   = innerFlow.emplaceBlock<CountingSink>();
    innerSource._probe    = probe.get();
    innerSource._slot     = 1UZ;
    expect(innerFlow.connect<"out", "in">(innerSource, innerSink).has_value());
    auto inner = std::make_shared<gr::SchedulerWrapper<TestScheduler>>(gr::property_map{{"poolName", std::string(kNestedBlockingInnerPoolName)}});
    inner->setGraph(std::move(innerFlow));

    gr::Graph flow;
    std::ignore = flow.addBlock(subgraph);
    std::ignore = flow.addBlock(gr::SchedulerModel::asBlockModelPtr(inner));

    auto scheduler   = std::make_unique<TestScheduler>(gr::property_map{{"poolName", std::string(kNestedBlockingPoolName)}});
    auto toScheduler = std::make_unique<gr::MsgPortOut>();
    expect(toScheduler->connect(scheduler->msgIn).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    TestScheduler* raw = scheduler.get();

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([raw, runnerDone] {
        std::ignore = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    expect(awaitCondition([&probe] { return probe->waiting[0UZ].load(std::memory_order_acquire) && probe->waiting[1UZ].load(std::memory_order_acquire); })) << "a blocking source made no work() call";
    gr::sendMessage<gr::message::Command::Set>(*toScheduler, raw->unique_name, gr::scheduler::property::kGraphGRC, gr::property_map{{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, "replace");
    expect(awaitCondition([&probe] { return probe->waitEnded[0UZ].load(std::memory_order_acquire) && probe->waitEnded[1UZ].load(std::memory_order_acquire); })) << "a blocking source did not end its wait in work()";
    expect(probe->endedByHook[0UZ].load(std::memory_order_acquire)) << "the graph replacement did not stop the blocking source inside the transparent subgraph during its wait in work()";
    expect(probe->endedByHook[1UZ].load(std::memory_order_acquire)) << "the graph replacement did not stop the blocking source inside the sub-scheduler during its wait in work()";
    if (gr::lifecycle::isActive(raw->state())) {
        std::ignore = raw->changeStateTo(REQUESTED_STOP);
    }
    const bool ended = awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        std::ignore = scheduler.release();
        std::ignore = toScheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
}

constexpr std::string_view kRetiredSubgraphPoolName = "qa_retired_subgraph_cpu";

// records the first run of a StopRecordingSource's stop() hook and whether the subgraph that holds the source was
// still alive then
struct RetiredSubgraphProbe {
    std::weak_ptr<gr::BlockModel> subgraph;
    std::atomic<bool>             worked{false};
    std::atomic<bool>             hookEntered{false};
    std::atomic<bool>             subgraphAlive{false};
    std::atomic<bool>             hookRan{false};
};

// a source that does not block in work(). One sample per processOne() call
struct StopRecordingSource : gr::Block<StopRecordingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(StopRecordingSource, out);

    RetiredSubgraphProbe* _probe = nullptr;

    void stop() {
        if (!_probe->hookEntered.exchange(true, std::memory_order_acq_rel)) {
            _probe->subgraphAlive.store(!_probe->subgraph.expired(), std::memory_order_release);
            _probe->hookRan.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] float processOne() noexcept {
        _probe->worked.store(true, std::memory_order_release);
        return 1.0f;
    }
};

// runs a StopRecordingSource and a CountingSink inside a transparent subgraph under TestScheduler and replaces the
// graph by message once the source has run. The scheduler holds the only references to the subgraph. A scheduler whose
// run does not end is leaked with its probe. The case then fails instead of hanging.
void expectGraphReplacementStopsSubgraphBlockBeforeDestruction() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto pool  = fixedPool(kRetiredSubgraphPoolName, 2U);
    auto probe = std::make_unique<RetiredSubgraphProbe>();

    auto  subgraph        = std::make_shared<gr::GraphWrapper<gr::Graph>>();
    auto& subgraphSource  = subgraph->graph()->emplaceBlock<StopRecordingSource>();
    auto& subgraphSink    = subgraph->graph()->emplaceBlock<CountingSink>();
    subgraphSource._probe = probe.get();
    expect(subgraph->graph()->connect<"out", "in">(subgraphSource, subgraphSink).has_value());
    probe->subgraph = subgraph;

    gr::Graph flow;
    std::ignore = flow.addBlock(subgraph);
    subgraph.reset();

    auto scheduler   = std::make_unique<TestScheduler>(gr::property_map{{"poolName", std::string(kRetiredSubgraphPoolName)}});
    auto toScheduler = std::make_unique<gr::MsgPortOut>();
    expect(toScheduler->connect(scheduler->msgIn).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    TestScheduler* raw = scheduler.get();

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([raw, runnerDone] {
        std::ignore = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    expect(awaitCondition([&probe] { return probe->worked.load(std::memory_order_acquire); })) << "the source inside the transparent subgraph made no work() call";
    expect(!probe->subgraph.expired()) << "the running graph does not hold the subgraph";
    expect(!probe->hookRan.load(std::memory_order_acquire)) << "the source's stop() hook ran before the graph replacement";
    gr::sendMessage<gr::message::Command::Set>(*toScheduler, raw->unique_name, gr::scheduler::property::kGraphGRC, gr::property_map{{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, "replace");
    expect(awaitCondition([&probe] { return probe->hookRan.load(std::memory_order_acquire); })) << "the graph replacement did not run the stop() hook of the source inside the transparent subgraph";
    expect(probe->subgraphAlive.load(std::memory_order_acquire)) << "the source's stop() hook ran only once the retired subgraph was destroyed";
    if (gr::lifecycle::isActive(raw->state())) {
        std::ignore = raw->changeStateTo(REQUESTED_STOP);
    }
    const bool ended = awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        std::ignore = scheduler.release();
        std::ignore = toScheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
}

constexpr std::string_view kInWorkStopPoolName = "qa_in_work_stop_cpu";

// shared by an InWorkStopSource and the case that runs it
struct InWorkStopProbe {
    std::function<bool()> mayStop;
    std::function<void()> stop;
    std::atomic<bool>     inWork{false};
    std::atomic<bool>     stopMade{false};
    std::atomic<bool>     stopReturned{false};
};

// one sample per work() call. The first call waits until mayStop() holds, at most kStageWait, and then makes the stop
struct InWorkStopSource : gr::Block<InWorkStopSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(InWorkStopSource, out);

    InWorkStopProbe* _probe = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (outSpan.size() == 0UZ) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        if (!_probe->inWork.exchange(true, std::memory_order_acq_rel)) {
            const bool mayStop = awaitCondition(_probe->mayStop, kStageWait);
            _probe->stopMade.store(mayStop, std::memory_order_release);
            if (mayStop) {
                _probe->stop();
            }
            _probe->stopReturned.store(true, std::memory_order_release);
        }
        outSpan[0UZ] = 1.0f;
        outSpan.publish(1UZ);
        return gr::work::Status::OK;
    }
};

// runs an InWorkStopSource and a CountingSink under TestScheduler. A message replaces the graph once the source is
// inside its call. The source requests a stop once the replacement has requested work quiescence. A scheduler or a
// thread whose request or run does not return is leaked with the probe. The case then fails instead of hanging.
void expectInWorkStopDuringGraphReplacementReturns() {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    auto      pool  = fixedPool(kInWorkStopPoolName, 4U);
    auto      probe = std::make_unique<InWorkStopProbe>();
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<InWorkStopSource>();
    auto&     sink   = flow.emplaceBlock<CountingSink>();
    source._probe    = probe.get();
    expect(flow.connect<"out", "in">(source, sink).has_value());

    auto scheduler   = std::make_unique<WorkQuiescenceProbe<TestScheduler>>(gr::property_map{{"poolName", std::string(kInWorkStopPoolName)}});
    auto toScheduler = std::make_unique<gr::MsgPortOut>();
    expect(toScheduler->connect(scheduler->msgIn).has_value());
    expect(scheduler->exchange(std::move(flow)).has_value());
    WorkQuiescenceProbe<TestScheduler>* raw = scheduler.get();
    probe->mayStop                          = [raw] { return raw->workQuiescenceRequested(); };
    probe->stop                             = [raw] { std::ignore = raw->changeStateTo(REQUESTED_STOP); };

    auto        runnerDone = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([raw, runnerDone] {
        std::ignore = raw->runAndWait();
        runnerDone->store(true, std::memory_order_release);
    });

    expect(awaitCondition([&probe] { return probe->inWork.load(std::memory_order_acquire); })) << "the source made no work() call";
    gr::sendMessage<gr::message::Command::Set>(*toScheduler, raw->unique_name, gr::scheduler::property::kGraphGRC, gr::property_map{{"value", gr::saveGrc(gr::globalPluginLoader(), gr::Graph{})}}, "replace");

    const bool returned = awaitCondition([&probe] { return probe->stopReturned.load(std::memory_order_acquire); });
    expect(returned) << "the stop made inside the work() call did not return";
    expect(probe->stopMade.load(std::memory_order_acquire)) << "the source made no stop: no request held the scheduler during its work() call";
    if (returned && gr::lifecycle::isActive(raw->state())) {
        std::ignore = raw->changeStateTo(REQUESTED_STOP);
    }
    const bool ended = returned && awaitCondition([&runnerDone] { return runnerDone->load(std::memory_order_acquire); });
    expect(ended) << "the run did not end";
    if (!ended) {
        runner.detach();
        std::ignore = scheduler.release();
        std::ignore = toScheduler.release();
        std::ignore = probe.release();
        return;
    }
    runner.join();
    expect(raw->state() == STOPPED) << "the run must end STOPPED";
}

} // namespace qa_sched

const boost::ut::suite<"a stop or a pause from inside work()"> ownRequestTests = [] {
    using namespace boost::ut;
    using enum gr::lifecycle::State;

    "a stop made inside a work() call waits for the other workers' calls and not for its own"_test = [] {
        qa_sched::expectRequestFromOwnWorkReturns<qa_sched::TestScheduler>(REQUESTED_STOP);
        qa_sched::expectRequestFromOwnWorkReturns<qa_sched::SerialScheduler>(REQUESTED_STOP);
    };

    "a pause made inside a work() call waits for the other workers' calls and not for its own"_test = [] {
        qa_sched::expectRequestFromOwnWorkReturns<qa_sched::TestScheduler>(REQUESTED_PAUSE);
        qa_sched::expectRequestFromOwnWorkReturns<qa_sched::SerialScheduler>(REQUESTED_PAUSE);
    };

    "a pause and a stop made inside the work() calls of two workers both return, and the pause reports no error"_test = [] { //
        qa_sched::expectRequestPairReturns(REQUESTED_PAUSE, REQUESTED_STOP, false);
    };

    "a resume made inside a work() call during a pause from another thread returns, and the pause returns after it"_test = [] { //
        qa_sched::expectRequestPairReturns(REQUESTED_PAUSE, RUNNING, true);
    };

    "a resume that overtakes a pause made inside another worker's work() call leaves the run RUNNING, and the pause reports no error"_test = [] { //
        qa_sched::expectRequestPairReturns(REQUESTED_PAUSE, RUNNING, false);
    };

    "a resume made inside a sub-scheduler's work() call during the parent's pause returns, and the pause reports no error"_test = [] { //
        qa_sched::expectResumeFromSubSchedulerWorkReturns();
    };
};

const boost::ut::suite<"a block whose work() call holds while a sweep stops it"> heldWorkTests = [] {
    using namespace boost::ut;

    "a graph replacement by message stops the running graph's blocks between their work() calls"_test = [] { //
        qa_sched::expectSweepWaitsForHeldWork<qa_sched::TestScheduler>(qa_sched::HeldWorkSweep::graphReplacement);
    };

    "a resume whose edge cannot connect stops the blocks between their work() calls"_test = [] { //
        qa_sched::expectSweepWaitsForHeldWork<qa_sched::TestScheduler>(qa_sched::HeldWorkSweep::failedResume);
    };

    "a stop reaches a block between the work() calls of a scheduler's own worker"_test = [] { //
        qa_sched::expectSweepWaitsForHeldWork<qa_sched::OwnWorkerScheduler>(qa_sched::HeldWorkSweep::stop);
    };

    "a graph replacement by message stops a blocking block inside a transparent subgraph and one inside a sub-scheduler during their waits in work()"_test = [] { //
        qa_sched::expectGraphReplacementStopsNestedBlockingBlocks();
    };

    "a graph replacement by message stops a non-blocking block inside a transparent subgraph before the retired graph is destroyed"_test = [] { //
        qa_sched::expectGraphReplacementStopsSubgraphBlockBeforeDestruction();
    };

    "a stop made inside a work() call while a graph replacement by message waits for that call returns, and the run ends"_test = [] { //
        qa_sched::expectInWorkStopDuringGraphReplacementReturns();
    };
};

int main() { /* tests are statically registered */ }
