#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace qa_progress_wake {

using Clock = std::chrono::steady_clock;
using Run   = gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>;

// A wake that a waiter misses shows as a wait of its whole deadline, so the timed waiter's deadline and the
// watchdog's period lie beyond the bound on a wake, which a loaded runner still meets.
// The worker spins, so the only wake under test is the one a work call sends.
constexpr auto       kTimedWait   = std::chrono::seconds(30);
constexpr auto       kUntimedWait = std::chrono::hours(1);
constexpr auto       kWakeBound   = std::chrono::seconds(5);
constexpr gr::Size_t kWatchdogMs  = 20'000U; // the watchdog's own wake lies beyond the bound

struct Gate {
    std::atomic<bool> open{false};
    std::atomic<bool> published{false};
};

// publishes one burst once the gate is open, and nothing before
struct GatedSource : gr::Block<GatedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GatedSource, out);

    Gate* _gate = nullptr;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!_gate->open.load(std::memory_order_acquire) || _gate->published.load(std::memory_order_acquire)) {
            outSpan.publish(0UZ);
            return gr::work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        const std::size_t nPublish = std::min(outSpan.size(), std::size_t{64});
        outSpan.publish(nPublish);
        _gate->published.store(nPublish > 0UZ, std::memory_order_release);
        return nPublish == 0UZ ? gr::work::Status::INSUFFICIENT_OUTPUT_ITEMS : gr::work::Status::OK;
    }
};

struct DrainSink : gr::Block<DrainSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(DrainSink, in);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

} // namespace qa_progress_wake

const boost::ut::suite<"progress wake"> progressWake = [] {
    using namespace boost::ut;
    using namespace qa_progress_wake;

    // Three threads wait on the graph's progress sequence while the blocks run and move nothing: one in waitUntil()
    // with a deadline of seconds, one in waitUntil() with a deadline an hour away, one in wait() with no deadline.
    // The first work call that moves samples advances the sequence and wakes all three, long before the timed deadline.
    "a work call wakes the timed, the untimed and the plain waiter of the progress sequence"_test = [] {
        Gate      gate;
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<GatedSource>();
        auto&     sink   = flow.emplaceBlock<DrainSink>();
        source._gate     = std::addressof(gate);
        expect(flow.connect<"out", "in">(source, sink).has_value());

        Run scheduler({{"watchdog_timeout", kWatchdogMs}});
        expect(scheduler.exchange(std::move(flow)).has_value());
        const gr::Sequence& progress = scheduler.graph().progress();

        const std::size_t before = progress.value();

        std::thread runner([&scheduler] { std::ignore = scheduler.runAndWait(); });

        std::atomic<Clock::rep> timedWokeAt{0};
        std::atomic<Clock::rep> untimedWokeAt{0};
        const auto              waiter = [&progress, before](Clock::duration length, std::atomic<Clock::rep>& wokeAt) {
            progress.waitUntil(before, Clock::now() + length);
            wokeAt.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
        };
        std::atomic<Clock::rep> plainWokeAt{0};
        std::thread             plain([&progress, before, &plainWokeAt] {
            progress.wait(before);
            plainWokeAt.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
        });
        std::thread             timed(waiter, kTimedWait, std::ref(timedWokeAt));
        std::thread             untimed(waiter, kUntimedWait, std::ref(untimedWokeAt));
        const bool              counted = [&progress] {
            const auto limit = Clock::now() + std::chrono::seconds(5);
            while ((progress.nTimedWaiters() < 2U || progress.nPlainWaiters() < 1U) && Clock::now() < limit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return progress.nTimedWaiters() >= 2U && progress.nPlainWaiters() >= 1U;
        }();
        expect(counted) << "all three waiters must be counted before the blocks move samples";

        const auto openedAt = Clock::now();
        gate.open.store(true, std::memory_order_release);

        const auto limit = openedAt + kWakeBound;
        while ((timedWokeAt.load(std::memory_order_acquire) == 0 || untimedWokeAt.load(std::memory_order_acquire) == 0 || plainWokeAt.load(std::memory_order_acquire) == 0) && Clock::now() < limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool timedWoke   = timedWokeAt.load(std::memory_order_acquire) != 0;
        const bool untimedWoke = untimedWokeAt.load(std::memory_order_acquire) != 0;
        expect(timedWoke) << "the timed waiter was not woken within the bound after a work call moved samples";
        expect(untimedWoke) << "the untimed waiter was not woken within the bound after a work call moved samples";
        const bool plainWoke = plainWokeAt.load(std::memory_order_acquire) != 0;
        expect(plainWoke) << "the waiter in wait() was not woken within the bound after a work call moved samples";
        expect(gt(progress.value(), before)) << "the work call advanced the sequence";

        if (!timedWoke || !untimedWoke || !plainWoke) { // a waiter that missed its wake would not end the case
            const_cast<gr::Sequence&>(progress).incrementAndGet();
            const_cast<gr::Sequence&>(progress).notify_all();
        }
        scheduler.requestStop();
        runner.join();
        timed.join();
        plain.join();
        untimed.join();
    };
};

int main() { /* tests are statically registered */ }
