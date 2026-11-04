#include <benchmark.hpp>
#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief What the SIMD width the framework picks for a const processOne costs, against the same block run sample by sample.
 *
 * A const `processOne` that takes `vir::simd` arguments runs over vectors of the width `Block` picks; one that takes
 * `float` runs one sample per call. Each arm is a chain of eight blocks between a source and a sink:
 *
 *   - gain: one multiply per sample,
 *   - horner: a polynomial of degree 7 by Horner's rule, seven dependent multiply-adds,
 *   - muladd: two inputs, the chain's sample times a constant plus a side stream fanned out to every member.
 *
 * The plain-loop arm runs the same arithmetic over one buffer with no graph. The probe prints the lane count each
 * SIMD block receives.
 */

namespace bm_simd {

inline constexpr float                kGain        = 1.0000001f;
inline constexpr float                kMulAddScale = 0.5f;
inline constexpr float                kSourceValue = 0.5f;                                                                        // a block that returns its input fails every arm's check at this value
inline constexpr std::array<float, 8> kPolynomial  = {0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f, 0.0078125f, 0.0078125f}; // c0..c7
inline constexpr std::size_t          kChainLength = 8UZ;
inline constexpr std::size_t          kSamples     = 1UZ << 23;
inline constexpr std::size_t          kRepeat      = 5UZ;
inline constexpr std::size_t          kChunk       = 4096UZ;

template<typename V>
[[nodiscard]] constexpr V gainOf(V x) noexcept {
    return x * kGain;
}

template<typename V>
[[nodiscard]] constexpr V hornerOf(V x) noexcept {
    V acc = V(kPolynomial.back());
    for (std::size_t k = kPolynomial.size() - 1UZ; k-- > 0UZ;) {
        acc = acc * x + kPolynomial[k];
    }
    return acc;
}

template<typename V>
[[nodiscard]] constexpr V mulAddOf(V x, V side) noexcept {
    return x * kMulAddScale + side;
}

// a compiler barrier: orders the accesses around it without emitting an instruction
inline void barrier() noexcept { std::atomic_signal_fence(std::memory_order_acq_rel); }

struct GainSimd : gr::Block<GainSimd> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GainSimd, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V x) const noexcept {
        return gainOf(x);
    }
};

struct GainScalar : gr::Block<GainScalar> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(GainScalar, in, out);

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return gainOf(x); }
};

struct HornerSimd : gr::Block<HornerSimd> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HornerSimd, in, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V x) const noexcept {
        return hornerOf(x);
    }
};

struct HornerScalar : gr::Block<HornerScalar> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(HornerScalar, in, out);

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return hornerOf(x); }
};

struct MulAddSimd : gr::Block<MulAddSimd> {
    gr::PortIn<float>  in;
    gr::PortIn<float>  side;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(MulAddSimd, in, side, out);

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] constexpr V processOne(V x, V sideValue) const noexcept {
        return mulAddOf(x, sideValue);
    }
};

struct MulAddScalar : gr::Block<MulAddScalar> {
    gr::PortIn<float>  in;
    gr::PortIn<float>  side;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(MulAddScalar, in, side, out);

    [[nodiscard]] constexpr float processOne(float x, float sideValue) const noexcept { return mulAddOf(x, sideValue); }
};

struct LaneProbe : gr::Block<LaneProbe> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LaneProbe, in, out);

    mutable std::size_t _widestCall = 0UZ;

    template<gr::meta::t_or_simd<float> V>
    [[nodiscard]] V processOne(V x) const noexcept {
        if constexpr (gr::meta::any_simd<V>) {
            _widestCall = std::max(_widestCall, static_cast<std::size_t>(V::size()));
        }
        return x;
    }
};

static_assert(gr::traits::block::can_processOne_simd<GainSimd> && gr::traits::block::can_processOne_simd<HornerSimd> && gr::traits::block::can_processOne_simd<MulAddSimd>);
static_assert(!gr::traits::block::can_processOne_simd<GainScalar> && !gr::traits::block::can_processOne_simd<HornerScalar> && !gr::traits::block::can_processOne_simd<MulAddScalar>);

struct Source : gr::Block<Source> {
    gr::PortOut<float> out;

    std::size_t nTotal = 0UZ;

    GR_MAKE_REFLECTABLE(Source, out);

    std::size_t _emitted = 0UZ;

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nTotal - _emitted);
        std::fill_n(outSpan.begin(), n, kSourceValue);
        _emitted += n;
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct Sink : gr::Block<Sink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    float       _last      = 0.0f;
    std::size_t _nReceived = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        if (n > 0UZ) {
            _last = inSpan[n - 1UZ];
            _nReceived += n;
        }
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

struct RunResult {
    std::size_t nReceived = 0UZ;
    float       last      = 0.0f;
    std::size_t lanes     = 0UZ;
};

template<typename TChainBlock>
[[nodiscard]] RunResult runChain(std::size_t nSamples = kSamples) {
    using namespace boost::ut;
    constexpr bool kHasSideInput = requires(TChainBlock& block) { block.side; };

    gr::Graph flow;
    auto&     source                    = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.nTotal                       = nSamples;
    [[maybe_unused]] Source* sideSource = nullptr;
    if constexpr (kHasSideInput) {
        sideSource         = &flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("side")}});
        sideSource->nTotal = nSamples;
    }
    std::vector<TChainBlock*> chain;
    for (std::size_t i = 0UZ; i < kChainLength; ++i) {
        chain.push_back(&flow.emplaceBlock<TChainBlock>(gr::property_map{{"name", std::format("b{}", i)}}));
    }
    auto& sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});

    expect(flow.connect<"out", "in">(source, *chain.front()).has_value());
    for (std::size_t i = 1UZ; i < kChainLength; ++i) {
        expect(flow.connect<"out", "in">(*chain[i - 1UZ], *chain[i]).has_value());
    }
    expect(flow.connect<"out", "in">(*chain.back(), sink).has_value());
    if constexpr (kHasSideInput) {
        for (TChainBlock* member : chain) {
            expect(flow.connect<"out", "side">(*sideSource, *member).has_value());
        }
    }

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler(gr::property_map{{"max_work_items", std::uint64_t{kChunk}}}); // std::size_t converts ambiguously to pmt::Value on wasm32
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    RunResult result{sink._nReceived, sink._last, 0UZ};
    if constexpr (requires(TChainBlock& block) { block._widestCall; }) {
        for (TChainBlock* member : chain) {
            result.lanes = std::max(result.lanes, member->_widestCall);
        }
    }
    return result;
}

enum class Kind { gain, horner, muladd };

[[nodiscard]] float plainLoop(Kind kind, std::size_t nSamples = kSamples) {
    std::vector<float> buffer(kChunk);
    std::vector<float> side(kChunk, kSourceValue);
    float              last = 0.0f;
    for (std::size_t done = 0UZ; done < nSamples; done += kChunk) {
        const std::size_t n = std::min(kChunk, nSamples - done);
        std::fill_n(buffer.begin(), n, kSourceValue);
        barrier();
        for (std::size_t stage = 0UZ; stage < kChainLength; ++stage) {
            for (std::size_t i = 0UZ; i < n; ++i) {
                switch (kind) {
                case Kind::gain: buffer[i] = gainOf(buffer[i]); break;
                case Kind::horner: buffer[i] = hornerOf(buffer[i]); break;
                case Kind::muladd: buffer[i] = mulAddOf(buffer[i], side[i]); break;
                }
            }
            barrier();
        }
        last = buffer[n - 1UZ];
    }
    return last;
}

[[nodiscard]] float expectedLast(Kind kind) noexcept {
    float x = kSourceValue;
    for (std::size_t stage = 0UZ; stage < kChainLength; ++stage) {
        switch (kind) {
        case Kind::gain: x = gainOf(x); break;
        case Kind::horner: x = hornerOf(x); break;
        case Kind::muladd: x = mulAddOf(x, kSourceValue); break;
        }
    }
    return x;
}

// eight gains move 0.5f by 4.8e-7; the gain check's tolerance is below that
[[nodiscard]] constexpr float toleranceOf(Kind kind) noexcept { return kind == Kind::gain ? 1e-7f : 1e-5f; }

[[nodiscard]] auto named(std::string_view name) { return ::benchmark::benchmark<1UZ>{name}; }

template<typename TChainBlock>
void addChainArm(std::string_view name, Kind kind) {
    using namespace boost::ut;
    named(name).repeat<kRepeat>(kSamples) = [kind] {
        const RunResult result = runChain<TChainBlock>();
        expect(eq(result.nReceived, kSamples));
        expect(approx(result.last, expectedLast(kind), toleranceOf(kind)));
    };
}

} // namespace bm_simd

inline const boost::ut::suite<"processOne SIMD width"> _processOneSimdWidth = [] {
    using namespace boost::ut;
    using namespace benchmark;
    using namespace bm_simd;

    "lanes per SIMD processOne call"_test = [] {
        const RunResult result = runChain<LaneProbe>(1UZ << 16);
        expect(gt(result.lanes, 0UZ));
        std::println("\nnative float lanes {}, lanes per SIMD processOne call {}\n", gr::meta::simdize<float>::size(), result.lanes);
    };

    addChainArm<GainSimd>("gain    N=8 SIMD processOne", Kind::gain);
    addChainArm<GainScalar>("gain    N=8 float processOne", Kind::gain);
    named("gain    N=8 plain loop (one buffer, no graph)").repeat<kRepeat>(kSamples) = [] { expect(approx(plainLoop(Kind::gain), expectedLast(Kind::gain), toleranceOf(Kind::gain))); };
    addChainArm<HornerSimd>("horner  N=8 SIMD processOne", Kind::horner);
    addChainArm<HornerScalar>("horner  N=8 float processOne", Kind::horner);
    named("horner  N=8 plain loop (one buffer, no graph)").repeat<kRepeat>(kSamples) = [] { expect(approx(plainLoop(Kind::horner), expectedLast(Kind::horner), toleranceOf(Kind::horner))); };
    addChainArm<MulAddSimd>("muladd  N=8 SIMD processOne", Kind::muladd);
    addChainArm<MulAddScalar>("muladd  N=8 float processOne", Kind::muladd);
    named("muladd  N=8 plain loop (one buffer, no graph)").repeat<kRepeat>(kSamples) = [] { expect(approx(plainLoop(Kind::muladd), expectedLast(Kind::muladd), toleranceOf(Kind::muladd))); };
};

int main() { /* not needed by the UT framework */ }
