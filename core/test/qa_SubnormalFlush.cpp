#include <boost/ut.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/thread/SubnormalFlush.hpp>
#include <gnuradio-4.0/thread/thread_pool.hpp>

namespace qa_subnormal_flush {

using gr::thread_pool::thread::detail::kCanFlushSubnormals;
using gr::thread_pool::thread::detail::setSubnormalFlush;
using gr::thread_pool::thread::detail::subnormalsFlushed;

// a quarter of the smallest normal float: exact, and its double is still below the normal range
inline constexpr float kSubnormal        = std::numeric_limits<float>::min() / 4.0f;
inline constexpr float kDoubledSubnormal = kSubnormal + kSubnormal;
static_assert(kSubnormal > 0.0f && kDoubledSubnormal < std::numeric_limits<float>::min());

// what a thread computes for kSubnormal + kSubnormal: zero under the flush, kDoubledSubnormal without it
[[nodiscard]] inline float doubledOnThisThread() {
    volatile float subnormal = kSubnormal; // keeps the addition out of constant folding
    const float    operand   = subnormal;
    volatile float sum       = operand + operand; // the store fixes the result before the caller changes the flush
    return sum;
}

// compares the bits: a comparison on a thread with the flush on reads a subnormal operand as zero
[[nodiscard]] constexpr bool isExpectedDoubled(float doubled, bool flush) { return std::bit_cast<std::uint32_t>(doubled) == std::bit_cast<std::uint32_t>(kCanFlushSubnormals && flush ? 0.0f : kDoubledSubnormal); }

struct SubnormalSource : gr::Block<SubnormalSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(SubnormalSource, out);

    bool _emitted = false;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        outSpan[0] = kSubnormal;
        outSpan.publish(1UZ);
        _emitted = true;
        return gr::work::Status::OK;
    }
};

// adds each input to itself on the thread that runs the block, and records that thread and its flush
struct DoublingSink : gr::Block<DoublingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(DoublingSink, in);

    std::size_t     _nReceived     = 0UZ;
    float           _doubled       = -1.0f;
    bool            _threadFlushes = false;
    std::thread::id _thread{};

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        for (const float value : inSpan) {
            volatile float sum = value + value; // the store fixes the result before the flush changes
            _doubled           = sum;
            ++_nReceived;
        }
        if (!inSpan.empty()) {
            _threadFlushes = subnormalsFlushed();
            _thread        = std::this_thread::get_id();
        }
        std::ignore = inSpan.consume(inSpan.size());
        return gr::work::Status::OK;
    }
};

struct RunResult {
    std::size_t nReceived     = 0UZ;
    float       doubled       = -1.0f;
    bool        threadFlushes = false;
    bool        ranOnCaller   = false;
};

template<gr::scheduler::ExecutionPolicy policy>
[[nodiscard]] RunResult runWithFlush(bool flush) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<SubnormalSource>();
    auto&     sink   = flow.emplaceBlock<DoublingSink>();
    expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));

    gr::scheduler::Simple<policy> scheduler{gr::property_map{{"flush_subnormals", flush}}};
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    expect(fatal(scheduler.runAndWait().has_value()));
    return {sink._nReceived, sink._doubled, sink._threadFlushes, sink._thread == std::this_thread::get_id()};
}

[[nodiscard]] std::string describe(const RunResult& result) { return std::format("received {}, doubled {:a}, thread flushes {}, ran on the caller {}", result.nReceived, result.doubled, result.threadFlushes, result.ranOnCaller); }

} // namespace qa_subnormal_flush

const boost::ut::suite<"the subnormal flush of a pool worker"> poolFlushTests = [] {
    using namespace boost::ut;
    using namespace qa_subnormal_flush;

    "a pool worker flushes subnormals by default, and keeps them once the pool's flush is off"_test = [] {
        const bool callerFlushes = subnormalsFlushed();
        const auto onWorker      = [] { return std::pair{doubledOnThisThread(), subnormalsFlushed()}; };

        gr::thread_pool::BasicThreadPool pool("qa_subnormal", gr::thread_pool::CPU_BOUND, 1U, 1U);
        expect(pool.flushesSubnormals());
        const auto [doubledFlushOn, flushesOn] = pool.execute(onWorker).get();
        pool.setFlushSubnormals(false);
        const auto [doubledFlushOff, flushesOff] = pool.execute(onWorker).get();

        expect(isExpectedDoubled(doubledFlushOn, true) && flushesOn == kCanFlushSubnormals) << std::format("flush on: doubled {:a}, flushes {}", doubledFlushOn, flushesOn);
        expect(isExpectedDoubled(doubledFlushOff, false) && !flushesOff) << std::format("flush off: doubled {:a}, flushes {}", doubledFlushOff, flushesOff);
        expect(eq(subnormalsFlushed(), callerFlushes)) << "the pool must leave the constructing thread's flush alone";
    };
};

const boost::ut::suite<"the subnormal flush of a scheduler"> schedulerFlushTests = [] {
    using namespace boost::ut;
    using namespace qa_subnormal_flush;
    using enum gr::scheduler::ExecutionPolicy;

    "a block on a pool worker sees a subnormal input flushed to zero with the flush on and kept with it off"_test = [] {
        const bool  callerFlushes = subnormalsFlushed();
        std::string failures;
        for (const bool flush : {true, false}) {
            const RunResult result = runWithFlush<multiThreaded>(flush);
            if (result.nReceived != 1UZ || !isExpectedDoubled(result.doubled, flush) || result.threadFlushes != (kCanFlushSubnormals && flush) || result.ranOnCaller) {
                failures += std::format("[flush_subnormals {}: {}] ", flush, describe(result));
            }
        }
        expect(failures.empty()) << failures;
        expect(eq(subnormalsFlushed(), callerFlushes)) << "a multi-threaded run must leave the caller's flush alone";
    };

    "a single-threaded run flushes on the caller's thread and leaves the caller's flush as it found it"_test = [] {
        std::string failures;
        for (const bool callerFlush : {false, true}) {
            for (const bool flush : {true, false}) {
                setSubnormalFlush(callerFlush);
                const RunResult result       = runWithFlush<singleThreaded>(flush);
                const bool      flushesAfter = subnormalsFlushed();
                const float     doubledAfter = doubledOnThisThread();
                setSubnormalFlush(false);
                const bool blockAsExpected = result.nReceived == 1UZ && isExpectedDoubled(result.doubled, flush) && result.threadFlushes == (kCanFlushSubnormals && flush) && result.ranOnCaller;
                const bool callerRestored  = flushesAfter == (kCanFlushSubnormals && callerFlush) && isExpectedDoubled(doubledAfter, callerFlush);
                if (!blockAsExpected || !callerRestored) {
                    failures += std::format("[caller flush {}, flush_subnormals {}: {}; the caller afterwards flushes {}, doubles to {:a}] ", callerFlush, flush, describe(result), flushesAfter, doubledAfter);
                }
            }
        }
        expect(failures.empty()) << failures;
    };
};

int main() { /* tests are statically registered */ }
