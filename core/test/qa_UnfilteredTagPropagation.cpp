#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief A block passing `kUnfilteredTagPropagationAdmissible` forwards every tag key at the tag's own offset.
 *
 * Such a block forwards every key whether it declares `UnfilteredTagPropagation` or no policy at all. A block declaring
 * `FilteredTagPropagation`, and every block the predicate refuses, keeps only the auto-forward keys. The tests check
 * that:
 * - every key survives
 * - a key the block declares as a setting carries the block's own current value
 * - a key named after a setting `Block<>` declares for every block keeps the upstream value
 * - a tag inside a chunk keeps its offset where an input minimum forbids a boundary at it
 *
 * They also check the multi-input dedup, the merge rule, and the key-filtered forwarding of a block the predicate
 * refuses. The compile-time guards are checked twice. This file checks the predicate, and six translation units under
 * `compile_fail/` must fail to build.
 *
 * The blocks are defined here: gnuradio4-core carries no standard block library, so a core test may not depend on one.
 */

namespace qa_unfiltered_tag_propagation {

using namespace gr;

inline constexpr std::size_t kSamples = 2048UZ;

/// an input minimum above one forbids a chunk boundary at a tag closer than that to the chunk's start
inline constexpr std::size_t kInputMinimum = 4UZ;

inline constexpr std::string_view kReservedKey = "trigger_name"; ///< reserved: survives the default key filter
inline constexpr std::string_view kCustomKey   = "record_id";    ///< neither reserved nor any block's setting
inline constexpr std::string_view kOwnedKey    = "gain";         ///< not reserved, but a declared setting of one block

inline constexpr float kUpstreamGain = 10.f;
inline constexpr float kUpstreamRate = 96000.f;
inline constexpr float kGainCeiling  = 2.f;
inline constexpr float kRateCeiling  = 50000.f;

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

    std::size_t _emitted    = 0UZ;
    std::size_t _nextTag    = 0UZ;
    std::size_t _chunkIndex = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        static constexpr std::array<std::size_t, 5> kPrimes{7UZ, 31UZ, 61UZ, 127UZ, 251UZ}; // irregular publish lengths

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
        for (; _emitted + n == nTotal && _nextTag < tagsToEmit.size(); ++_nextTag) { // a tag at or past the last sample leaves at the end index
            outSpan.publishTag(tagsToEmit[_nextTag].map, n);
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

/// records the tags its input ring holds at or past its last sample, in its epilogue
struct EndSink : Block<EndSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(EndSink, in);

    std::size_t            nSamples = 0UZ;
    std::vector<TagRecord> endTags;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        nSamples += inSpan.size();
        std::ignore = inSpan.consume(inSpan.size());
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan) {
        const std::size_t end = inSpan.streamIndex + inSpan.size();
        for (const Tag& tag : in.tagReader().get()) {
            if (tag.index >= end) {
                endTags.push_back(TagRecord{tag.index, tag.map});
            }
        }
        nSamples += inSpan.size();
        return work::Status::OK;
    }
};

/// declares no policy and passes the predicate, and every key of every tag leaves this block
struct Relay : Block<Relay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Relay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct BulkRelay : Block<BulkRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(BulkRelay, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// whatever a tag carries, only the auto-forward keys leave this block
struct FilteredRelay : Block<FilteredRelay, FilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilteredRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct FilteredBulkRelay : Block<FilteredBulkRelay, FilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilteredBulkRelay, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// keeps every second sample and declares no tag-propagation policy
struct HalvingDecimator : Block<HalvingDecimator, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(HalvingDecimator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / 2UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[2UZ * i];
        }
        std::ignore = inSpan.consume(2UZ * n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// a synchronous input copied onto an asynchronous output, with no tag-propagation policy
struct AsyncOutputRelay : Block<AsyncOutputRelay> {
    PortIn<float>         in;
    PortOut<float, Async> out;

    GR_MAKE_REFLECTABLE(AsyncOutputRelay, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// its own forwardTags() republishes every tag at relIndex <= 0 whole. It relies on the input span retiring only those.
struct FirstTagForwarder : Block<FirstTagForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FirstTagForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t /*processedIn*/) {
        gr::for_each_reader_span(
            [&outputSpans](auto& inSpan) {
                for (const auto& [relIndex, tagMapRef] : inSpan.tags(1UZ)) {
                    gr::for_each_writer_span([&tagMapRef](auto& outSpan) { outSpan.publishTag(tagMapRef.get(), 0UZ); }, outputSpans);
                }
            },
            inputSpans);
    }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct UnfilteredRelay : Block<UnfilteredRelay, UnfilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(UnfilteredRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct UnfilteredBulkRelay : Block<UnfilteredBulkRelay, UnfilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(UnfilteredBulkRelay, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// an identity block whose trailing samples, and the tags among them, arrive through processEpilogue
struct UnfilteredTailRelay : Block<UnfilteredTailRelay, UnfilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(UnfilteredTailRelay, in, out);

    std::size_t epilogueRuns = 0UZ;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        ++epilogueRuns;
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// an override constrained to the span tuples the work path passes, which a probe on empty tuples does not see
struct ConstrainedForwarder : Block<ConstrainedForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(ConstrainedForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    requires(std::tuple_size_v<TInputSpans> > 0UZ)
    void forwardTags(TInputSpans&, TOutputSpans&, std::size_t) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// caps both of its settings, so the value it forwards for a key it owns differs from the value the tag carried
struct UnfilteredCap : Block<UnfilteredCap, UnfilteredTagPropagation> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate     = 1000.f;
    float gain            = 1.f;
    float max_gain        = kGainCeiling;
    float max_sample_rate = kRateCeiling;

    GR_MAKE_REFLECTABLE(UnfilteredCap, in, out, sample_rate, gain, max_gain, max_sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) {
        gain        = std::min(gain, max_gain);
        sample_rate = std::min(sample_rate, max_sample_rate);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct Decimating : Block<Decimating, UnfilteredTagPropagation, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Decimating, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

struct SettableRatio : Block<SettableRatio, Resampling<1U, 1U, false>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(SettableRatio, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

struct Strided : Block<Strided, Stride<2U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Strided, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct AsyncInput : Block<AsyncInput> {
    PortIn<float, Async> in;
    PortOut<float>       out;

    GR_MAKE_REFLECTABLE(AsyncInput, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

struct AsyncOutput : Block<AsyncOutput> {
    PortIn<float>         in;
    PortOut<float, Async> out;

    GR_MAKE_REFLECTABLE(AsyncOutput, in, out);

    work::Status processBulk(InputSpanLike auto&, OutputSpanLike auto&) { return work::Status::OK; }
};

/// `Optional` means "may be left unconnected", not "asynchronous", and must not be refused
struct OptionalOutput : Block<OptionalOutput> {
    PortIn<float>            in;
    PortOut<float, Optional> out;

    GR_MAKE_REFLECTABLE(OptionalOutput, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<typename TPolicy>
struct PolicyRelay : Block<PolicyRelay<TPolicy>, TPolicy> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(PolicyRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct OwnForwarder : Block<OwnForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(OwnForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans&, TOutputSpans&, std::size_t) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<bool kRetireOnlyFirstTag>
using FloatInputSpan = std::remove_cvref_t<decltype(std::declval<PortIn<float>&>().template get<SpanReleasePolicy::ProcessAll, kRetireOnlyFirstTag>(0UZ))>;

/// an override that accepts the input span of one tag window and refuses the other
template<bool kRetireOnlyFirstTag>
struct WindowConstrainedForwarder : Block<WindowConstrainedForwarder<kRetireOnlyFirstTag>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(WindowConstrainedForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    requires std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0UZ, TInputSpans>>, FloatInputSpan<kRetireOnlyFirstTag>>
    void forwardTags(TInputSpans&, TOutputSpans&, std::size_t) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// the opt-out beside Resampling<>, which the predicate refuses already
struct FilteredDecimator : Block<FilteredDecimator, FilteredTagPropagation, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilteredDecimator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / 2UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[2UZ * i];
        }
        std::ignore = inSpan.consume(2UZ * n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// the opt-out beside `Stride<>` or another tag-propagation policy, which the predicate refuses already
template<typename TArgument>
struct FilteredPolicyRelay : Block<FilteredPolicyRelay<TArgument>, FilteredTagPropagation, TArgument> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilteredPolicyRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

template<typename TArgument>
constexpr bool kOptOutChangesNothing = !FilteredPolicyRelay<TArgument>::forwardsEveryKey() && FilteredPolicyRelay<TArgument>::hasWholeChunkTagWindow() == PolicyRelay<TArgument>::hasWholeChunkTagWindow();

/// passes its input through, and takes the previous output on a second input inside a feedback merge
struct FeedbackRelay : Block<FeedbackRelay> {
    PortIn<float>  in;
    PortIn<float>  previous;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FeedbackRelay, in, previous, out);

    [[nodiscard]] constexpr float processOne(float value, float /*previousOutput*/) const noexcept { return value; }
};

using MergedPlain       = MergeByIndex<Relay, 0UZ, Relay, 0UZ>;
using MergedFiltered    = MergeByIndex<FilteredRelay, 0UZ, Relay, 0UZ>;
using MergedSilent      = MergeByIndex<Relay, 0UZ, PolicyRelay<NoTagPropagation>, 0UZ>;
using SplitPlain        = SplitMergeCombine<Relay, Relay>;
using SplitFiltered     = SplitMergeCombine<Relay, FilteredRelay>;
using SignedFiltered    = SplitMergeCombine<OutputSigns<1.0f, -1.0f>, Relay, FilteredRelay>;
using FeedbackPlain     = FeedbackMergeByIndex<FeedbackRelay, 0UZ, Relay, 0UZ, 1UZ, void>;
using FeedbackFiltered  = FeedbackMergeByIndex<FeedbackRelay, 0UZ, FilteredRelay, 0UZ, 1UZ, void>;
using FeedbackMonitored = FeedbackMergeByIndex<FeedbackRelay, 0UZ, Relay, 0UZ, 1UZ, FilteredRelay>;
using TapFiltered       = FeedbackMergeWithTapByIndex<FeedbackRelay, 0UZ, FilteredRelay, 0UZ, 1UZ, void>;

[[nodiscard]] const pmt::Value* valueOf(const property_map& map, std::string_view key) {
    const auto it = map.find(key);
    return it == map.end() ? nullptr : std::addressof(it->second);
}

[[nodiscard]] std::vector<TagRecord> carrying(const std::vector<TagRecord>& tags, std::string_view key) {
    return tags | std::views::filter([key](const TagRecord& record) { return record.map.contains(key); }) | std::ranges::to<std::vector>();
}

[[nodiscard]] std::vector<std::size_t> indicesCarrying(const std::vector<TagRecord>& tags, std::string_view key) { return carrying(tags, key) | std::views::transform(&TagRecord::index) | std::ranges::to<std::vector>(); }

[[nodiscard]] property_map protocolTag(std::string_view recordId) {
    property_map map;
    gr::tag::put(map, kReservedKey, std::string("trigger"));
    gr::tag::put(map, kCustomKey, std::string(recordId));
    return map;
}

struct RunResult {
    std::vector<float>     samples;
    std::vector<TagRecord> tags;
};

template<typename TBuild>
[[nodiscard]] RunResult runOnce(TBuild&& build) {
    using namespace boost::ut;

    gr::Graph flow;
    Sink*     sink = build(flow);

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return RunResult{sink->samples, sink->tags};
}

// source -> TMiddle... -> sink, emplaced and connected in that order
template<typename... TMiddle>
[[nodiscard]] Sink* buildSeries(gr::Graph& flow, const std::vector<TagRecord>& tags, const std::array<gr::property_map, sizeof...(TMiddle)>& middleSettings = {}) {
    using namespace boost::ut;

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;

    // a braced initializer fixes the emplacement order
    std::tuple<TMiddle&...> middle = [&]<std::size_t... I>(std::index_sequence<I...>) { return std::tuple<TMiddle&...>{flow.emplaceBlock<TMiddle>(middleSettings[I])...}; }(std::index_sequence_for<TMiddle...>{});
    auto&                   sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});

    const auto link = [&flow](auto& from, auto& to) { expect(flow.connect<"out", "in">(from, to).has_value()); };
    link(source, std::get<0UZ>(middle));
    [&]<std::size_t... I>(std::index_sequence<I...>) { (link(std::get<I>(middle), std::get<I + 1UZ>(middle)), ...); }(std::make_index_sequence<sizeof...(TMiddle) - 1UZ>{});
    link(std::get<sizeof...(TMiddle) - 1UZ>(middle), sink);
    return std::addressof(sink);
}

// source -> one middle -> sink, with the input window that decides whether a chunk can break at a tag
template<typename TMiddle>
[[nodiscard]] Sink* buildWindowed(gr::Graph& flow, const std::vector<TagRecord>& tags, std::size_t nTotal, std::size_t minSamples, std::size_t maxSamples) {
    using namespace boost::ut;

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;
    source.nTotal     = nTotal;

    auto& middle          = flow.emplaceBlock<TMiddle>(gr::property_map{{"name", std::string("mid")}});
    middle.in.min_samples = minSamples;
    middle.in.max_samples = maxSamples;

    auto& sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());
    return std::addressof(sink);
}

// three blocks in series, with the input window set on the first of them
[[nodiscard]] Sink* buildChainWindowed(gr::Graph& flow, const std::vector<TagRecord>& tags, std::size_t minSamples) {
    using namespace boost::ut;

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;

    auto& first          = flow.emplaceBlock<UnfilteredRelay>(gr::property_map{{"name", std::string("m0")}});
    auto& second         = flow.emplaceBlock<UnfilteredRelay>(gr::property_map{{"name", std::string("m1")}});
    auto& third          = flow.emplaceBlock<UnfilteredRelay>(gr::property_map{{"name", std::string("m2")}});
    first.in.min_samples = minSamples;

    auto&      sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    const auto link = [&flow](auto& from, auto& to) { expect(flow.connect<"out", "in">(from, to).has_value()); };
    link(source, first);
    link(first, second);
    link(second, third);
    link(third, sink);
    return std::addressof(sink);
}

/// what an end sink recorded
struct EndResult {
    std::size_t            nSamples{};
    std::vector<TagRecord> endTags;
};

/// source -> TMiddle -> end sink, with `tags` published by the source
template<typename TMiddle>
[[nodiscard]] EndResult runToEnd(const std::vector<TagRecord>& tags) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source  = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;
    auto& middle      = flow.emplaceBlock<TMiddle>(gr::property_map{{"name", std::string("mid")}});
    auto& sink        = flow.emplaceBlock<EndSink>(gr::property_map{{"name", std::string("snk")}});
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return EndResult{sink.nSamples, sink.endTags};
}

// a composition of blocks that all forward every key, against the same composition with one member filtering
template<typename TPlain, typename TFiltered>
void expectComposedKeyRule(std::string_view composition) {
    using namespace boost::ut;

    const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
    const RunResult              plain    = runOnce([&tags](gr::Graph& flow) { return buildSeries<TPlain>(flow, tags); });
    const RunResult              filtered = runOnce([&tags](gr::Graph& flow) { return buildSeries<TFiltered>(flow, tags); });

    const std::vector<std::size_t> offsets{tags[0].index, tags[1].index};
    expect(indicesCarrying(plain.tags, kCustomKey) == offsets) << std::format("{}: members forwarding every key compose into a block forwarding every key", composition);
    expect(indicesCarrying(filtered.tags, kReservedKey) == offsets) << std::format("{}: the reserved key crosses the composed block", composition);
    expect(carrying(filtered.tags, kCustomKey).empty()) << std::format("{}: a member declaring FilteredTagPropagation filters the composed block", composition);
}

void expectTagIndices(std::string_view scenario, const std::vector<TagRecord>& tags, const std::vector<std::size_t>& expected) {
    using namespace boost::ut;

    const std::vector<std::size_t> actual = indicesCarrying(tags, kCustomKey);
    expect(eq(actual.size(), expected.size())) << std::format("{}: {} tags carrying {}, expected {}", scenario, actual.size(), kCustomKey, expected.size());
    for (std::size_t i = 0UZ; i < std::min(actual.size(), expected.size()); ++i) {
        expect(eq(actual[i], expected[i])) << std::format("{}: tag {} arrived at output index {}, expected {}", scenario, i, actual[i], expected[i]);
    }
}

} // namespace qa_unfiltered_tag_propagation

const boost::ut::suite<"unfiltered tag propagation"> _unfilteredTagPropagation = [] {
    using namespace boost::ut;
    using namespace qa_unfiltered_tag_propagation;

    static_assert(std::ranges::none_of(gr::tag::kDefaultTags, [](std::string_view reserved) { return std::ranges::contains(gr::block::kFrameworkOwnedSettings, reserved); }), //
        "a reserved tag key sharing a name with a framework-owned setting would change forwarding under every policy");

    static_assert(gr::block::kUnfilteredTagPropagationAdmissible<Relay>);
    static_assert(gr::block::kUnfilteredTagPropagationAdmissible<UnfilteredBulkRelay>);
    static_assert(gr::block::kUnfilteredTagPropagationAdmissible<OptionalOutput>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<Decimating>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<SettableRatio>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<Strided>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<AsyncInput>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<AsyncOutput>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<PolicyRelay<gr::NoTagPropagation>>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<PolicyRelay<gr::ForwardTagPropagation>>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<PolicyRelay<gr::BackwardTagPropagation>>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<PolicyRelay<gr::MergeTagPropagation>>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<OwnForwarder>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<ConstrainedForwarder>); // the probe passes the work path's own span tuples
    static_assert(gr::block::kUnfilteredTagPropagationAdmissible<UnfilteredTailRelay>);
    static_assert(gr::block::kUnfilteredTagPropagationAdmissible<BulkRelay>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<FilteredRelay>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<HalvingDecimator>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<AsyncOutputRelay>);
    static_assert(!gr::block::kUnfilteredTagPropagationAdmissible<FirstTagForwarder>);
    static_assert(Relay::forwardsEveryKey() && UnfilteredRelay::forwardsEveryKey());
    static_assert(!FilteredRelay::forwardsEveryKey() && !HalvingDecimator::forwardsEveryKey() && !Strided::forwardsEveryKey());
    static_assert(!AsyncOutputRelay::forwardsEveryKey() && !FirstTagForwarder::forwardsEveryKey());
    static_assert(WindowConstrainedForwarder<true>::hasForwardTagsOverride() && WindowConstrainedForwarder<false>::hasForwardTagsOverride(), "the probe tries the spans of both tag windows");
    static_assert(!FilteredDecimator::forwardsEveryKey() && FilteredDecimator::hasWholeChunkTagWindow() == HalvingDecimator::hasWholeChunkTagWindow(), "the opt-out beside Resampling<> changes nothing");
    static_assert(kOptOutChangesNothing<Stride<2U, true>> && kOptOutChangesNothing<ForwardTagPropagation> && kOptOutChangesNothing<BackwardTagPropagation> && kOptOutChangesNothing<MergeTagPropagation>);
    static_assert(MergedPlain::forwardsEveryKey() && !MergedFiltered::forwardsEveryKey() && !MergedSilent::forwardsEveryKey());
    static_assert(SplitPlain::forwardsEveryKey() && !SplitFiltered::forwardsEveryKey() && !SignedFiltered::forwardsEveryKey());
    static_assert(FeedbackPlain::forwardsEveryKey() && !FeedbackFiltered::forwardsEveryKey() && !FeedbackMonitored::forwardsEveryKey() && !TapFiltered::forwardsEveryKey());

    "an undeclared admissible block forwards every key with its value and offset intact"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, BulkRelay>(flow, tags); });

        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(eq(carried.size(), tags.size())) << "every custom key crosses two blocks that declare no policy";
        for (std::size_t i = 0UZ; i < std::min(carried.size(), tags.size()); ++i) {
            expect(eq(carried[i].index, tags[i].index)) << std::format("tag {} keeps its offset", i);
            const pmt::Value* value = valueOf(carried[i].map, kCustomKey);
            expect(value != nullptr);
            if (value != nullptr) {
                expect(eq(value->value_or(std::string_view{}), std::string_view(std::format("r{}", i)))) << "the value is forwarded verbatim";
            }
        }
    };

    "an undeclared admissible block keeps an interior tag at the offset it arrived at"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ; // closer to the chunk start than the input minimum, so no boundary can fall on it
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<BulkRelay>(flow, tags, kTotal, kInputMinimum, 7UZ); });
        expectTagIndices("undeclared block, input minimum above one", result.tags, {kAt});
    };

    "a block declaring FilteredTagPropagation defers an interior tag as a key-filtered block does"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const RunResult              result   = runOnce([&tags](gr::Graph& flow) { return buildWindowed<FilteredBulkRelay>(flow, tags, kTotal, kInputMinimum, 7UZ); });
        const std::vector<TagRecord> reserved = carrying(result.tags, kReservedKey);
        expect(eq(reserved.size(), 1UZ)) << "the reserved key leaves the block once";
        if (!reserved.empty()) {
            expect(gt(reserved.front().index, kAt)) << "the tag moves to the first sample of a later chunk";
        }
        expect(carrying(result.tags, kCustomKey).empty()) << "the custom key stays behind";
    };

    "an undeclared block declaring Resampling<> keeps the auto-forward keys alone"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<HalvingDecimator>(flow, tags); });

        expect(indicesCarrying(result.tags, kReservedKey) == std::vector<std::size_t>{tags[0].index / 2UZ, tags[1].index / 2UZ}) << "the reserved key lands on the decimated offset";
        expect(carrying(result.tags, kCustomKey).empty()) << "a rate-changing block keeps the key filter";
    };

    "an undeclared block declaring Stride<> keeps the auto-forward keys alone"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Strided>(flow, tags); });

        expect(eq(carrying(result.tags, kReservedKey).size(), 1UZ)) << "the reserved key crosses the block";
        expect(carrying(result.tags, kCustomKey).empty()) << "a strided block keeps the key filter";
    };

    "an undeclared block with an asynchronous output keeps the auto-forward keys alone"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<AsyncOutputRelay>(flow, tags); });

        expect(eq(carrying(result.tags, kReservedKey).size(), tags.size())) << "the reserved key crosses the block";
        expect(carrying(result.tags, kCustomKey).empty()) << "an asynchronous port keeps the key filter";
    };

    "a block supplying forwardTags() keeps the retired tag window its override reads"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const RunResult              result  = runOnce([&tags](gr::Graph& flow) { return buildWindowed<FirstTagForwarder>(flow, tags, kTotal, kInputMinimum, 7UZ); });
        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(eq(carried.size(), 1UZ)) << "the interior tag reaches the override exactly once";
        if (!carried.empty()) {
            expect(gt(carried.front().index, kAt)) << "the override publishes the deferred tag at a later chunk's first sample";
        }
    };

    "a block declaring FilteredTagPropagation beside Resampling<> forwards as the undeclared block does"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              undeclared = runOnce([&tags](gr::Graph& flow) { return buildSeries<HalvingDecimator>(flow, tags); });
        const RunResult              declared   = runOnce([&tags](gr::Graph& flow) { return buildSeries<FilteredDecimator>(flow, tags); });

        expect(eq(carrying(declared.tags, kReservedKey).size(), tags.size())) << "the reserved key crosses the block";
        expect(declared.tags == undeclared.tags) << "the opt-out changes no forwarded tag";
    };

    "a block declaring FilteredTagPropagation beside Stride<> or another policy forwards as that block does alone"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const auto                   compare = [&tags]<typename TArgument>(std::string_view name) {
            const RunResult alone    = runOnce([&tags](gr::Graph& flow) { return buildSeries<PolicyRelay<TArgument>>(flow, tags); });
            const RunResult declared = runOnce([&tags](gr::Graph& flow) { return buildSeries<FilteredPolicyRelay<TArgument>>(flow, tags); });
            expect(!carrying(declared.tags, kReservedKey).empty()) << std::format("{}: the reserved key crosses the block", name);
            expect(declared.tags == alone.tags) << std::format("{}: the opt-out changes no forwarded tag", name);
        };
        compare.template operator()<Stride<2U, true>>("Stride<>");
        compare.template operator()<ForwardTagPropagation>("ForwardTagPropagation");
        compare.template operator()<BackwardTagPropagation>("BackwardTagPropagation");
        compare.template operator()<MergeTagPropagation>("MergeTagPropagation");
    };

    "a compile-time merge keeps the auto-forward keys alone where either half does"_test = [] { expectComposedKeyRule<MergedPlain, MergedFiltered>("MergeByIndex"); };

    "a split-merge-combine keeps the auto-forward keys alone where any path does"_test = [] { expectComposedKeyRule<SplitPlain, SplitFiltered>("SplitMergeCombine"); };

    "a feedback merge keeps the auto-forward keys alone where its feedback block does"_test = [] { expectComposedKeyRule<FeedbackPlain, FeedbackFiltered>("FeedbackMergeByIndex"); };

    "a feedback merge keeps the auto-forward keys alone where its monitor does"_test = [] { expectComposedKeyRule<FeedbackPlain, FeedbackMonitored>("FeedbackMergeByIndex with a monitor"); };

    "a block declaring FilteredTagPropagation drops a custom key and keeps the reserved one"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<FilteredRelay, Relay>(flow, tags); });

        expect(indicesCarrying(result.tags, kReservedKey) == std::vector<std::size_t>{tags[0].index, tags[1].index}) << "a reserved key crosses the filtered block at its offset";
        expect(carrying(result.tags, kCustomKey).empty()) << "a key outside the auto-forward set must not survive the key filter";
    };

    "the default policy carries the transmit burst keys through blocks that declare nothing about them"_test = [] {
        constexpr std::size_t   kBurstFirst  = 100UZ;
        constexpr std::size_t   kBurstLast   = 899UZ;
        constexpr std::uint64_t kBurstTimeNs = 1'720'000'000'123'456'789ULL;

        property_map burstStart;
        gr::tag::put(burstStart, "tx_sob", true);
        gr::tag::put(burstStart, "tx_time", kBurstTimeNs);
        property_map burstEnd;
        gr::tag::put(burstEnd, "tx_eob", true);
        const std::vector<TagRecord> tags{TagRecord{kBurstFirst, burstStart}, TagRecord{kBurstLast, burstEnd}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); });

        expect(indicesCarrying(result.tags, "tx_sob") == std::vector{kBurstFirst}) << "tx_sob stays on the burst's first sample";
        expect(indicesCarrying(result.tags, "tx_time") == std::vector{kBurstFirst}) << "tx_time stays on the burst's first sample";
        expect(indicesCarrying(result.tags, "tx_eob") == std::vector{kBurstLast}) << "tx_eob stays on the burst's last sample";

        for (const TagRecord& record : result.tags) {
            if (const pmt::Value* start = valueOf(record.map, "tx_sob"); start != nullptr) {
                expect(start->holds<bool>() && *start->get_if<bool>()) << "tx_sob keeps its type and value";
            }
            if (const pmt::Value* time = valueOf(record.map, "tx_time"); time != nullptr) {
                expect(time->holds<std::uint64_t>() && *time->get_if<std::uint64_t>() == kBurstTimeNs) << "tx_time keeps its type and value";
            }
            if (const pmt::Value* end = valueOf(record.map, "tx_eob"); end != nullptr) {
                expect(end->holds<bool>() && *end->get_if<bool>()) << "tx_eob keeps its type and value";
            }
        }
    };

    "a custom key crosses a chain of unfiltered blocks with its value and offset intact"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); });

        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(eq(carried.size(), tags.size())) << "every custom key survives two intermediate blocks";
        for (std::size_t i = 0UZ; i < std::min(carried.size(), tags.size()); ++i) {
            expect(eq(carried[i].index, tags[i].index)) << std::format("tag {} keeps its offset", i);
            expect(carried[i].map.contains(kReservedKey)) << "the custom key travels in the same tag as the reserved one";
            const pmt::Value* value = valueOf(carried[i].map, kCustomKey);
            expect(value != nullptr);
            if (value != nullptr) {
                expect(eq(value->value_or(std::string_view{}), std::string_view(std::format("r{}", i)))) << "the value is forwarded verbatim";
            }
        }
    };

    "a key the block declares as a setting is substituted with the block's own current value"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);
        gr::tag::put(map, "sample_rate", kUpstreamRate);

        const std::vector<TagRecord> tags{TagRecord{0UZ, map}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredCap, UnfilteredRelay>(flow, tags); });

        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(eq(carried.size(), 1UZ));
        if (carried.empty()) {
            return;
        }
        const property_map& arrived = carried.front().map;

        const pmt::Value* owned = valueOf(arrived, kOwnedKey);
        expect(owned != nullptr) << "an owned non-reserved key survives only under the policy, and must survive";
        if (owned != nullptr) {
            expect(eq(owned->value_or<float>(0.f), kGainCeiling)) << "the block's capped value, not the upstream one";
        }

        const pmt::Value* rate = valueOf(arrived, "sample_rate");
        expect(rate != nullptr);
        if (rate != nullptr) {
            expect(eq(rate->value_or<float>(0.f), kRateCeiling)) << "an owned reserved key is substituted exactly as before";
        }

        const pmt::Value* custom = valueOf(arrived, kCustomKey);
        expect(custom != nullptr);
        if (custom != nullptr) {
            expect(eq(custom->value_or(std::string_view{}), std::string_view("r0"))) << "a key no block owns passes through unchanged";
        }
    };

    "a key named after a setting Block<> declares for every block is not substituted"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, "name", 7.f); // the upstream protocol's own use of the name, not a block name

        const std::vector<TagRecord> tags{TagRecord{0UZ, map}};
        const RunResult              unfiltered = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); });

        const std::vector<TagRecord> carried = carrying(unfiltered.tags, kCustomKey);
        expect(eq(carried.size(), 1UZ));
        if (!carried.empty()) {
            const pmt::Value* name = valueOf(carried.front().map, "name");
            expect(name != nullptr) << "the key still crosses the block";
            if (name != nullptr) {
                expect(name->get_if<float>() != nullptr) << "the upstream value, not the block's own name";
                expect(eq(name->value_or<float>(0.f), 7.f));
            }
        }

        const RunResult filtered = runOnce([&tags](gr::Graph& flow) { return buildSeries<FilteredRelay, FilteredRelay>(flow, tags); });
        expect(carrying(filtered.tags, "name").empty()) << "under the key filter the key never reaches the substitution at all";
    };

    "a framework-owned key a filtered block auto-forwards keeps the value it arrived with"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, "name", 7.f); // the upstream protocol's own use of the name, not a block name

        const std::vector<TagRecord> tags  = {TagRecord{0UZ, map}};
        const auto                   build = [&tags](gr::Graph& flow) {
            auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
            source.tagsToEmit = tags;
            auto& relay       = flow.emplaceBlock<FilteredRelay>(gr::property_map{{"name", std::string("mid")}});
            relay.settings().addAutoForwardParameters({"name"}); // the set is fixed once the block runs, so it is widened here
            auto& sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
            expect(flow.connect<"out", "in">(source, relay).has_value());
            expect(flow.connect<"out", "in">(relay, sink).has_value());
            return std::addressof(sink);
        };
        const RunResult result = runOnce(build);

        const std::vector<TagRecord> carried = carrying(result.tags, "name");
        expect(eq(carried.size(), 1UZ)) << "an auto-forwarded key survives the key filter";
        if (!carried.empty()) {
            const pmt::Value* name = valueOf(carried.front().map, "name");
            expect(name != nullptr);
            if (name != nullptr) {
                expect(name->get_if<float>() != nullptr) << "the upstream value, not the block's own name";
                expect(eq(name->value_or<float>(0.f), 7.f));
            }
        }
    };

    "an owned key is substituted in the order the blocks run"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);

        const std::vector<TagRecord>            tags{TagRecord{0UZ, map}};
        const std::array<gr::property_map, 2UZ> caps{gr::property_map{{"max_gain", kGainCeiling}}, gr::property_map{{"max_gain", 1.f}}};

        const RunResult result  = runOnce([&tags, &caps](gr::Graph& flow) { return buildSeries<UnfilteredCap, UnfilteredCap>(flow, tags, caps); });
        const auto      carried = carrying(result.tags, kOwnedKey);
        expect(eq(carried.size(), 1UZ));
        if (!carried.empty()) {
            expect(eq(valueOf(carried.front().map, kOwnedKey)->value_or<float>(0.f), 1.f)) << "the last block's value reaches the sink";
        }
    };

    "an unfiltered processBulk block carries custom keys through its own work path"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredBulkRelay, UnfilteredRelay>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), tags.size())) << "the custom key crosses a processBulk block between two processOne blocks";
    };

    "an interior tag keeps its offset where an input minimum forbids a chunk boundary at it"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ; // closer to the chunk start than the input minimum, so no boundary can fall on it
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const auto windowed = [&tags](std::size_t minSamples) { return [&tags, minSamples](gr::Graph& flow) { return buildWindowed<UnfilteredBulkRelay>(flow, tags, kTotal, minSamples, 7UZ); }; };

        expectTagIndices("input minimum of one", runOnce(windowed(1UZ)).tags, {kAt});
        expectTagIndices(std::format("input minimum of {}", kInputMinimum), runOnce(windowed(kInputMinimum)).tags, {kAt});
    };

    "a chain of three unfiltered blocks lands an interior tag at the offset it arrived at"_test = [] {
        const std::vector<TagRecord>   tags{TagRecord{1UZ, protocolTag("r0")}, TagRecord{701UZ, protocolTag("r1")}, TagRecord{702UZ, protocolTag("r2")}};
        const std::vector<std::size_t> expected{1UZ, 701UZ, 702UZ}; // the first and the third are interior, the second falls on a boundary

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildChainWindowed(flow, tags, kInputMinimum); });
        expectTagIndices("three-block chain", result.tags, expected);
        expect(eq(result.samples.size(), kSamples)) << "every sample reaches the sink";
    };

    "a tag interior to the stream's last chunk still leaves, and the trailing window keeps its offsets"_test = [] {
        constexpr std::size_t        kTotal = 10UZ; // seven samples, then a three-sample tail below the input minimum
        const std::vector<TagRecord> tags{TagRecord{3UZ, protocolTag("r0")}, TagRecord{8UZ, protocolTag("r1")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<UnfilteredTailRelay>(flow, tags, kTotal, kInputMinimum, 7UZ); });
        expectTagIndices("stream tail", result.tags, {3UZ, 8UZ});
    };

    "at the end index a custom key crosses a block forwarding every key and stops at the opt-out"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{kSamples, protocolTag("end")}};

        const auto expectOnce = [](const EndResult& sink, std::string_view scenario) {
            expect(eq(sink.nSamples, kSamples)) << scenario;
            const std::vector<TagRecord> reserved = carrying(sink.endTags, kReservedKey);
            expect(eq(reserved.size(), 1UZ)) << std::format("{}: the tag stands once past the last sample", scenario);
            for (const TagRecord& record : sink.endTags) {
                expect(eq(record.index, kSamples)) << std::format("{}: every tag past the last sample stands at the end index", scenario);
            }
            expect(eq(carrying(sink.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << std::format("{}: the end_of_stream key stands once", scenario);
            return reserved;
        };
        const auto expectCustomKept = [](const std::vector<TagRecord>& reserved, std::string_view scenario) {
            for (const TagRecord& record : reserved) {
                const pmt::Value* value = valueOf(record.map, kCustomKey);
                expect(value != nullptr) << std::format("{}: the custom key crosses at the end index", scenario);
                if (value != nullptr) {
                    expect(eq(value->value_or(std::string_view{}), std::string_view("end"))) << std::format("{}: the value is forwarded verbatim", scenario);
                }
            }
        };

        expectCustomKept(expectOnce(runToEnd<Relay>(tags), "undeclared block"), "undeclared block");
        expectCustomKept(expectOnce(runToEnd<UnfilteredRelay>(tags), "unfiltered policy"), "unfiltered policy");

        const std::vector<TagRecord> filtered = expectOnce(runToEnd<FilteredRelay>(tags), "FilteredTagPropagation");
        for (const TagRecord& record : filtered) {
            expect(!record.map.contains(kCustomKey)) << "FilteredTagPropagation drops a custom key at the end index";
        }
    };

    "at the end index a key the block declares leaves with the value the end tag sets"_test = [] {
        property_map map = protocolTag("end");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);
        gr::tag::put(map, "sample_rate", kUpstreamRate);

        const EndResult              result  = runToEnd<UnfilteredCap>({TagRecord{kSamples, map}});
        const std::vector<TagRecord> carried = carrying(result.endTags, kCustomKey);
        expect(eq(carried.size(), 1UZ)) << "the tag stands once past the last sample";
        expect(eq(carrying(result.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once";
        if (carried.empty()) {
            return;
        }
        expect(eq(carried.front().index, result.nSamples)) << "the tag stands at the end index";
        const property_map& arrived = carried.front().map;

        const pmt::Value* owned = valueOf(arrived, kOwnedKey);
        expect(owned != nullptr);
        if (owned != nullptr) {
            expect(eq(owned->value_or<float>(0.f), kGainCeiling)) << "the block applied the end tag and forwards its capped value";
        }
        const pmt::Value* rate = valueOf(arrived, "sample_rate");
        expect(rate != nullptr);
        if (rate != nullptr) {
            expect(eq(rate->value_or<float>(0.f), kRateCeiling)) << "the block applied the end tag and forwards its capped value";
        }
    };

    "tags two samples apart all keep their custom keys"_test = [] {
        std::vector<TagRecord> tags;
        for (std::size_t i = 0UZ; i < 64UZ; ++i) {
            tags.push_back(TagRecord{300UZ + 2UZ * i, protocolTag(std::format("dense-{}", i))});
        }

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), tags.size())) << "every custom key survives, none is duplicated";
    };
};

int main() { /* not needed by the UT framework */ }
