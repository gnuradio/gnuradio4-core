#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <thread>
#include <tuple>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/LifeCycle.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_start_stop {

// the block hooks the scheduler's child sweeps ran, counted per cycle
inline std::atomic<int> gStartHooks{0};
inline std::atomic<int> gStopHooks{0};

// hold start()'s child sweep open so that a stop can be placed inside it
inline std::atomic<bool> gSweepEntered{false};
inline std::atomic<bool> gSweepRelease{false};

struct ParkedStartSource : gr::Block<ParkedStartSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ParkedStartSource, out);

    void start() {
        gStartHooks.fetch_add(1, std::memory_order_relaxed);
        gSweepEntered.store(true, std::memory_order_release);
        gSweepEntered.notify_all();
        gSweepRelease.wait(false, std::memory_order_acquire);
    }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    void start() { gStartHooks.fetch_add(1, std::memory_order_relaxed); }

    void stop() { gStopHooks.fetch_add(1, std::memory_order_relaxed); }

    void processOne(float) {}
};

using TestScheduler = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>;

// the stop claims the scheduler's transition before its own child sweep runs, so this returns inside
// the window under test
[[nodiscard]] bool awaitShutdownClaim(const TestScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gr::lifecycle::isShuttingDown(scheduler.state())) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace qa_start_stop

const boost::ut::suite<"stop requested during the start transient"> startStopRaceTests = [] {
    using namespace boost::ut;

    // the scheduler claims RUNNING before start() has moved a single child, so a caller that waits for
    // RUNNING and then stops is inside start()'s sweep by construction
    "a stop claimed inside start()'s child sweep starts every child or none"_test = [] {
        constexpr int nCycles = 64;

        std::size_t nCyclesPartlyStarted = 0UZ;
        std::size_t nCyclesLeftActive    = 0UZ;
        std::size_t nCyclesUnclaimed     = 0UZ;

        for (int cycle = 0; cycle < nCycles; ++cycle) {
            gr::Graph flow;
            auto&     source = flow.emplaceBlock<qa_start_stop::ParkedStartSource>();
            auto&     sink   = flow.emplaceBlock<qa_start_stop::CountingSink>();
            expect(flow.connect<"out", "in">(source, sink).has_value());

            qa_start_stop::gStartHooks.store(0, std::memory_order_release);
            qa_start_stop::gStopHooks.store(0, std::memory_order_release);
            qa_start_stop::gSweepEntered.store(false, std::memory_order_release);
            qa_start_stop::gSweepRelease.store(false, std::memory_order_release);

            qa_start_stop::TestScheduler scheduler({{"timeout_ms", gr::Size_t(5)}});
            expect(scheduler.exchange(std::move(flow)).has_value());

            std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });
            qa_start_stop::gSweepEntered.wait(false, std::memory_order_acquire);

            std::thread stopper([&scheduler] { scheduler.requestStop(); });
            if (!qa_start_stop::awaitShutdownClaim(scheduler)) {
                nCyclesUnclaimed++;
            }

            qa_start_stop::gSweepRelease.store(true, std::memory_order_release);
            qa_start_stop::gSweepRelease.notify_all();

            stopper.join();
            runner.join();

            if (qa_start_stop::gStartHooks.load(std::memory_order_acquire) != 2) {
                nCyclesPartlyStarted++;
            }
            if (gr::lifecycle::isActive(scheduler.state())) {
                nCyclesLeftActive++;
            }
        }

        expect(eq(nCyclesUnclaimed, 0UZ)) << "cycles in which the stop never claimed the scheduler's transition";
        expect(eq(nCyclesPartlyStarted, 0UZ)) << "cycles in which the stop left a child that start() could then not start";
        expect(eq(nCyclesLeftActive, 0UZ)) << "cycles that left the scheduler in an active state";
    };
};

int main() { /* tests are statically registered */ }
