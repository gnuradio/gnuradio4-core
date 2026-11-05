#include <benchmark.hpp>
#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/thread/SubnormalFlush.hpp>

/**
 * @brief What subnormal floats cost a block, with the subnormal flush on and off.
 *
 * A one-pole recursion fed a single impulse decays toward zero. Below the normal range, round to nearest holds it at a
 * small multiple of the smallest subnormal from then on, and a power sum of its output squares those subnormals. Without
 * the flush each multiply and add then works on a subnormal operand or result. With the flush the recursion reaches
 * zero at the bottom of the normal range, and no operation sees a subnormal after that.
 *
 * The graph arms run source, recursion and power sum under gr::scheduler::Simple on the caller's thread, with the
 * scheduler's flush_subnormals on and off. The kernel arms run the same arithmetic over one buffer with no graph, inside
 * a thread::detail::SubnormalFlushScope. BM_SUBNORMAL_FLUSH=on or BM_SUBNORMAL_FLUSH=off keeps the arms of that setting alone,
 * so that a counter run over the process sees one setting.
 */

namespace bm_subnormal {

inline constexpr float       kPole    = 1.0f - 1.0f / 64.0f;
inline constexpr std::size_t kSamples = 1'000'000UZ;
inline constexpr std::size_t kChunk   = 4096UZ;
inline constexpr std::size_t kRepeat  = 7UZ;
inline constexpr std::size_t kRounds  = 2UZ;

// a compiler barrier: orders the accesses around it without emitting an instruction
inline void barrier() noexcept { std::atomic_signal_fence(std::memory_order_acq_rel); }

// one sample of 1 followed by zeros
struct ImpulseSource : gr::Block<ImpulseSource> {
    gr::PortOut<float> out;

    std::size_t nTotal = 0UZ;

    GR_MAKE_REFLECTABLE(ImpulseSource, out);

    std::size_t _emitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nTotal - _emitted);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = _emitted + i == 0UZ ? 1.0f : 0.0f;
        }
        _emitted += n;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct OnePole : gr::Block<OnePole> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(OnePole, in, out);

    float _state = 0.0f;

    [[nodiscard]] float processOne(float value) noexcept {
        _state = kPole * _state + value;
        return _state;
    }
};

struct PowerSum : gr::Block<PowerSum> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(PowerSum, in);

    float       _power     = 0.0f;
    float       _last      = -1.0f;
    std::size_t _nReceived = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n     = inSpan.size();
        float             power = 0.0f;
        for (std::size_t i = 0UZ; i < n; ++i) {
            power += inSpan[i] * inSpan[i];
        }
        if (n > 0UZ) {
            _last = inSpan[n - 1UZ];
        }
        _power += power;
        _nReceived += n;
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

struct RunResult {
    std::size_t nReceived = 0UZ;
    float       last      = -1.0f; // the recursion's last output
    float       power     = 0.0f;
};

[[nodiscard]] RunResult runGraph(bool flush) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<ImpulseSource>();
    auto&     pole   = flow.emplaceBlock<OnePole>();
    auto&     sink   = flow.emplaceBlock<PowerSum>();
    source.nTotal    = kSamples;
    expect(flow.connect<"out", "in">(source, pole).has_value());
    expect(flow.connect<"out", "in">(pole, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{gr::property_map{{"flush_subnormals", flush}}};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
    return {sink._nReceived, sink._last, sink._power};
}

[[nodiscard]] RunResult runKernel(bool flush) {
    const gr::thread_pool::thread::detail::SubnormalFlushScope subnormalFlush(flush);
    std::vector<float>                                         input(kChunk, 0.0f);
    std::vector<float>                                         output(kChunk, 0.0f);
    float                                                      state = 0.0f;
    RunResult                                                  result;
    for (std::size_t done = 0UZ; done < kSamples; done += kChunk) {
        const std::size_t n = std::min(kChunk, kSamples - done);
        input[0]            = done == 0UZ ? 1.0f : 0.0f;
        for (std::size_t i = 0UZ; i < n; ++i) {
            state     = kPole * state + input[i];
            output[i] = state;
        }
        barrier();
        float power = 0.0f;
        for (std::size_t i = 0UZ; i < n; ++i) {
            power += output[i] * output[i];
        }
        barrier();
        result.power += power;
        result.nReceived += n;
    }
    result.last = state;
    return result;
}

// the arm ran with the flush it names: a flushed recursion ends at zero, an unflushed one on a subnormal
[[nodiscard]] bool endsAsExpected(const RunResult& result, bool flush) {
    const bool flushed = gr::thread_pool::thread::detail::kCanFlushSubnormals && flush;
    return result.nReceived == kSamples && (flushed ? result.last == 0.0f : std::fpclassify(result.last) == FP_SUBNORMAL);
}

[[nodiscard]] bool armSelected(bool flush) {
    const char* only = std::getenv("BM_SUBNORMAL_FLUSH");
    return only == nullptr || std::string_view(only) == (flush ? "on" : "off");
}

[[nodiscard]] auto named(std::string_view name) { return ::benchmark::benchmark<1UZ>{name}; }

} // namespace bm_subnormal

inline const boost::ut::suite<"subnormal flush"> _subnormal_flush = [] {
    using namespace boost::ut;
    using namespace bm_subnormal;

    for (std::size_t round = 1UZ; round <= kRounds; ++round) {
        for (const bool flush : {true, false}) {
            if (!armSelected(flush)) {
                continue;
            }
            const std::string_view setting = flush ? "on " : "off";

            const std::string graphName                = std::format("graph  flush {} round {}: impulse, one-pole, power sum", setting, round);
            named(graphName).repeat<kRepeat>(kSamples) = [flush] { expect(endsAsExpected(runGraph(flush), flush)); };

            const std::string kernelName                = std::format("kernel flush {} round {}: one-pole and power sum, no graph", setting, round);
            named(kernelName).repeat<kRepeat>(kSamples) = [flush] { expect(endsAsExpected(runKernel(flush), flush)); };
        }
    }
};

int main() { /* not needed by the UT framework */ }
