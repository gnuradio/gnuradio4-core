#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

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

using TestScheduler     = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;
using SerialScheduler   = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;
using BlockingScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreadedBlocking>;

[[nodiscard]] gr::Graph makeGraph() {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<BlockingSource>();
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

constexpr auto kRunBound = std::chrono::milliseconds(500);

// runAndWait() on its own thread with a deadline, so a stop that fails to take fails the assertion
// instead of hanging ctest; `duringStartup` runs on the caller's thread the moment the runner exists
template<typename TDuringStartup>
[[nodiscard]] bool runAndWaitWithin(BlockingScheduler& scheduler, std::chrono::milliseconds bound, TDuringStartup duringStartup) {
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

[[nodiscard]] bool runAndWaitWithin(BlockingScheduler& scheduler, std::chrono::milliseconds bound) {
    return runAndWaitWithin(scheduler, bound, [] {});
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

int main() { /* tests are statically registered */ }
