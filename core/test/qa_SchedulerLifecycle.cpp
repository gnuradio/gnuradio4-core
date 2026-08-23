#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

struct EndlessSource : gr::Block<EndlessSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(EndlessSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
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

// adoptBlock is the scheduler's entry point for a block added to an already running graph
struct AdoptingScheduler : TestScheduler {
    using TestScheduler::adoptBlock;
    using TestScheduler::TestScheduler;
};

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

[[nodiscard]] std::shared_ptr<gr::thread_pool::TaskExecutor> twoThreadPool() {
    auto pool = std::make_shared<gr::thread_pool::ThreadPoolWrapper>(std::make_unique<gr::thread_pool::BasicThreadPool>(kOccupiedPoolName, gr::thread_pool::TaskType::CPU_BOUND, 2U, 2U), "CPU");
    gr::thread_pool::Manager::instance().replacePool(std::string(kOccupiedPoolName), pool);
    return pool;
}

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
        expect(scheduler.runAndWait().has_value());

        expect(scheduler.state() == ERROR) << "a failing block drives the scheduler to ERROR";
        expect(eq(source._nStopCalls, 1)) << "stop() must run for the block that failed";
        expect(eq(sink._nStopCalls, 1)) << "stop() must run for the blocks torn down alongside it";
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

int main() { /* tests are statically registered */ }
