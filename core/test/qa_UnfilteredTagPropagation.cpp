#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief `UnfilteredTagPropagation` carries every tag key across a block, and leaves every tag at the offset it arrived at.
 *
 * The default forwarder keeps only the reserved keys of `gr::tag::kDefaultTags`, so a protocol carrying its own key
 * cannot cross a single stock block. These tests pin what the policy governs — every key survives, a key the block
 * declares as a setting is substituted with the block's own current value, a key named after one of the settings
 * `Block<>` declares for every block is not, and a tag interior to a chunk keeps its offset where an input minimum
 * forbids a boundary at it — and what it leaves alone: the multi-input dedup, the merge rule and the
 * fused-versus-unfused equivalence. The compile-time guard is pinned twice: as a predicate here, and as three
 * translation units under `compile_fail/` that must not build.
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

/// the default policy: whatever a tag carries, only the reserved keys leave this block
struct Relay : Block<Relay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Relay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
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
    std::vector<float>       samples;
    std::vector<TagRecord>   tags;
    std::vector<std::size_t> runSizes;
};

[[nodiscard]] std::vector<std::size_t> collectRunSizes(const std::vector<std::vector<fusion::RunPlan>>& plan) {
    std::vector<std::size_t> runSizes;
    for (const auto& job : plan) {
        for (const fusion::RunPlan& run : job) {
            runSizes.push_back(run.members.size());
        }
    }
    return runSizes;
}

template<typename TBuild>
[[nodiscard]] RunResult runOnce(TBuild&& build, bool fusion, std::size_t chunkSamples) {
    using namespace boost::ut;

    gr::Graph flow;
    Sink*     sink = build(flow);

    const gr::property_map                                                schedulerSettings{{"enable_fusion", fusion}, {"fusion_chunk_samples", static_cast<gr::Size_t>(chunkSamples)}};
    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{schedulerSettings};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return RunResult{sink->samples, sink->tags, collectRunSizes(scheduler.fusionPlan())};
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

// three composed members; only the front decides the chunk of a fused run, so only its window matters
[[nodiscard]] Sink* buildComposedWindowed(gr::Graph& flow, const std::vector<TagRecord>& tags, std::size_t minSamples) {
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

void expectTagIndices(std::string_view scenario, const std::vector<TagRecord>& tags, const std::vector<std::size_t>& expected) {
    using namespace boost::ut;

    const std::vector<std::size_t> actual = indicesCarrying(tags, kCustomKey);
    expect(eq(actual.size(), expected.size())) << std::format("{}: {} tags carrying {}, expected {}", scenario, actual.size(), kCustomKey, expected.size());
    for (std::size_t i = 0UZ; i < std::min(actual.size(), expected.size()); ++i) {
        expect(eq(actual[i], expected[i])) << std::format("{}: tag {} arrived at output index {}, expected {}", scenario, i, actual[i], expected[i]);
    }
}

inline constexpr std::array kChunkSizes = {1UZ, 61UZ, 4096UZ};

template<typename TBuild>
void expectFusedMatchesUnfused(std::string_view scenario, TBuild&& build, std::size_t expectedRunLength) {
    using namespace boost::ut;

    const RunResult reference = runOnce(build, false, 0UZ);
    expect(eq(reference.samples.size(), kSamples)) << scenario;
    expect(reference.runSizes.empty()) << scenario;

    for (const std::size_t chunk : kChunkSizes) {
        const RunResult   fused = runOnce(build, true, chunk);
        const std::string what  = std::format("{} chunk={}", scenario, chunk);

        expect(eq(fused.runSizes.size(), 1UZ)) << what;
        if (!fused.runSizes.empty()) {
            expect(eq(fused.runSizes[0], expectedRunLength)) << what;
        }
        expect(std::ranges::equal(fused.samples, reference.samples)) << what << "samples differ";
        expect(eq(fused.tags.size(), reference.tags.size())) << what << "tag count differs";
        expect(fused.tags == reference.tags) << what << "tag sequence differs";
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

    "the default policy drops a custom key at the first block"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); }, false, 0UZ);

        expect(eq(carrying(result.tags, kReservedKey).size(), tags.size())) << "a reserved key crosses a chain of default-policy blocks";
        expect(carrying(result.tags, kCustomKey).empty()) << "a key outside kDefaultTags must not survive the default key filter";
    };

    "a custom key crosses a chain of unfiltered blocks with its value and offset intact"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); }, false, 0UZ);

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
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredCap, UnfilteredRelay>(flow, tags); }, false, 0UZ);

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
        const RunResult              unfiltered = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); }, false, 0UZ);

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

        const RunResult standard = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); }, false, 0UZ);
        expect(carrying(standard.tags, "name").empty()) << "under the default policy the key never reaches the substitution at all";
    };

    "a framework-owned key a default-policy block auto-forwards keeps the value it arrived with"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, "name", 7.f); // the upstream protocol's own use of the name, not a block name

        const std::vector<TagRecord> tags  = {TagRecord{0UZ, map}};
        const auto                   build = [&tags](gr::Graph& flow) {
            auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
            source.tagsToEmit = tags;
            auto& relay       = flow.emplaceBlock<Relay>(gr::property_map{{"name", std::string("mid")}});
            relay.settings().addAutoForwardParameters({"name"}); // the set is fixed once the block runs, so it is widened here
            auto& sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
            expect(flow.connect<"out", "in">(source, relay).has_value());
            expect(flow.connect<"out", "in">(relay, sink).has_value());
            return std::addressof(sink);
        };
        const RunResult result = runOnce(build, false, 0UZ);

        const std::vector<TagRecord> carried = carrying(result.tags, "name");
        expect(eq(carried.size(), 1UZ)) << "an auto-forwarded key survives the default key filter";
        if (!carried.empty()) {
            const pmt::Value* name = valueOf(carried.front().map, "name");
            expect(name != nullptr);
            if (name != nullptr) {
                expect(name->get_if<float>() != nullptr) << "the upstream value, not the block's own name";
                expect(eq(name->value_or<float>(0.f), 7.f));
            }
        }
    };

    "an unfiltered processOne block is a composed fusion stage"_test = [] {
        expect(gr::block::fusedStageOf<UnfilteredRelay>() != nullptr) << "the policy changes the surviving key set, not the tag regime";
        expect(gr::block::fusedStageOf<UnfilteredCap>() != nullptr);
        expect(gr::block::fusedStageOf<PolicyRelay<gr::ForwardTagPropagation>>() == nullptr) << "a policy that moves tag positions stays excluded";
    };

    "a fused chain carrying custom keys is tag for tag identical to the unfused one"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{255UZ, protocolTag("r1")}, TagRecord{256UZ, protocolTag("r2")}, TagRecord{1000UZ, protocolTag("r3")}, TagRecord{kSamples - 1UZ, protocolTag("r4")}};
        expectFusedMatchesUnfused("unfiltered chain", [&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay, UnfilteredRelay>(flow, tags); }, 3UZ);
    };

    "an owned key is substituted in run order inside a composed segment"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);

        const std::vector<TagRecord>            tags{TagRecord{0UZ, map}};
        const std::array<gr::property_map, 2UZ> caps{gr::property_map{{"max_gain", kGainCeiling}}, gr::property_map{{"max_gain", 1.f}}};
        const auto                              build = [&tags, &caps](gr::Graph& flow) { return buildSeries<UnfilteredCap, UnfilteredCap>(flow, tags, caps); };

        expectFusedMatchesUnfused("run-order substitution", build, 2UZ);

        const RunResult fused   = runOnce(build, true, 61UZ);
        const auto      carried = carrying(fused.tags, kOwnedKey);
        expect(eq(carried.size(), 1UZ));
        if (!carried.empty()) {
            expect(eq(valueOf(carried.front().map, kOwnedKey)->value_or<float>(0.f), 1.f)) << "the last member's value wins, as it does unfused";
        }
    };

    "an unfiltered processBulk member carries custom keys through its own work path"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        expectFusedMatchesUnfused("bulk member", [&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredBulkRelay, UnfilteredRelay>(flow, tags); }, 3UZ);

        const RunResult fused = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredBulkRelay, UnfilteredRelay>(flow, tags); }, true, 61UZ);
        expect(eq(carrying(fused.tags, kCustomKey).size(), tags.size())) << "the custom key crosses a composed-to-bulk and a bulk-to-composed boundary";
    };

    "an interior tag keeps its offset where an input minimum forbids a chunk boundary at it"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ; // closer to the chunk start than the input minimum, so no boundary can fall on it
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const auto windowed = [&tags](std::size_t minSamples) { return [&tags, minSamples](gr::Graph& flow) { return buildWindowed<UnfilteredBulkRelay>(flow, tags, kTotal, minSamples, 7UZ); }; };

        expectTagIndices("input minimum of one", runOnce(windowed(1UZ), false, 0UZ).tags, {kAt});
        expectTagIndices(std::format("input minimum of {}", kInputMinimum), runOnce(windowed(kInputMinimum), false, 0UZ).tags, {kAt});
    };

    "a fused chain lands an interior tag where the unfused one does"_test = [] {
        const std::vector<TagRecord>   tags{TagRecord{1UZ, protocolTag("r0")}, TagRecord{701UZ, protocolTag("r1")}, TagRecord{702UZ, protocolTag("r2")}};
        const std::vector<std::size_t> expected{1UZ, 701UZ, 702UZ}; // the first and the third are interior, the second falls on a boundary
        const auto                     build = [&tags](gr::Graph& flow) { return buildComposedWindowed(flow, tags, kInputMinimum); };

        const RunResult reference = runOnce(build, false, 0UZ);
        expectTagIndices("composed chain, unfused", reference.tags, expected);

        for (const std::size_t chunk : kChunkSizes) {
            const RunResult   fused = runOnce(build, true, chunk);
            const std::string what  = std::format("composed chain, fused chunk={}", chunk);

            expect(eq(fused.runSizes.size(), 1UZ)) << what;
            if (!fused.runSizes.empty()) {
                expect(eq(fused.runSizes[0], 3UZ)) << what;
            }
            expectTagIndices(what, fused.tags, expected);
            const bool sameTags = carrying(fused.tags, kCustomKey) == carrying(reference.tags, kCustomKey);
            expect(sameTags) << what << "tag for tag against the unfused chain";
            if (chunk >= kInputMinimum) { // a run chunk below the front's input minimum leaves the stream's last samples, fewer than that minimum, unprocessed
                expect(std::ranges::equal(fused.samples, reference.samples)) << std::format("{}: {} samples reached the sink, {} unfused", what, fused.samples.size(), reference.samples.size());
            }
        }
    };

    "a tag interior to the stream's last chunk still leaves, and the trailing window keeps its offsets"_test = [] {
        constexpr std::size_t        kTotal = 10UZ; // seven samples, then a three-sample tail below the input minimum
        const std::vector<TagRecord> tags{TagRecord{3UZ, protocolTag("r0")}, TagRecord{8UZ, protocolTag("r1")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<UnfilteredTailRelay>(flow, tags, kTotal, kInputMinimum, 7UZ); }, false, 0UZ);
        expectTagIndices("stream tail", result.tags, {3UZ, 8UZ});
    };

    "tags denser than one chunk all keep their custom keys"_test = [] {
        std::vector<TagRecord> tags;
        for (std::size_t i = 0UZ; i < 64UZ; ++i) {
            tags.push_back(TagRecord{300UZ + 2UZ * i, protocolTag(std::format("dense-{}", i))});
        }
        expectFusedMatchesUnfused("dense tags", [&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); }, 2UZ);

        const RunResult unfused = runOnce([&tags](gr::Graph& flow) { return buildSeries<UnfilteredRelay, UnfilteredRelay>(flow, tags); }, false, 0UZ);
        expect(eq(carrying(unfused.tags, kCustomKey).size(), tags.size())) << "every custom key survives, none is duplicated";
    };
};

int main() { /* not needed by the UT framework */ }
