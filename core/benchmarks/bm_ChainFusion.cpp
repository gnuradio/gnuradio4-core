#include <benchmark.hpp>
#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <print>
#include <string>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief What a chain of trivial 1:1 blocks costs, and where that cost sits.
 *
 * Every edge in a series graph is a write plus a read pass of every sample through a
 * double-mapped ring, so a chain of N no-op blocks pays N ring round-trips for work that
 * would fuse into a single pass over one scratch buffer. The arms separate the candidate
 * costs into what is already reachable with the knobs the scheduler has today and
 * what only fusion can remove:
 *
 *   - edge size: sweeps the ring over L1/L2/L3/DRAM working sets (8Ki..2Mi samples),
 *   - block order: gr::scheduler::Simple runs blocks in graph insertion order, so
 *     emplacing the chain forwards vs backwards is the locality-order knob,
 *   - processOne vs processBulk: per-sample dispatch against one call per chunk,
 *   - scratch: the same arithmetic over one buffer with no graph, the upper
 *     bound any fused execution could reach.
 *
 * The probe test prints the observed work-call granularity and execution order.
 */

namespace bm_chain {

inline constexpr float       kGain    = 1.0000001f;
inline constexpr std::size_t kSamples = 10'000'000UZ;
inline constexpr std::size_t kRepeat  = 7UZ;

inline void barrier() noexcept { asm volatile("" : : : "memory"); }

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    std::size_t nTotal    = 0UZ;
    bool        touchData = true;

    GR_MAKE_REFLECTABLE(Source, out);

    std::size_t _emitted = 0UZ;
    std::size_t _nCalls  = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nTotal - _emitted);
        if (touchData) {
            for (std::size_t i = 0UZ; i < n; ++i) {
                outSpan[i] = 1.0f;
            }
        }
        _emitted += n;
        _nCalls += n > 0UZ ? 1UZ : 0UZ;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct GainOne : gr::Block<GainOne> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GainOne, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * kGain; }
};

struct GainBulk : gr::Block<GainBulk> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GainBulk, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i] * kGain;
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    bool touchData = true;

    GR_MAKE_REFLECTABLE(Sink, in);

    float       _acc       = 0.0f;
    std::size_t _nReceived = 0UZ;
    std::size_t _nCalls    = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n   = inSpan.size();
        float             acc = 0.0f;
        if (touchData) {
            for (std::size_t i = 0UZ; i < n; ++i) {
                acc += inSpan[i];
            }
        }
        _acc += acc;
        _nReceived += n;
        _nCalls += n > 0UZ ? 1UZ : 0UZ;
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

enum class Order { forward, reversed };

struct RunStats {
    float                    acc          = 0.0f;
    std::size_t              nReceived    = 0UZ;
    std::size_t              nSourceCalls = 0UZ;
    std::size_t              nSinkCalls   = 0UZ;
    std::vector<std::string> executionOrder;
};

template<typename TChainBlock, gr::scheduler::ExecutionPolicy policy>
[[nodiscard]] RunStats runChain(std::size_t nChain, std::size_t minBufferSize, Order order = Order::forward, bool touchData = true, std::size_t nSamples = kSamples) {
    using namespace boost::ut;

    gr::Graph                                        flow;
    Source*                                          source = nullptr;
    Sink*                                            sink   = nullptr;
    std::vector<std::reference_wrapper<TChainBlock>> chain;
    chain.reserve(nChain);

    const auto addSource = [&] { source = &flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}}); };
    const auto addSink   = [&] { sink = &flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}}); };
    const auto addChain  = [&](std::size_t i) { chain.emplace_back(flow.emplaceBlock<TChainBlock>(gr::property_map{{"name", std::format("g{}", i)}})); };

    if (order == Order::forward) {
        addSource();
        for (std::size_t i = 0UZ; i < nChain; ++i) {
            addChain(i);
        }
        addSink();
    } else {
        addSink();
        for (std::size_t i = nChain; i-- > 0UZ;) {
            addChain(i);
        }
        std::ranges::reverse(chain);
        addSource();
    }
    source->nTotal    = nSamples;
    source->touchData = touchData;
    sink->touchData   = touchData;

    const gr::EdgeParameters edge{.minBufferSize = minBufferSize};
    if (nChain == 0UZ) {
        expect(flow.connect<"out", "in">(*source, *sink, edge).has_value());
    } else {
        expect(flow.connect<"out", "in">(*source, chain.front().get(), edge).has_value());
        for (std::size_t i = 1UZ; i < nChain; ++i) {
            expect(flow.connect<"out", "in">(chain[i - 1UZ].get(), chain[i].get(), edge).has_value());
        }
        expect(flow.connect<"out", "in">(chain.back().get(), *sink, edge).has_value());
    }

    gr::scheduler::Simple<policy> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    RunStats stats{sink->_acc, sink->_nReceived, source->_nCalls, sink->_nCalls, {}};
    for (const auto& job : *scheduler.jobs()) {
        for (const auto& block : job) {
            stats.executionOrder.emplace_back(block->name());
        }
    }
    return stats;
}

[[nodiscard]] float runScratch(std::size_t nChain, std::size_t chunk, std::size_t nSamples = kSamples) {
    std::vector<float> scratch(chunk);
    float              acc = 0.0f;

    for (std::size_t done = 0UZ; done < nSamples; done += chunk) {
        const std::size_t n = std::min(chunk, nSamples - done);
        for (std::size_t i = 0UZ; i < n; ++i) {
            scratch[i] = 1.0f;
        }
        barrier();
        for (std::size_t stage = 0UZ; stage < nChain; ++stage) {
            for (std::size_t i = 0UZ; i < n; ++i) {
                scratch[i] *= kGain;
            }
            barrier();
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            acc += scratch[i];
        }
        barrier();
    }
    return acc;
}

[[nodiscard]] std::string sizeLabel(std::size_t nElements) { return nElements >= (1UZ << 20) ? std::format("{}Mi", nElements >> 20) : std::format("{}Ki", nElements >> 10); }

[[nodiscard]] auto named(std::string_view name) { return ::benchmark::benchmark<1UZ>{name}; }

inline constexpr std::array  kChainLengths = {0UZ, 1UZ, 2UZ, 4UZ, 8UZ};
inline constexpr std::array  kRingSizes    = {8192UZ, 32768UZ, 65536UZ, 2097152UZ}; // 65536 == graph::defaultMinBufferSize()
inline constexpr std::array  kChunkSizes   = {4096UZ, 65536UZ};
inline constexpr std::size_t kDefaultRing  = 65536UZ;

} // namespace bm_chain

inline const boost::ut::suite<"chain fusion"> _chain_fusion = [] {
    using namespace boost::ut;
    using namespace benchmark;
    using namespace bm_chain;
    using enum gr::scheduler::ExecutionPolicy;

    "work-call granularity and execution order"_test = [] {
        std::println("");
        for (const Order order : {Order::forward, Order::reversed}) {
            for (const std::size_t ring : kRingSizes) {
                const RunStats stats = runChain<GainOne, singleThreaded>(4UZ, ring, order, true, 1'000'000UZ);
                expect(eq(stats.nReceived, 1'000'000UZ));
                std::string blocks;
                for (const std::string& block : stats.executionOrder) {
                    blocks += blocks.empty() ? block : std::format(" {}", block);
                }
                std::println("probe N=4 ring={:>7} order=[{}] : {:.0f} samples/source-call, {:.0f} samples/sink-call", //
                    sizeLabel(ring), blocks, 1e6 / static_cast<double>(stats.nSourceCalls), 1e6 / static_cast<double>(stats.nSinkCalls));
            }
        }
        for (const bool touch : {true, false}) {
            for (const std::size_t nChain : kChainLengths) {
                const auto     t0    = std::chrono::steady_clock::now();
                const RunStats stats = runChain<GainOne, singleThreaded>(nChain, kDefaultRing, Order::forward, touch);
                const auto     ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                expect(eq(stats.nReceived, kSamples));
                std::println("probe N={} ring=64Ki touch={:<5} : {:5.2f} ns/input, {:.0f} samples/source-call, {:.0f} samples/sink-call", //
                    nChain, touch, static_cast<double>(ns) / static_cast<double>(kSamples),                                               //
                    static_cast<double>(kSamples) / static_cast<double>(stats.nSourceCalls), static_cast<double>(kSamples) / static_cast<double>(stats.nSinkCalls));
            }
        }
        std::println("");
    };

    for (const std::size_t nChain : kChainLengths) {
        for (const std::size_t ring : kRingSizes) {
            const std::string name                = std::format("processOne  N={} ring={:>7} fwd  singleThreaded", nChain, sizeLabel(ring));
            named(name).repeat<kRepeat>(kSamples) = [nChain, ring] { expect(eq(runChain<GainOne, singleThreaded>(nChain, ring).nReceived, kSamples)); };
        }

        const std::string noTouchName                = std::format("processOne  N={} ring={:>7} fwd  singleThreaded, src/snk do not touch data", nChain, sizeLabel(kDefaultRing));
        named(noTouchName).repeat<kRepeat>(kSamples) = [nChain] { expect(eq(runChain<GainOne, singleThreaded>(nChain, kDefaultRing, Order::forward, false).nReceived, kSamples)); };

        if (nChain >= 2UZ) {
            for (const std::size_t ring : {8192UZ, kDefaultRing}) {
                const std::string blockingName                = std::format("processOne  N={} ring={:>7} fwd  singleThreadedBlocking", nChain, sizeLabel(ring));
                named(blockingName).repeat<kRepeat>(kSamples) = [nChain, ring] { expect(eq(runChain<GainOne, singleThreadedBlocking>(nChain, ring).nReceived, kSamples)); };

                const std::string reversedName                = std::format("processOne  N={} ring={:>7} rev  singleThreaded", nChain, sizeLabel(ring));
                named(reversedName).repeat<kRepeat>(kSamples) = [nChain, ring] { expect(eq(runChain<GainOne, singleThreaded>(nChain, ring, Order::reversed).nReceived, kSamples)); };
            }

            const std::string bulkName                = std::format("processBulk N={} ring={:>7} fwd  singleThreaded", nChain, sizeLabel(kDefaultRing));
            named(bulkName).repeat<kRepeat>(kSamples) = [nChain] { expect(eq(runChain<GainBulk, singleThreaded>(nChain, kDefaultRing).nReceived, kSamples)); };
        }

        for (const std::size_t chunk : kChunkSizes) {
            const std::string scratchName                = std::format("scratch     N={} chunk={:>6} (fusion upper bound)", nChain, sizeLabel(chunk));
            named(scratchName).repeat<kRepeat>(kSamples) = [nChain, chunk] { expect(runScratch(nChain, chunk) > 0.0f); };
        }
    }
};

int main() { /* not needed by the UT framework */ }
