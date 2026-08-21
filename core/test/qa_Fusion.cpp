#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief Fused execution must be indistinguishable from unfused execution.
 *
 * Every scenario builds one graph twice and runs it with `enable_fusion` false and true over a matrix of fused chunk
 * sizes and edge ring sizes, then compares the sink's samples byte for byte and its tags as an ordered sequence of
 * (absolute stream index, property_map) pairs. The blocks are defined here: gnuradio4-core carries no standard block
 * library, so a core test may not depend on one.
 */

namespace qa_fusion {

using namespace gr;

inline constexpr std::size_t kSamples    = 4096UZ;
inline constexpr float       kGainFactor = 1.0009765625f;

struct TagRecord {
    std::size_t  index{};
    property_map map{};

    bool operator==(const TagRecord&) const = default;
};

struct Source : Block<Source> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    std::size_t            nTotal = kSamples;
    std::vector<TagRecord> tagsToEmit; // ordered by index
    std::size_t            nStopCalls = 0UZ;

    std::size_t _emitted    = 0UZ;
    std::size_t _nextTag    = 0UZ;
    std::size_t _chunkIndex = 0UZ;

    void stop() { nStopCalls++; }

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        static constexpr std::array<std::size_t, 6> kPrimes{7UZ, 13UZ, 31UZ, 61UZ, 127UZ, 251UZ}; // irregular publish lengths

        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min({kPrimes[_chunkIndex++ % kPrimes.size()], outSpan.size(), nTotal - _emitted});
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<float>(_emitted + i);
        }
        while (_nextTag < tagsToEmit.size() && tagsToEmit[_nextTag].index < _emitted + n) {
            const TagRecord& tag = tagsToEmit[_nextTag];
            outSpan.publishTag(tag.map, tag.index >= _emitted ? tag.index - _emitted : 0UZ);
            _nextTag++;
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Sink : Block<Sink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::vector<float>     samples;
    std::vector<TagRecord> tags;
    std::size_t            nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        inSpan.consumeTags(n);
        samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

struct Gain : Block<Gain> {
    PortIn<float>  in;
    PortOut<float> out;

    float gain = kGainFactor;

    GR_MAKE_REFLECTABLE(Gain, in, out, gain);

    std::size_t nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    // deduced argument: this is the member that exercises the SIMD dispatch
    [[nodiscard]] constexpr auto processOne(auto value) const noexcept { return value * gain; }
};

struct Quantize : Block<Quantize> {
    PortIn<float>  in;
    PortOut<float> out;

    float step = 4.0f;

    GR_MAKE_REFLECTABLE(Quantize, in, out, step);

    std::size_t nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    // integer truncation keeps this off the SIMD path, so a run mixes both const processOne loop shapes
    [[nodiscard]] float processOne(float value) const noexcept { return step * static_cast<float>(static_cast<std::int64_t>(value / step)); }
};

struct RunningMean : Block<RunningMean> {
    PortIn<float>  in;
    PortOut<float> out;

    float alpha = 0.125f;

    GR_MAKE_REFLECTABLE(RunningMean, in, out, alpha);

    float       mean       = 0.0f;
    std::size_t nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    // non-const: a run holds at most one of these and it is the last member
    [[nodiscard]] float processOne(float value) noexcept {
        mean += alpha * (value - mean);
        return value - mean;
    }
};

struct RateOwner : Block<RateOwner> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1.0f;

    GR_MAKE_REFLECTABLE(RateOwner, in, out, sample_rate);

    std::size_t rateChangeAt  = std::numeric_limits<std::size_t>::max();
    float       observedAtTag = 0.0f;
    std::size_t nStopCalls    = 0UZ;
    std::size_t _index        = 0UZ;

    void stop() { nStopCalls++; }

    [[nodiscard]] float processOne(float value) noexcept {
        if (this->inputTagsPresent()) {
            std::ignore = this->mergedInputTag();
            if (rateChangeAt == std::numeric_limits<std::size_t>::max() && sample_rate != 1.0f) {
                rateChangeAt  = _index;
                observedAtTag = sample_rate;
            }
        }
        _index++;
        return value + sample_rate;
    }
};

struct TagAt : Block<TagAt> {
    PortIn<float>  in;
    PortOut<float> out;

    gr::Size_t tag_at = 0U;

    GR_MAKE_REFLECTABLE(TagAt, in, out, tag_at);

    std::size_t nStopCalls = 0UZ;
    std::size_t _index     = 0UZ;

    void stop() { nStopCalls++; }

    [[nodiscard]] float processOne(float value) {
        if (_index == static_cast<std::size_t>(tag_at)) {
            this->publishTag(property_map{{"marker", static_cast<gr::Size_t>(_index)}}, 0UZ);
        }
        _index++;
        return value;
    }
};

struct BulkGain : Block<BulkGain> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(BulkGain, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i];
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct AsyncGain : Block<AsyncGain> {
    PortIn<float, Async> in;
    PortOut<float>       out;

    GR_MAKE_REFLECTABLE(AsyncGain, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct Decimate2 : Block<Decimate2, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Decimate2, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size() / 2UZ, outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[2UZ * i];
        }
        std::ignore = inSpan.consume(2UZ * n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Decimate4 : Block<Decimate4, Resampling<4U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Decimate4, in, out);

    std::size_t nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size() / 4UZ, outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[4UZ * i];
        }
        std::ignore = inSpan.consume(4UZ * n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Interpolate3 : Block<Interpolate3, Resampling<1U, 3U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Interpolate3, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size() / 3UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            for (std::size_t j = 0UZ; j < 3UZ; ++j) {
                outSpan[3UZ * i + j] = inSpan[i] + static_cast<float>(j);
            }
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(3UZ * n);
        return work::Status::OK;
    }
};

struct ThreeToTwo : Block<ThreeToTwo, Resampling<3U, 2U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(ThreeToTwo, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size() / 3UZ, outSpan.size() / 2UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[2UZ * i]       = inSpan[3UZ * i];
            outSpan[2UZ * i + 1UZ] = inSpan[3UZ * i + 2UZ];
        }
        std::ignore = inSpan.consume(3UZ * n);
        outSpan.publish(2UZ * n);
        return work::Status::OK;
    }
};

struct TwoToThree : Block<TwoToThree, Resampling<2U, 3U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(TwoToThree, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size() / 2UZ, outSpan.size() / 3UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[3UZ * i]       = inSpan[2UZ * i];
            outSpan[3UZ * i + 1UZ] = inSpan[2UZ * i] + inSpan[2UZ * i + 1UZ];
            outSpan[3UZ * i + 2UZ] = inSpan[2UZ * i + 1UZ];
        }
        std::ignore = inSpan.consume(2UZ * n);
        outSpan.publish(3UZ * n);
        return work::Status::OK;
    }
};

// state carried across chunk boundaries: a run admits any number of these where it admits one stateful processOne
struct BulkAccumulate : Block<BulkAccumulate> {
    PortIn<float>  in;
    PortOut<float> out;

    float alpha = 0.125f;

    GR_MAKE_REFLECTABLE(BulkAccumulate, in, out, alpha);

    float       mean       = 0.0f;
    std::size_t nStopCalls = 0UZ;

    void stop() { nStopCalls++; }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        for (std::size_t i = 0UZ; i < n; ++i) {
            mean += alpha * (inSpan[i] - mean);
            outSpan[i] = inSpan[i] - mean;
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

// ForwardTagPropagation is excluded from a composed segment and admitted for a bulk member
struct BulkForwarder : Block<BulkForwarder, ForwardTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(BulkForwarder, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

// the ratio is writable while the graph runs, so a fused run must re-check it rather than rely on the plan
struct VariableDecimate : Block<VariableDecimate, Resampling<2U, 1U, false>> {
    PortIn<float>  in;
    PortOut<float> out;

    gr::Size_t retune_after   = std::numeric_limits<gr::Size_t>::max(); // input samples, a multiple of the initial ratio
    gr::Size_t retune_to      = 2U;
    bool       stop_at_retune = false; // end the chunk exactly at the retune point, leaving the reservation part-published

    GR_MAKE_REFLECTABLE(VariableDecimate, in, out, retune_after, retune_to, stop_at_retune);

    std::size_t nStopCalls     = 0UZ;
    std::size_t consumed       = 0UZ;
    std::size_t switchedAt     = 0UZ; ///< input samples consumed when the ratio changed; where it lands is chunk-dependent
    bool        retuned        = false;
    bool        underPublished = false;

    void stop() { nStopCalls++; }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t decim = static_cast<std::size_t>(input_chunk_size);
        std::size_t       n     = std::min(inSpan.size() / decim, outSpan.size());
        if (stop_at_retune && !retuned && consumed < static_cast<std::size_t>(retune_after)) {
            n              = std::min(n, (static_cast<std::size_t>(retune_after) - consumed) / decim);
            underPublished = underPublished || n < outSpan.size();
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[decim * i];
        }
        std::ignore = inSpan.consume(decim * n);
        outSpan.publish(n);
        consumed += decim * n;

        if (!retuned && consumed >= static_cast<std::size_t>(retune_after)) {
            retuned     = true;
            switchedAt  = consumed;
            std::ignore = this->settings().setStaged({{"input_chunk_size", retune_to}}); // the route a settingsChanged() handler takes
        }
        return work::Status::OK;
    }
};

struct StrideGain : Block<StrideGain, Stride<2U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(StrideGain, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct TwoInputs : Block<TwoInputs> {
    PortIn<float>  in;
    PortIn<float>  reference;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(TwoInputs, in, reference, out);

    [[nodiscard]] constexpr float processOne(float value, float other) const noexcept { return value + other; }
};

struct RunResult {
    std::vector<float>       samples;
    std::vector<TagRecord>   tags;
    std::vector<std::size_t> runSizes;
    std::vector<std::size_t> runChunks;
    bool                     anyRatioLatched = false;
};

[[nodiscard]] inline std::vector<std::size_t> collectRunSizes(const std::vector<std::vector<fusion::RunPlan>>& plan) {
    std::vector<std::size_t> runSizes;
    for (const auto& job : plan) {
        for (const fusion::RunPlan& run : job) {
            runSizes.push_back(run.members.size());
        }
    }
    return runSizes;
}

// the run entries survive in the job list until a graph edit dissolves them, so the executor's own state is readable
template<typename TScheduler>
inline void collectRunState(const TScheduler& scheduler, RunResult& result) {
    for (const auto& job : *scheduler.jobs()) {
        for (const auto& entry : job) {
            if (const auto* run = dynamic_cast<const fusion::FusedRun*>(entry.get()); run != nullptr) {
                result.runChunks.push_back(run->chunkSize());
                result.anyRatioLatched = result.anyRatioLatched || run->isRatioLatched();
            }
        }
    }
}

template<gr::scheduler::ExecutionPolicy policy, typename TBuild>
[[nodiscard]] RunResult runOnce(TBuild&& build, bool fusion, std::size_t chunkSamples) {
    using namespace boost::ut;

    gr::Graph flow;
    Sink*     sink = build(flow);

    const gr::property_map        schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunkSamples)}};
    gr::scheduler::Simple<policy> scheduler{schedulerSettings};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    RunResult result{sink->samples, sink->tags, collectRunSizes(scheduler.fusionPlan()), {}, false};
    collectRunState(scheduler, result);
    return result;
}

// planning happens in init(), so a graph that is never started still yields its plan
template<typename TBuild>
[[nodiscard]] std::vector<std::size_t> planOnly(TBuild&& build) {
    using namespace boost::ut;

    gr::Graph flow;
    build(flow);

    const gr::property_map                                                schedulerSettings{{"enable_fusion", true}};
    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{schedulerSettings};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.changeStateTo(lifecycle::State::INITIALISED).has_value());
    return collectRunSizes(scheduler.fusionPlan());
}

inline constexpr std::array kChunkSizes = {1UZ, 3UZ, 256UZ, 4096UZ, 65536UZ};
inline constexpr std::array kRingSizes  = {8192UZ, 65536UZ};

template<gr::scheduler::ExecutionPolicy policy = gr::scheduler::ExecutionPolicy::singleThreaded, typename TBuild>
void expectFusedMatchesUnfused(std::string_view scenario, TBuild&& build, std::size_t expectedRunLength, std::size_t expectedSamples = kSamples) {
    using namespace boost::ut;

    for (const std::size_t ring : kRingSizes) {
        const RunResult reference = runOnce<policy>([&build, ring](gr::Graph& flow) { return build(flow, ring); }, false, 0UZ);
        expect(eq(reference.samples.size(), expectedSamples)) << scenario;
        expect(reference.runSizes.empty()) << scenario;

        for (const std::size_t chunk : kChunkSizes) {
            const RunResult   fused = runOnce<policy>([&build, ring](gr::Graph& flow) { return build(flow, ring); }, true, chunk);
            const std::string what  = std::format("{} ring={} chunk={}", scenario, ring, chunk);

            expect(eq(fused.runSizes.size(), 1UZ)) << what;
            if (!fused.runSizes.empty()) {
                expect(eq(fused.runSizes[0], expectedRunLength)) << what;
            }
            expect(eq(fused.samples.size(), reference.samples.size())) << what;
            expect(std::ranges::equal(fused.samples, reference.samples)) << what << "samples differ";
            expect(eq(fused.tags.size(), reference.tags.size())) << what << "tag count differs";
            expect(fused.tags == reference.tags) << what << "tag sequence differs";
        }
    }
}

struct ChainShape {
    std::size_t nStatelessPairs = 1UZ; // one pair is Gain -> Quantize
    bool        trailingMean    = false;
};

[[nodiscard]] inline Sink* buildChain(gr::Graph& flow, std::size_t ring, ChainShape shape, const std::vector<TagRecord>& tags = {}) {
    using namespace boost::ut;
    const gr::EdgeParameters edge{.minBufferSize = ring};

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;

    auto& firstGain = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain0")}});
    expect(flow.connect<"out", "in">(source, firstGain).has_value());

    Quantize* tail = std::addressof(flow.emplaceBlock<Quantize>(gr::property_map{{"name", std::string("quant0")}}));
    expect(flow.connect<"out", "in">(firstGain, *tail, edge).has_value());

    for (std::size_t i = 1UZ; i < shape.nStatelessPairs; ++i) {
        auto& gain = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::format("gain{}", i)}});
        expect(flow.connect<"out", "in">(*tail, gain, edge).has_value());
        auto& quantize = flow.emplaceBlock<Quantize>(gr::property_map{{"name", std::format("quant{}", i)}});
        expect(flow.connect<"out", "in">(gain, quantize, edge).has_value());
        tail = std::addressof(quantize);
    }

    auto& sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    if (shape.trailingMean) {
        auto& mean = flow.emplaceBlock<RunningMean>(gr::property_map{{"name", std::string("mean")}});
        expect(flow.connect<"out", "in">(*tail, mean, edge).has_value());
        expect(flow.connect<"out", "in">(mean, sink, edge).has_value());
    } else {
        expect(flow.connect<"out", "in">(*tail, sink, edge).has_value());
    }
    return std::addressof(sink);
}

// source -> TMiddle... -> sink, emplaced and connected in that order; a braced initializer fixes the emplacement order
template<typename... TMiddle>
[[nodiscard]] inline Sink* buildSeries(gr::Graph& flow, std::size_t ring, const std::vector<TagRecord>& tags = {}) {
    using namespace boost::ut;
    const gr::EdgeParameters edge{.minBufferSize = ring};

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;

    std::tuple<TMiddle&...> middle{flow.emplaceBlock<TMiddle>()...};
    auto&                   sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});

    const auto link = [&flow, &edge](auto& from, auto& to) { expect(flow.connect<"out", "in">(from, to, edge).has_value()); };
    link(source, std::get<0UZ>(middle));
    [&]<std::size_t... I>(std::index_sequence<I...>) { (link(std::get<I>(middle), std::get<I + 1UZ>(middle)), ...); }(std::make_index_sequence<sizeof...(TMiddle) - 1UZ>{});
    link(std::get<sizeof...(TMiddle) - 1UZ>(middle), sink);
    return std::addressof(sink);
}

struct RetuneArm {
    std::vector<float> samples;
    std::size_t        switchedAt{};
    bool               retuned{};
    bool               latched{};
    std::size_t        nRuns{};
    bool               underPublished{};
};

// the member's own state is read while the scheduler that owns it is still alive
[[nodiscard]] inline RetuneArm runRetuneChain(bool fusion, std::size_t chunkSamples, bool stopAtRetune = false) {
    using namespace boost::ut;
    const gr::EdgeParameters edge{.minBufferSize = 65536UZ};

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    auto&     gain   = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
    auto&     decim  = flow.emplaceBlock<VariableDecimate>(gr::property_map{{"name", std::string("decim")}, {"retune_after", 2048U}, {"retune_to", 4U}, {"stop_at_retune", stopAtRetune}});
    auto&     sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    expect(flow.connect<"out", "in">(source, gain, edge).has_value());
    expect(flow.connect<"out", "in">(gain, decim, edge).has_value());
    expect(flow.connect<"out", "in">(decim, sink, edge).has_value());

    const gr::property_map                                                schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunkSamples)}};
    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{schedulerSettings};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    RetuneArm arm{sink.samples, decim.switchedAt, decim.retuned, false, collectRunSizes(scheduler.fusionPlan()).size(), decim.underPublished};
    for (const auto& job : *scheduler.jobs()) {
        for (const auto& entry : job) {
            if (const auto* run = dynamic_cast<const fusion::FusedRun*>(entry.get()); run != nullptr) {
                arm.latched = arm.latched || run->isRatioLatched();
            }
        }
    }
    return arm;
}

// every second sample at 2:1 up to the switch, every fourth after it, and the last partial chunk dropped at end of stream
[[nodiscard]] inline std::vector<float> expectedRetuneSamples(std::size_t switchedAt) {
    std::vector<float> expected;
    for (std::size_t i = 0UZ; i < switchedAt; i += 2UZ) {
        expected.push_back(static_cast<float>(i) * kGainFactor);
    }
    for (std::size_t i = switchedAt; i + 4UZ <= kSamples; i += 4UZ) {
        expected.push_back(static_cast<float>(i) * kGainFactor);
    }
    return expected;
}

struct RateArm {
    std::vector<float> samples;
    std::size_t        rateChangeAt{};
    float              observedAtTag{};
};

[[nodiscard]] inline Sink* buildRateChain(gr::Graph& flow, std::size_t ring, const std::vector<TagRecord>& tags, RateOwner** observed) {
    using namespace boost::ut;
    const gr::EdgeParameters edge{.minBufferSize = ring};

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;
    auto& gain        = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
    auto& rate        = flow.emplaceBlock<RateOwner>(gr::property_map{{"name", std::string("rate")}});
    auto& sink        = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    if (observed != nullptr) {
        *observed = std::addressof(rate);
    }
    expect(flow.connect<"out", "in">(source, gain, edge).has_value());
    expect(flow.connect<"out", "in">(gain, rate, edge).has_value());
    expect(flow.connect<"out", "in">(rate, sink, edge).has_value());
    return std::addressof(sink);
}

// the member's own state is read while the scheduler that owns it is still alive
[[nodiscard]] inline RateArm runRateChain(const std::vector<TagRecord>& tags, bool fusion, std::size_t chunkSamples) {
    using namespace boost::ut;

    gr::Graph  flow;
    RateOwner* rate = nullptr;
    Sink*      sink = buildRateChain(flow, 65536UZ, tags, &rate);

    const gr::property_map                                                schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunkSamples)}};
    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{schedulerSettings};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return RateArm{sink->samples, rate->rateChangeAt, rate->observedAtTag};
}

} // namespace qa_fusion

const boost::ut::suite<"fusion"> _fusion = [] {
    using namespace boost::ut;
    using namespace qa_fusion;
    using enum gr::scheduler::ExecutionPolicy;

    "two stateless members produce the same stream"_test = [] { expectFusedMatchesUnfused("two stateless", [](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{1UZ, false}); }, 2UZ); };

    "four stateless members produce the same stream"_test = [] { expectFusedMatchesUnfused("four stateless", [](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{2UZ, false}); }, 4UZ); };

    "a trailing accumulator advances exactly once per sample"_test = [] { expectFusedMatchesUnfused("trailing accumulator", [](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{2UZ, true}); }, 5UZ); };

    "a trailing accumulator is identical under the blocking policy"_test = [] { expectFusedMatchesUnfused<singleThreadedBlocking>("trailing accumulator, blocking", [](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{1UZ, true}); }, 3UZ); };

    "tags at chunk-critical offsets keep their positions"_test = [] {
        const std::vector<TagRecord> tags{
            TagRecord{0UZ, {{"signal_name", std::string("at-zero")}}},         //
            TagRecord{255UZ, {{"signal_name", std::string("before-chunk")}}},  //
            TagRecord{256UZ, {{"signal_name", std::string("chunk-aligned")}}}, //
            TagRecord{257UZ, {{"signal_name", std::string("after-chunk")}}},   //
            TagRecord{1000UZ, {{"signal_name", std::string("odd")}}},          //
            TagRecord{4095UZ, {{"signal_name", std::string("last-sample")}}},  //
        };
        expectFusedMatchesUnfused("tag offsets", [&tags](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{2UZ, false}, tags); }, 4UZ);
    };

    "tags denser than one chunk all survive"_test = [] {
        std::vector<TagRecord> tags;
        for (std::size_t i = 0UZ; i < 64UZ; ++i) {
            tags.push_back(TagRecord{300UZ + i, {{"signal_name", std::format("dense-{}", i)}}});
        }
        expectFusedMatchesUnfused("dense tags", [&tags](gr::Graph& flow, std::size_t ring) { return buildChain(flow, ring, ChainShape{1UZ, true}, tags); }, 3UZ);
    };

    "a forwarded key owned by a later member is substituted in run order"_test = [] {
        const std::vector<TagRecord> tags{
            TagRecord{0UZ, {{"sample_rate", 48000.0f}, {"signal_name", std::string("start")}, {"private_key", 7.0f}}}, //
            TagRecord{1500UZ, {{"sample_rate", 96000.0f}}},                                                            //
        };
        expectFusedMatchesUnfused("forwarded key", [&tags](gr::Graph& flow, std::size_t ring) { return buildRateChain(flow, ring, tags, nullptr); }, 2UZ);

        const RunResult fused     = runOnce<singleThreaded>([&tags](gr::Graph& flow) { return buildRateChain(flow, 65536UZ, tags, nullptr); }, true, 256UZ);
        const auto      forwarded = std::ranges::find_if(fused.tags, [](const TagRecord& tag) { return tag.map.contains("signal_name"); });
        expect(forwarded != fused.tags.end()) << "an auto-forward key must reach the sink";
        if (forwarded != fused.tags.end()) {
            expect(forwarded->map.contains("sample_rate"));
            expect(!forwarded->map.contains("private_key")) << "a key outside autoForwardParameters must not be forwarded";
        }
    };

    "a tag-driven setting takes effect at the same sample index"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{1500UZ, {{"sample_rate", 96000.0f}}}};

        const RateArm unfused = runRateChain(tags, false, 0UZ);
        expect(eq(unfused.rateChangeAt, 1500UZ));
        expect(eq(unfused.observedAtTag, 96000.0f));

        for (const std::size_t chunk : kChunkSizes) {
            const RateArm fused = runRateChain(tags, true, chunk);
            expect(eq(fused.rateChangeAt, unfused.rateChangeAt)) << std::format("chunk={}", chunk);
            expect(eq(fused.observedAtTag, unfused.observedAtTag)) << std::format("chunk={}", chunk);
            expect(std::ranges::equal(fused.samples, unfused.samples)) << std::format("chunk={}", chunk);
        }
    };

    "a tag published from inside processOne lands at the same offset"_test = [] {
        const auto build = [](gr::Graph& flow, std::size_t ring) {
            const gr::EdgeParameters edge{.minBufferSize = ring};
            auto&                    source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
            auto&                    gain   = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
            auto&                    tagAt  = flow.emplaceBlock<TagAt>(gr::property_map{{"name", std::string("tagAt")}, {"tag_at", 777U}});
            auto&                    sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
            expect(flow.connect<"out", "in">(source, gain, edge).has_value());
            expect(flow.connect<"out", "in">(gain, tagAt, edge).has_value());
            expect(flow.connect<"out", "in">(tagAt, sink, edge).has_value());
            return std::addressof(sink);
        };
        expectFusedMatchesUnfused("published tag", build, 2UZ);

        const RunResult fused  = runOnce<singleThreaded>([&build](gr::Graph& flow) { return build(flow, 65536UZ); }, true, 256UZ);
        const auto      marker = std::ranges::find_if(fused.tags, [](const TagRecord& tag) { return tag.map.contains("marker"); });
        expect(marker != fused.tags.end()) << "a tag published from processOne must reach the sink";
        if (marker != fused.tags.end()) {
            expect(eq(marker->index, 777UZ));
        }
    };

    "end of stream stops every member exactly once"_test = [] {
        struct Arm {
            std::size_t                nSamples{};
            std::vector<TagRecord>     tags{};
            std::array<std::size_t, 5> nStops{};
        };

        const auto runArm = [](bool fusion, std::size_t chunk) {
            const gr::EdgeParameters edge{.minBufferSize = 65536UZ};

            gr::Graph flow;
            auto&     source   = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
            auto&     gain     = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
            auto&     quantize = flow.emplaceBlock<Quantize>(gr::property_map{{"name", std::string("quant")}});
            auto&     mean     = flow.emplaceBlock<RunningMean>(gr::property_map{{"name", std::string("mean")}});
            auto&     sink     = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
            expect(flow.connect<"out", "in">(source, gain, edge).has_value());
            expect(flow.connect<"out", "in">(gain, quantize, edge).has_value());
            expect(flow.connect<"out", "in">(quantize, mean, edge).has_value());
            expect(flow.connect<"out", "in">(mean, sink, edge).has_value());

            const gr::property_map                schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunk)}};
            gr::scheduler::Simple<singleThreaded> scheduler{schedulerSettings};
            expect(scheduler.exchange(std::move(flow)).has_value());
            expect(scheduler.runAndWait().has_value());

            return Arm{sink.samples.size(), sink.tags, std::array<std::size_t, 5>{source.nStopCalls, gain.nStopCalls, quantize.nStopCalls, mean.nStopCalls, sink.nStopCalls}};
        };

        const Arm unfused = runArm(false, 0UZ);
        expect(eq(unfused.nSamples, kSamples));
        for (const std::size_t nStops : unfused.nStops) {
            expect(eq(nStops, 1UZ)) << "unfused: stop() runs exactly once per block";
        }

        for (const std::size_t chunk : {256UZ, 4096UZ}) {
            const Arm fused = runArm(true, chunk);
            expect(eq(fused.nSamples, unfused.nSamples)) << std::format("chunk={}", chunk);
            expect(fused.tags == unfused.tags) << std::format("chunk={}: end-of-stream tag sequence differs", chunk);
            for (const std::size_t nStops : fused.nStops) {
                expect(eq(nStops, 1UZ)) << std::format("chunk={}: stop() must run exactly once per member", chunk);
            }
        }
    };

    "a run spans a processBulk member"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& bulk   = flow.emplaceBlock<BulkGain>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& gainC  = flow.emplaceBlock<Gain>();
            auto& sink   = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, bulk).has_value());
            expect(flow.connect<"out", "in">(bulk, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, gainC).has_value());
            expect(flow.connect<"out", "in">(gainC, sink).has_value());
        });
        expect(eq(runSizes.size(), 1UZ)) << "the processBulk member is a stage, not a boundary";
        if (!runSizes.empty()) {
            expect(eq(runSizes[0], 4UZ));
        }
    };

    "a bulk member at 1:1 joins two composed segments"_test = [] { expectFusedMatchesUnfused("bulk 1:1", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Quantize, BulkGain, Gain, Quantize>(flow, ring); }, 5UZ); };

    "a decimating bulk member is exact on the quantum"_test = [] {
        expectFusedMatchesUnfused("decimate 2:1", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Decimate2, Quantize>(flow, ring); }, 3UZ, kSamples / 2UZ);
        expectFusedMatchesUnfused("decimate 4:1", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Decimate4, Quantize>(flow, ring); }, 3UZ, kSamples / 4UZ);
    };

    "an interpolating bulk member is exact on the quantum"_test = [] { expectFusedMatchesUnfused("interpolate 1:3", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Interpolate3, Quantize>(flow, ring); }, 3UZ, 3UZ * kSamples); };

    "two bulk members in series give a quantum neither of them has"_test = [] {
        // 3:2 then 2:3 needs three input samples per run chunk, which is not L of either member
        expectFusedMatchesUnfused("3:2 then 2:3", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, ThreeToTwo, TwoToThree, Quantize>(flow, ring); }, 4UZ, 4095UZ);
    };

    "two stateful bulk members advance exactly once per sample"_test = [] { expectFusedMatchesUnfused("two accumulators", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, BulkAccumulate, BulkAccumulate, Quantize>(flow, ring); }, 4UZ); };

    "a mixed run is identical under the blocking policy"_test = [] { expectFusedMatchesUnfused<singleThreadedBlocking>("mixed, blocking", [](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Decimate2, RunningMean>(flow, ring); }, 3UZ, kSamples / 2UZ); };

    "a bulk member with forward tag propagation is admitted"_test = [] {
        const std::vector<TagRecord> tags{
            TagRecord{0UZ, {{"signal_name", std::string("start")}}},   //
            TagRecord{777UZ, {{"signal_name", std::string("middle")}}} //
        };
        expectFusedMatchesUnfused("forward tag policy", [&tags](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, BulkForwarder, Quantize>(flow, ring, tags); }, 3UZ);
    };

    "tags aligned to an L-block keep their exact offsets"_test = [] {
        std::vector<TagRecord> tags;
        for (std::size_t i = 0UZ; i < 64UZ; ++i) { // denser than one chunk, every one of them a legal chunk boundary
            tags.push_back(TagRecord{400UZ + 4UZ * i, {{"signal_name", std::format("dense-{}", i)}}});
        }
        expectFusedMatchesUnfused("aligned dense tags", [&tags](gr::Graph& flow, std::size_t ring) { return buildSeries<Gain, Decimate4, Quantize>(flow, ring, tags); }, 3UZ, kSamples / 4UZ);
    };

    "a tag interior to an L-block keeps its order and payload"_test = [] {
        // the framework defers a tag that cannot become a chunk boundary, and where it resurfaces is chunk-dependent,
        // so what is asserted here is order, payload, published-exactly-once and a lateness bounded by one fused chunk
        const std::vector<TagRecord> tags{
            TagRecord{0UZ, {{"signal_name", std::string("at-block-start")}}},   //
            TagRecord{400UZ, {{"signal_name", std::string("aligned")}}},        //
            TagRecord{405UZ, {{"signal_name", std::string("block-interior")}}}, //
            TagRecord{1024UZ, {{"signal_name", std::string("chunk-aligned")}}}, //
            TagRecord{2048UZ, {{"signal_name", std::string("late")}}},          //
        };
        const auto build = [&tags](gr::Graph& flow) { return buildSeries<Gain, Decimate4, Quantize>(flow, 65536UZ, tags); };

        const RunResult unfused = runOnce<singleThreaded>(build, false, 0UZ);
        expect(eq(unfused.tags.size(), tags.size()));

        for (const std::size_t chunk : kChunkSizes) {
            const RunResult   fused = runOnce<singleThreaded>(build, true, chunk);
            const std::string what  = std::format("interior tag chunk={}", chunk);

            expect(std::ranges::equal(fused.samples, unfused.samples)) << what;
            expect(eq(fused.tags.size(), tags.size())) << what << "every tag is published exactly once";
            expect(std::ranges::equal(fused.tags, unfused.tags, {}, &TagRecord::map, &TagRecord::map)) << what << "tag order or payload differs";
            expect(eq(fused.runChunks.size(), 1UZ)) << what;

            const std::size_t lateness = (fused.runChunks.empty() ? 0UZ : fused.runChunks[0] / 4UZ) + 1UZ;
            for (std::size_t i = 0UZ; i < std::min(fused.tags.size(), tags.size()); ++i) {
                const std::size_t ideal = tags[i].index / 4UZ;
                expect(ge(fused.tags[i].index, ideal)) << what << "a tag must never be published early";
                expect(le(fused.tags[i].index, ideal + lateness)) << what << "a deferred tag must be late by at most one fused chunk";
            }
        }
    };

    "a ratio changed while running latches the run to unfused execution"_test = [] {
        // where the ratio changes is chunk-dependent, so each arm is checked against the stream its own switch implies
        const RetuneArm unfused = runRetuneChain(false, 0UZ);
        expect(unfused.retuned) << "the reference arm must reach the retune point";
        expect(eq(unfused.nRuns, 0UZ));
        expect(std::ranges::equal(unfused.samples, expectedRetuneSamples(unfused.switchedAt))) << "unfused: 2:1 up to the switch, 4:1 after it";

        for (const std::size_t chunk : {256UZ, 1024UZ}) {
            const RetuneArm   fused = runRetuneChain(true, chunk);
            const std::string what  = std::format("chunk={}", chunk);
            expect(eq(fused.nRuns, 1UZ)) << what;
            expect(fused.retuned) << what;
            expect(ge(fused.switchedAt, 2048UZ)) << what;
            expect(lt(fused.switchedAt, kSamples)) << what << "the switch must leave data for the latched arm to carry";
            expect(fused.latched) << what << "a ratio the run did not plan for must latch it to unfused execution";
            expect(std::ranges::equal(fused.samples, expectedRetuneSamples(fused.switchedAt))) << what << "no sample may be lost or duplicated at the latch";
        }
    };

    // a decimator that ends its chunk on the retune point leaves part of the reserved output span unpublished;
    // the released remainder must not shift the stream, on either path
    "a bulk block that under-publishes across a live ratio change keeps its stream exact"_test = [] {
        const RetuneArm unfused = runRetuneChain(false, 0UZ, true);
        expect(unfused.underPublished) << "the arm must actually leave a reservation part-published";
        expect(unfused.retuned);
        expect(eq(unfused.switchedAt, 2048UZ)) << "the clamped chunk must land the switch exactly on the retune point";
        expect(std::ranges::equal(unfused.samples, expectedRetuneSamples(2048UZ))) << "unfused: 2:1 up to the switch, 4:1 after it";

        for (const std::size_t chunk : {256UZ, 1024UZ}) {
            const RetuneArm   fused = runRetuneChain(true, chunk, true);
            const std::string what  = std::format("chunk={}", chunk);
            expect(fused.underPublished) << what;
            expect(fused.retuned) << what;
            expect(eq(fused.switchedAt, 2048UZ)) << what;
            expect(std::ranges::equal(fused.samples, unfused.samples)) << what << "a part-published reservation must read the same fused";
        }
    };

    "end of stream stops every member of a mixed run exactly once"_test = [] {
        const auto runArm = [](bool fusion, std::size_t chunk) {
            const gr::EdgeParameters edge{.minBufferSize = 65536UZ};

            gr::Graph flow;
            auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
            auto&     gain   = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
            auto&     decim  = flow.emplaceBlock<Decimate4>(gr::property_map{{"name", std::string("decim")}});
            auto&     accum  = flow.emplaceBlock<BulkAccumulate>(gr::property_map{{"name", std::string("accum")}});
            auto&     mean   = flow.emplaceBlock<RunningMean>(gr::property_map{{"name", std::string("mean")}});
            auto&     sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
            expect(flow.connect<"out", "in">(source, gain, edge).has_value());
            expect(flow.connect<"out", "in">(gain, decim, edge).has_value());
            expect(flow.connect<"out", "in">(decim, accum, edge).has_value());
            expect(flow.connect<"out", "in">(accum, mean, edge).has_value());
            expect(flow.connect<"out", "in">(mean, sink, edge).has_value());

            const gr::property_map                schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunk)}};
            gr::scheduler::Simple<singleThreaded> scheduler{schedulerSettings};
            expect(scheduler.exchange(std::move(flow)).has_value());
            expect(scheduler.runAndWait().has_value());

            return std::pair{sink.samples.size(), std::array<std::size_t, 5>{source.nStopCalls, gain.nStopCalls, decim.nStopCalls, accum.nStopCalls, mean.nStopCalls}};
        };

        const auto [unfusedSamples, unfusedStops] = runArm(false, 0UZ);
        expect(eq(unfusedSamples, kSamples / 4UZ));
        for (const std::size_t nStops : unfusedStops) {
            expect(eq(nStops, 1UZ)) << "unfused: stop() runs exactly once per block";
        }

        for (const std::size_t chunk : {256UZ, 4096UZ}) {
            const auto [fusedSamples, fusedStops] = runArm(true, chunk);
            expect(eq(fusedSamples, unfusedSamples)) << std::format("chunk={}", chunk);
            for (const std::size_t nStops : fusedStops) {
                expect(eq(nStops, 1UZ)) << std::format("chunk={}: stop() must run exactly once per member", chunk);
            }
        }
    };

    "an interior edge is born small and its recorded size survives a re-plan"_test = [] {
        constexpr std::size_t    kWide = 1UZ << 19;
        const gr::EdgeParameters edge{.minBufferSize = kWide};

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        auto&     gain   = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("gain")}});
        auto&     quant  = flow.emplaceBlock<Quantize>(gr::property_map{{"name", std::string("quant")}});
        auto&     bulk   = flow.emplaceBlock<BulkGain>(gr::property_map{{"name", std::string("bulk")}});
        auto&     tail   = flow.emplaceBlock<Gain>(gr::property_map{{"name", std::string("tail")}});
        auto&     sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, gain, edge).has_value());
        expect(flow.connect<"out", "in">(gain, quant, edge).has_value());
        expect(flow.connect<"out", "in">(quant, bulk, edge).has_value());
        expect(flow.connect<"out", "in">(bulk, tail, edge).has_value());
        expect(flow.connect<"out", "in">(tail, sink, edge).has_value());

        gr::scheduler::Simple<singleThreaded> scheduler{gr::property_map{{"enable_fusion", true}}};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());
        expect(eq(sink.samples.size(), kSamples));

        std::vector<std::size_t> plannedSamples;
        for (const auto& job : scheduler.fusionPlan()) {
            for (const fusion::RunPlan& run : job) {
                expect(eq(run.members.size(), 4UZ));
                expect(eq(run.stages.size(), 3UZ)) << "a composed pair, the bulk member, and a composed tail";
                expect(eq(run.interiorEdges.size(), 2UZ));
                for (const fusion::InteriorEdge& interior : run.interiorEdges) {
                    expect(eq(interior.previousMinBufferSize, kWide)) << "the user's size must be recorded before it is overwritten";
                    expect(lt(interior.samples, kWide)) << "an interior edge must be born small";
                    plannedSamples.push_back(interior.samples);
                }
            }
        }
        expect(eq(plannedSamples.size(), 2UZ));

        std::size_t nInteriorLive = 0UZ;
        std::size_t nBoundaryLive = 0UZ;
        for (const gr::Edge& live : scheduler.graph().edges()) {
            const std::string_view producer = live.sourceBlock()->name();
            if (producer == "quant" || producer == "bulk") { // the live edge, not the plan's copy, must carry the ring
                expect(lt(live.bufferSize(), kWide)) << "an interior edge must not keep the user's size";
                expect(ge(live.bufferSize(), plannedSamples[nInteriorLive])) << "an interior edge must be at least the planned size";
                expect(eq(live.minBufferSize(), plannedSamples[nInteriorLive])) << "the planner writes the live edge";
                nInteriorLive++;
            } else {
                expect(ge(live.bufferSize(), kWide)) << "a boundary edge is not touched";
                nBoundaryLive++;
            }
        }
        expect(eq(nInteriorLive, 2UZ));
        expect(eq(nBoundaryLive, 3UZ));

        expect(scheduler.changeStateTo(lifecycle::State::INITIALISED).has_value());
        for (const auto& job : scheduler.fusionPlan()) {
            for (const fusion::RunPlan& run : job) {
                for (const fusion::InteriorEdge& interior : run.interiorEdges) {
                    expect(eq(interior.previousMinBufferSize, kWide)) << "a re-plan must record the restored size, not the shrunk one";
                }
            }
        }
    };

    "the classifier breaks a run at a stride"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source  = flow.emplaceBlock<Source>();
            auto& gainA   = flow.emplaceBlock<Gain>();
            auto& strided = flow.emplaceBlock<StrideGain>();
            auto& gainB   = flow.emplaceBlock<Gain>();
            auto& sink    = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, strided).has_value());
            expect(flow.connect<"out", "in">(strided, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, sink).has_value());
        });
        expect(runSizes.empty()) << "a strided member skips or overlaps its input and leaves no chain of two";
    };

    "the classifier breaks a run at a second stream input"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& other  = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& adder  = flow.emplaceBlock<TwoInputs>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& sink   = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, adder).has_value());
            expect(flow.connect<"out", "reference">(other, adder).has_value());
            expect(flow.connect<"out", "in">(adder, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, sink).has_value());
        });
        expect(runSizes.empty()) << "a two-input member leaves no chain of two";
    };

    "the classifier breaks a run at an async port"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& async  = flow.emplaceBlock<AsyncGain>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& sink   = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, async).has_value());
            expect(flow.connect<"out", "in">(async, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, sink).has_value());
        });
        expect(runSizes.empty()) << "an async member leaves no chain of two";
    };

    "a run spans a decimating processBulk member"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& decim  = flow.emplaceBlock<Decimate2>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& sink   = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, decim).has_value());
            expect(flow.connect<"out", "in">(decim, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, sink).has_value());
        });
        expect(eq(runSizes.size(), 1UZ)) << "a constant L:M member is a stage, not a boundary";
        if (!runSizes.empty()) {
            expect(eq(runSizes[0], 3UZ));
        }
    };

    "the classifier breaks a run at an interior fan-out"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& gainC  = flow.emplaceBlock<Gain>();
            auto& sinkA  = flow.emplaceBlock<Sink>();
            auto& sinkB  = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, gainC).has_value());
            expect(flow.connect<"out", "in">(gainB, sinkB).has_value());
            expect(flow.connect<"out", "in">(gainC, sinkA).has_value());
        });
        expect(eq(runSizes.size(), 1UZ)) << "the fan-out ends the run at the producer";
        if (!runSizes.empty()) {
            expect(eq(runSizes[0], 2UZ)) << "the block downstream of the fan-out is left alone";
        }
    };

    "a fanning-out block is admitted as the last member"_test = [] {
        const std::vector<std::size_t> runSizes = planOnly([](gr::Graph& flow) {
            auto& source = flow.emplaceBlock<Source>();
            auto& gainA  = flow.emplaceBlock<Gain>();
            auto& gainB  = flow.emplaceBlock<Gain>();
            auto& sinkA  = flow.emplaceBlock<Sink>();
            auto& sinkB  = flow.emplaceBlock<Sink>();
            expect(flow.connect<"out", "in">(source, gainA).has_value());
            expect(flow.connect<"out", "in">(gainA, gainB).has_value());
            expect(flow.connect<"out", "in">(gainB, sinkA).has_value());
            expect(flow.connect<"out", "in">(gainB, sinkB).has_value());
        });
        expect(eq(runSizes.size(), 1UZ));
        if (!runSizes.empty()) {
            expect(eq(runSizes[0], 2UZ));
        }
    };

    "fusion is off unless it is asked for"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Source>();
        auto&     gainA  = flow.emplaceBlock<Gain>();
        auto&     gainB  = flow.emplaceBlock<Gain>();
        auto&     sink   = flow.emplaceBlock<Sink>();
        source.nTotal    = 1024UZ;
        expect(flow.connect<"out", "in">(source, gainA).has_value());
        expect(flow.connect<"out", "in">(gainA, gainB).has_value());
        expect(flow.connect<"out", "in">(gainB, sink).has_value());

        gr::scheduler::Simple<singleThreaded> scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());
        expect(scheduler.fusionPlan().empty()) << "enable_fusion defaults to false";
        expect(eq(sink.samples.size(), 1024UZ));
    };
};

int main() { /* not needed by the UT framework */ }
