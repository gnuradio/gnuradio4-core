#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <numeric>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

/**
 * @brief What a negative relative tag index means, and who may act on it.
 *
 * A block sees its input tags as offsets relative to the first sample of the current chunk, and that offset may be
 * negative. The default forwarder clamps a negative offset to zero and publishes the tag there. These tests pin down
 * why: a negative offset marks a tag the framework deferred out of an earlier chunk and has not forwarded yet, so the
 * clamped publication is that tag's first and only one. They also pin down the one shape where the same tag is
 * seen twice — a block that supplies its own forwardTags() and reads a wider window than the framework retires.
 *
 * The blocks are defined here: gnuradio4-core carries no standard block library, so a core test may not depend on one.
 */

namespace qa_tag_forwarding {

using namespace gr;

inline constexpr std::size_t kSamples = 64UZ;
inline constexpr gr::Size_t  kDecim   = 4U;
inline constexpr gr::Size_t  kChunk   = 8U;

/// a fixed input window for a decimator, and an input index inside it that no chunk boundary can fall on
inline constexpr std::size_t kWindow       = 16UZ;
inline constexpr std::size_t kWindowOutput = kWindow / kDecim;
inline constexpr std::size_t kInteriorAt   = 5UZ;
static_assert(kInteriorAt < kWindow && kInteriorAt % kDecim != 0UZ);

/// `trigger_name` because only the standard tag keys survive the default forwarder's key filter
inline constexpr std::string_view kNameKey = "trigger_name";

struct TagRecord {
    std::size_t  index{};
    property_map map{};
};

/// the window the default forwarder reads — `tags(1)` — split by the sign of the relative index
struct WindowCensus {
    std::size_t deferred{}; // relIndex < 0: carried out of an earlier chunk, not yet forwarded
    std::size_t atStart{};  // relIndex == 0: this chunk begins at the tag
};

void census(InputSpanLike auto& inSpan, WindowCensus& counts) {
    for (const auto& [relIndex, tagMapRef] : inSpan.tags(1UZ)) {
        if (relIndex < 0) {
            counts.deferred++;
        } else {
            counts.atStart++;
        }
    }
}

[[nodiscard]] property_map namedTag(std::size_t index) {
    property_map map;
    gr::tag::put(map, kNameKey, std::format("t{}", index));
    return map;
}

[[nodiscard]] std::size_t countNamed(const std::vector<TagRecord>& tags, std::string_view name) {
    const auto carriesName = [name](const TagRecord& record) { return std::ranges::any_of(record.map, [name](const auto& entry) { return entry.first == kNameKey && entry.second.value_or(std::string_view{}) == name; }); };
    return static_cast<std::size_t>(std::ranges::count_if(tags, carriesName));
}

struct Source : Block<Source> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Source, out);

    std::size_t               nTotal = kSamples;
    std::vector<std::size_t>  tagAt;   // ordered stream indices
    std::vector<property_map> endTags; // published at the end index, one past the last sample

    std::size_t _emitted = 0UZ;
    std::size_t _nextTag = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= nTotal) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nTotal - _emitted);
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<float>(_emitted + i);
        }
        while (_nextTag < tagAt.size() && tagAt[_nextTag] < _emitted + n) {
            outSpan.publishTag(namedTag(tagAt[_nextTag]), tagAt[_nextTag] - _emitted);
            _nextTag++;
        }
        if (_emitted + n == nTotal) { // the last call places them where the end_of_stream tag follows
            for (const property_map& tag : endTags) {
                outSpan.publishTag(tag, n);
            }
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Sink : Block<Sink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::vector<TagRecord> tags;
    std::vector<float>     samples;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// records its input like Sink, and in its epilogue the tags its input ring holds at or past its last sample
struct EndSink : Block<EndSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(EndSink, in);

    std::size_t            nSamples = 0UZ;
    std::vector<TagRecord> tags;    // tags on consumed samples
    std::vector<TagRecord> endTags; // tags at or past the end index

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        nSamples += n;
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

/// the samples a partial epilogue consumes and publishes from its tail
inline constexpr std::size_t kEpilogueTake = 2UZ;

/// a default-forwarding copier whose epilogue consumes and publishes only the first kEpilogueTake samples of its tail
struct PartialEpilogue : Block<PartialEpilogue> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(PartialEpilogue, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min({inSpan.size(), outSpan.size(), kEpilogueTake});
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// two synchronous inputs, summed
struct Add2 : Block<Add2> {
    PortIn<float>  in0;
    PortIn<float>  in1;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Add2, in0, in1, out);

    [[nodiscard]] constexpr float processOne(float a, float b) const noexcept { return a + b; }
};

/// an ordinary block: no epilogue, no forwardTags(), the default policy
struct Relay : Block<Relay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Relay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// its own forwardTags() publishes every tag of the input span, the end_of_stream key dropped; a tag past the window
/// leaves at the output span's end
struct WholeSpanForwarder : Block<WholeSpanForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(WholeSpanForwarder, in, out);

    std::size_t nPastWindow = 0UZ; // tags handed at a relative index at or past processedIn

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        gr::for_each_reader_span(
            [&](auto& inSpan) {
                if (!inSpan.isSync || !inSpan.isConnected) {
                    return;
                }
                for (const auto& [relIndex, tagMapRef] : inSpan.tags()) {
                    property_map forwarded = tagMapRef.get();
                    forwarded.erase(gr::tag::END_OF_STREAM.key());
                    if (relIndex < 0 || forwarded.empty()) {
                        continue;
                    }
                    const std::size_t at = static_cast<std::size_t>(relIndex);
                    if (at >= processedIn) {
                        nPastWindow++;
                    }
                    gr::for_each_writer_span([&](auto& outSpan) { outSpan.publishTag(forwarded, std::min(at, outSpan.size())); }, outputSpans);
                }
            },
            inputSpans);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// the forwardTags() example of the tag mechanics document, unchanged: it adds a key to every tag it forwards
struct DocumentedForwarder : Block<DocumentedForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(DocumentedForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        const auto forward = [&outputSpans](property_map tagMap) {
            tagMap.erase(gr::tag::END_OF_STREAM.key()); // publishEoS() publishes the block's own
            if (tagMap.empty()) {
                return;
            }
            tagMap["my_key"] = "my_value";
            for_each_writer_span([&tagMap](auto& outSpan) { outSpan.publishTag(tagMap, 0UZ); }, outputSpans);
        };
        for_each_reader_span(
            [&](auto& inSpan) {
                if (processedIn == 0UZ) { // the end of the stream: the tags at or past the read position
                    for (const auto& [relIndex, tagMapRef] : inSpan.tags()) {
                        if (relIndex >= 0) {
                            forward(tagMapRef.get());
                        }
                    }
                } else { // a chunk: the tags the input span retires
                    for (const auto& [relIndex, tagMapRef] : inSpan.tags(1UZ)) {
                        forward(tagMapRef.get());
                    }
                }
            },
            inputSpans);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// Inspect EOS with the real work-path InputSpan alive, as tag-aware blocks do.
struct InspectingForwarder : Block<InspectingForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(InspectingForwarder, in, out);

    std::size_t            invocations  = 0UZ;
    std::size_t            epilogueRuns = 0UZ;
    std::vector<TagRecord> seen;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        ++invocations;
        std::ignore = samples_to_eos_tag(in);
        for (const auto& [relIndex, map] : inSpan.tags(1UZ)) {
            seen.push_back({inSpan.streamIndex, map.get()});
        }
        std::ranges::copy(inSpan, outSpan.begin());
        std::ignore = inSpan.consume(inSpan.size());
        outSpan.publish(inSpan.size());
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto&, OutputSpanLike auto& outSpan) {
        ++epilogueRuns;
        outSpan.publish(0UZ);
        return work::Status::OK;
    }
};

/// default policy, but `input_chunk_size` > 1 forbids a chunk boundary at every tag
struct Decimate : Block<Decimate, Resampling<kDecim, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Decimate, in, out);

    WindowCensus counts;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        census(inSpan, counts);
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / kDecim);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i * kDecim];
        }
        std::ignore = inSpan.consume(n * kDecim);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// the same decimator under the policy written for it: the whole retired window is read and mapped to output offset 0
struct BackwardDecimate : Block<BackwardDecimate, BackwardTagPropagation, Resampling<kDecim, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(BackwardDecimate, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / kDecim);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i * kDecim];
        }
        std::ignore = inSpan.consume(n * kDecim);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// fixed-chunk shape of an FFT: the chunk is never broken at a tag, so every interior tag is deferred
struct FixedChunk : Block<FixedChunk, ForwardTagPropagation, Resampling<kChunk, kChunk, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FixedChunk, in, out);

    WindowCensus counts;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        census(inSpan, counts);
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// a fixed-chunk copier whose trailing samples arrive through processEpilogue
struct EpilogueChunk : Block<EpilogueChunk, ForwardTagPropagation, Resampling<kChunk, kChunk, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EpilogueChunk, in, out);

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

/// the accumulate-then-emit shape: it consumes every sample it is offered and has nothing to
/// publish until the stream ends, so the epilogue is the only call that can carry its result out
struct EpilogueFlush : Block<EpilogueFlush> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(EpilogueFlush, in, out);

    std::size_t epilogueRuns = 0UZ;
    float       accumulated  = 0.f;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        accumulated += std::accumulate(inSpan.begin(), inSpan.end(), 0.f);
        std::ignore = inSpan.consume(inSpan.size());
        outSpan.publish(0UZ);
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        ++epilogueRuns;
        accumulated += std::accumulate(inSpan.begin(), inSpan.end(), 0.f);
        if (!outSpan.empty()) {
            outSpan[0] = accumulated;
            outSpan.publish(1UZ);
        }
        return work::Status::OK;
    }
};

/// the rate-changing shape: its own forwardTags() maps each tag to the output offset the rate change puts it at
struct MappedForwarder : Block<MappedForwarder, Resampling<kDecim, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1000.f;

    GR_MAKE_REFLECTABLE(MappedForwarder, in, out, sample_rate);

    bool        skipDeferred = true; // the resampler's guard: ignore what an earlier chunk already handled
    std::size_t nDeferred    = 0UZ;
    std::size_t nPublished   = 0UZ;
    std::size_t retuneAfter  = 0UZ; // output samples after which the block stages a new sample_rate, 0 = never
    float       retuneTo     = 2000.f;

    std::size_t _produced = 0UZ;

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        gr::for_each_reader_span(
            [&](auto& inSpan) {
                if (!inSpan.isSync || !inSpan.isConnected) {
                    return;
                }
                for (const auto& [relIndex, tagMapRef] : inSpan.tags(processedIn)) {
                    if (relIndex < 0) {
                        nDeferred++;
                        if (skipDeferred) {
                            continue;
                        }
                    }
                    const std::size_t offset = relIndex < 0 ? 0UZ : static_cast<std::size_t>(relIndex) / kDecim;
                    gr::for_each_writer_span(
                        [&](auto& outSpan) {
                            if (outSpan.size() == 0UZ) {
                                return;
                            }
                            outSpan.publishTag(tagMapRef.get(), std::min(offset, outSpan.size() - 1UZ));
                        },
                        outputSpans);
                    nPublished++;
                }
            },
            inputSpans);
    }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / kDecim);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[i * kDecim];
        }
        std::ignore = inSpan.consume(n * kDecim);
        outSpan.publish(n);
        _produced += n;
        if (retuneAfter > 0UZ && _produced >= retuneAfter && sample_rate != retuneTo) {
            std::ignore = settings().setStaged(property_map{{"sample_rate", retuneTo}});
        }
        return work::Status::OK;
    }
};

/// stages a forwardable setting inside its own work call and stops, so the framework applies and forwards those
/// parameters while it still holds the output span
struct StageAndStop : Block<StageAndStop> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1000.f;

    GR_MAKE_REFLECTABLE(StageAndStop, in, out, sample_rate);

    std::size_t stopAfter  = 2UZ * kChunk;
    float       retuneTo   = 2000.f;
    std::size_t produced   = 0UZ;
    std::size_t lastWindow = 0UZ; ///< samples of the work call that staged the setting

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        produced += n;
        if (produced >= stopAfter) {
            lastWindow  = n;
            std::ignore = settings().setStaged(property_map{{"sample_rate", retuneTo}});
            this->requestStop();
        }
        return work::Status::OK;
    }
};

[[nodiscard]] std::vector<TagRecord> carrying(const std::vector<TagRecord>& tags, std::string_view key) {
    const auto carriesKey = [key](const TagRecord& record) { return std::ranges::any_of(record.map, [key](const auto& entry) { return entry.first == key; }); };
    return tags | std::views::filter(carriesKey) | std::ranges::to<std::vector>();
}

/// the middle block's own state is read while the scheduler that owns it is still alive
template<typename TMiddle, typename TInspect>
void runChain(const std::vector<std::size_t>& tagAt, TInspect&& inspect, auto&& configure) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagAt     = tagAt;
    auto& middle     = flow.emplaceBlock<TMiddle>(gr::property_map{{"name", std::string("mid")}});
    auto& sink       = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    configure(middle);
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    inspect(middle, sink);
}

template<typename TMiddle, typename TInspect>
void runChain(const std::vector<std::size_t>& tagAt, TInspect&& inspect) {
    runChain<TMiddle>(tagAt, std::forward<TInspect>(inspect), [](TMiddle&) {});
}

/// what an end sink recorded
struct EndResult {
    std::size_t            nSamples{};
    std::vector<TagRecord> tags;
    std::vector<TagRecord> endTags;
};

/// source -> TMiddle... -> end sink; `configure` sets up the source and the middle blocks, and `inspect` reads the
/// middle blocks while the scheduler that owns them is still alive
template<typename... TMiddle>
[[nodiscard]] EndResult runToEnd(auto&& configure, auto&& inspect) {
    using namespace boost::ut;

    gr::Graph               flow;
    auto&                   source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    std::tuple<TMiddle&...> middle{flow.emplaceBlock<TMiddle>()...}; // a braced initializer fixes the emplacement order
    auto&                   sink = flow.emplaceBlock<EndSink>(gr::property_map{{"name", std::string("snk")}});
    configure(source, middle);

    const auto link  = [&flow](auto& from, auto& to) { expect(flow.connect<"out", "in">(from, to).has_value()); };
    auto       chain = std::apply([&](auto&... blocks) { return std::tie(source, blocks..., sink); }, middle);
    [&]<std::size_t... I>(std::index_sequence<I...>) { (link(std::get<I>(chain), std::get<I + 1UZ>(chain)), ...); }(std::make_index_sequence<sizeof...(TMiddle) + 1UZ>{});

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    inspect(middle);
    return EndResult{sink.nSamples, sink.tags, sink.endTags};
}

/// a source of kSamples samples that publishes `endTags` at its end index
template<typename... TMiddle>
[[nodiscard]] EndResult runToEnd(const std::vector<property_map>& endTags) {
    return runToEnd<TMiddle...>([&endTags](Source& source, auto&) { source.endTags = endTags; }, [](auto&) {});
}

/// the tag named `name` stands once at the sink's end index and on no sample; the end_of_stream key stands once
void expectAtEnd(const EndResult& result, std::string_view name) {
    using namespace boost::ut;

    expect(eq(countNamed(result.endTags, name), 1UZ)) << std::format("{} stands once past the sink's last sample", name);
    expect(eq(countNamed(result.tags, name), 0UZ)) << std::format("{} rides no sample", name);
    for (const TagRecord& record : result.endTags) {
        expect(eq(record.index, result.nSamples)) << "every tag past the last sample stands at the end index";
    }
    expect(eq(carrying(result.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once";
}

} // namespace qa_tag_forwarding

const boost::ut::suite<"tag forwarding"> _tagForwarding = [] {
    using namespace boost::ut;
    using namespace qa_tag_forwarding;

    const std::vector<std::size_t> kInteriorTags{0UZ, 1UZ, 2UZ, 5UZ, 6UZ, 9UZ};

    "tag inspection during work delivers each tag exactly once"_test = [] {
        const std::vector<std::size_t> tagAt{0UZ, 1UZ, 7UZ, 8UZ, 9UZ, 63UZ};
        runChain<InspectingForwarder>(
            tagAt,
            [&](InspectingForwarder& middle, Sink& sink) {
                expect(gt(middle.invocations, 1UZ));
                expect(eq(sink.samples.size(), kSamples));
                expect(std::ranges::equal(sink.samples, std::views::iota(0UZ, kSamples) | std::views::transform([](auto i) { return static_cast<float>(i); })));
                for (const auto at : tagAt) {
                    expect(eq(countNamed(middle.seen, std::format("t{}", at)), 1UZ)) << "input tag observed once across work calls";
                    expect(eq(countNamed(sink.tags, std::format("t{}", at)), 1UZ)) << "output tag delivered once";
                }
                expect(eq(middle.epilogueRuns, 1UZ)) << "EOS completes the work path once";
            },
            [](InspectingForwarder& middle) { middle.in.max_samples = kChunk; });
    };

    "a decimator's interior tags arrive through a negative relative index"_test = [&] {
        runChain<Decimate>(kInteriorTags, [&](Decimate& middle, Sink& sink) {
            for (std::size_t at : kInteriorTags) {
                expect(eq(countNamed(sink.tags, std::format("t{}", at)), 1UZ)) << std::format("tag t{} arrives exactly once", at);
            }
            expect(eq(middle.counts.deferred + middle.counts.atStart, kInteriorTags.size())) << "every tag is forwarded from exactly one sighting";
            expect(gt(middle.counts.deferred, 0UZ)) << "input_chunk_size forbids a boundary at every tag, and dropping the deferred ones would lose exactly those";
        });
    };

    "BackwardTagPropagation places an interior tag on the chunk that consumed it"_test = [&] {
        const std::vector<std::size_t> oneInteriorTag{kInteriorAt};
        const auto                     fixedWindow = [](auto& middle) {
            middle.in.min_samples = kWindow; // a chunk then starts only on a multiple of the window, never at the tag
            middle.in.max_samples = kWindow;
        };
        const auto onlyTagIndex = [](Sink& sink) {
            expect(eq(sink.tags.size(), 1UZ)) << "the tag must arrive exactly once";
            return sink.tags.size() == 1UZ ? sink.tags.front().index : std::numeric_limits<std::size_t>::max();
        };

        std::size_t defaultIndex  = 0UZ;
        std::size_t backwardIndex = 0UZ;
        runChain<Decimate>(oneInteriorTag, [&](Decimate&, Sink& sink) { defaultIndex = onlyTagIndex(sink); }, fixedWindow);
        runChain<BackwardDecimate>(oneInteriorTag, [&](BackwardDecimate&, Sink& sink) { backwardIndex = onlyTagIndex(sink); }, fixedWindow);

        expect(lt(backwardIndex, kWindowOutput)) << "the tag must land on an output sample the chunk holding it produced";
        expect(ge(defaultIndex, kWindowOutput)) << "the default forwarder reads only tags(1), so it first sees the tag one chunk later";
    };

    "tags ride the tail through processEpilogue"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        source.nTotal    = 62UZ; // seven full chunks of kChunk, then a six-sample tail
        source.tagAt     = {56UZ, 60UZ};
        auto& middle     = flow.emplaceBlock<EpilogueChunk>(gr::property_map{{"name", std::string("mid")}});
        auto& sink       = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(middle.epilogueRuns, 1UZ)) << "the tail did not go through the epilogue, so this test discriminates nothing";
        expect(eq(countNamed(sink.tags, "t56"), 1UZ)) << "the tag at the tail's first sample was dropped";
        expect(eq(countNamed(sink.tags, "t60"), 1UZ)) << "the tag interior to the tail was dropped";
    };

    "a block that consumes all of its input still runs its epilogue at end of stream"_test = [] {
        constexpr std::size_t kTotal = 40UZ;

        gr::Graph flow;
        auto&     source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        source.nTotal    = kTotal;
        auto& middle     = flow.emplaceBlock<EpilogueFlush>(gr::property_map{{"name", std::string("mid")}});
        auto& sink       = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(middle.epilogueRuns, 1UZ)) << "a block that leaves nothing trailing got no end-of-stream call";
        expect(eq(sink.samples.size(), 1UZ)) << "the epilogue's flush did not reach the sink";
        if (sink.samples.size() == 1UZ) {
            const float expected = static_cast<float>(kTotal * (kTotal - 1UZ) / 2UZ);
            expect(eq(sink.samples.front(), expected)) << "the flush carried a total the block never accumulated";
        }
    };

    "a fixed-chunk block defers every tag that is not at its chunk boundary"_test = [&] {
        runChain<FixedChunk>(
            kInteriorTags,
            [&](FixedChunk& middle, Sink& sink) {
                for (std::size_t at : kInteriorTags) {
                    expect(eq(countNamed(sink.tags, std::format("t{}", at)), 1UZ)) << std::format("tag t{} arrives exactly once", at);
                }
                expect(eq(middle.counts.deferred + middle.counts.atStart, kInteriorTags.size())) << "every tag is forwarded from exactly one sighting";
                expect(gt(middle.counts.deferred, 0UZ)) << "ForwardTagPropagation never breaks a chunk at a tag";
            },
            [](FixedChunk& middle) { middle.in.max_samples = kChunk; }); // more than one chunk, so a deferred tag has a chunk to arrive in
    };

    "a custom forwardTags that reads the whole chunk sees a deferred tag a second time"_test = [&] {
        runChain<MappedForwarder>(kInteriorTags, [&](MappedForwarder& middle, Sink& sink) {
            for (std::size_t at : kInteriorTags) {
                expect(eq(countNamed(sink.tags, std::format("t{}", at)), 1UZ)) << std::format("tag t{} arrives exactly once", at);
            }
            expect(gt(middle.nDeferred, 0UZ)) << "the framework retires only tags(1), so the rest come back";
            expect(eq(middle.nPublished, kInteriorTags.size())) << "the guard publishes each tag once";
        });
    };

    "without the guard the same custom forwardTags duplicates them"_test = [&] {
        runChain<MappedForwarder>(
            kInteriorTags,
            [&](MappedForwarder& middle, Sink& sink) {
                std::size_t nDelivered = 0UZ;
                for (std::size_t at : kInteriorTags) {
                    nDelivered += countNamed(sink.tags, std::format("t{}", at));
                }
                expect(gt(nDelivered, kInteriorTags.size())) << "each deferred tag is published a second time";
                expect(eq(middle.nPublished, kInteriorTags.size() + middle.nDeferred)) << "the extras are exactly the deferred sightings";
            },
            [](MappedForwarder& middle) { middle.skipDeferred = false; });
    };

    "forwarded parameters never land before a tag the same work call already published"_test = [] {
        runChain<MappedForwarder>(
            {2UZ, 12UZ, 13UZ}, // the first chunk stages the retune, the second maps its tags to a non-zero output offset
            [&](MappedForwarder& middle, Sink& sink) {
                const std::vector<TagRecord> retuned = carrying(sink.tags, "sample_rate");
                expect(eq(retuned.size(), 1UZ)) << "the staged parameters must reach the sink exactly once";
                expect(std::ranges::is_sorted(sink.tags, {}, &TagRecord::index)) << "a tag published behind one already in the span breaks the span's order";
                if (retuned.size() == 1UZ) {
                    const auto sharingIndex = std::ranges::count(sink.tags, retuned.front().index, &TagRecord::index);
                    expect(ge(static_cast<std::size_t>(sharingIndex), 2UZ)) << "the parameters must ride at the mapped tag of their work call, or the ordering hazard is not reproduced";
                }
                expect(eq(middle.nPublished, 3UZ)) << "every source tag is mapped exactly once";
            },
            [](MappedForwarder& middle) {
                middle.in.min_samples = kChunk; // a fixed window: the retune is staged in one work call and forwarded in the next, which carries the mapped tags
                middle.in.max_samples = kChunk;
                middle.retuneAfter    = 1UZ;
            });
    };

    "a block that stages a setting and stops forwards it through its open output span"_test = [] {
        gr::Graph flow;
        auto&     source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        auto&     middle      = flow.emplaceBlock<StageAndStop>(gr::property_map{{"name", std::string("mid")}});
        auto&     sink        = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
        middle.in.max_samples = kChunk; // the staging work call must not be the first, so its window does not start at 0
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        const std::vector<TagRecord> retuned = carrying(sink.tags, "sample_rate");
        expect(eq(retuned.size(), 1UZ)) << "the staged parameters must reach the sink exactly once";
        if (retuned.size() == 1UZ) {
            expect(eq(retuned.front().index, middle.produced - middle.lastWindow)) << "the tag marks the work call the setting was staged in";
            expect(eq(retuned.front().map.size(), 1UZ)) << "only the staged parameter may be forwarded";
        }
        expect(eq(sink.tags.size(), retuned.size())) << "no other tag may surface: an untagged source produces none";
        expect(gt(middle.lastWindow, 0UZ));
        expect(gt(middle.produced, middle.lastWindow)) << "the staging work call must not be the first";
    };
    "a tag at the source's end index crosses a block without an epilogue to the sink's end index"_test = [] {
        const EndResult result = runToEnd<Relay>({namedTag(kSamples)});
        expect(eq(result.nSamples, kSamples));
        expectAtEnd(result, std::format("t{}", kSamples));
    };

    "a tag at the source's end index crosses two ordinary blocks"_test = [] {
        const EndResult result = runToEnd<Relay, Relay>({namedTag(kSamples)});
        expect(eq(result.nSamples, kSamples));
        expectAtEnd(result, std::format("t{}", kSamples));
    };

    "a block whose epilogue reads no tag still passes the tag at the end index"_test = [] {
        std::size_t     epilogueRuns = 0UZ;
        const EndResult result       = runToEnd<InspectingForwarder>([](Source& source, auto&) { source.endTags = {namedTag(kSamples)}; }, [&epilogueRuns](auto& middle) { epilogueRuns = std::get<0UZ>(middle).epilogueRuns; });
        expect(eq(epilogueRuns, 1UZ)) << "the block did not run its epilogue, so this case discriminates nothing";
        expect(eq(result.nSamples, kSamples));
        expectAtEnd(result, std::format("t{}", kSamples));
    };

    "a tag on samples a block without an epilogue leaves unconsumed stands at its end index"_test = [] {
        constexpr std::size_t kTotal = 62UZ; // seven chunks of kChunk, then six samples below the input minimum
        constexpr std::size_t kTagAt = 58UZ; // inside the tail
        const EndResult       result = runToEnd<Relay>(
            [](Source& source, auto& middle) {
                source.nTotal        = kTotal;
                source.tagAt         = {kTagAt};
                Relay& relay         = std::get<0UZ>(middle);
                relay.in.min_samples = kChunk;
                relay.in.max_samples = kChunk;
            },
            [](auto&) {});
        expect(eq(result.nSamples, kTotal - kTotal % kChunk)) << "the block passed the tail on, so this case discriminates nothing";
        expectAtEnd(result, std::format("t{}", kTagAt));
    };

    "a forwardTags() override is handed the tags past the end, and the default forwarder never runs in its place"_test = [] {
        std::size_t     nPastWindow = 0UZ;
        const EndResult handed      = runToEnd<WholeSpanForwarder>([](Source& source, auto&) { source.endTags = {namedTag(kSamples)}; }, [&nPastWindow](auto& middle) { nPastWindow = std::get<0UZ>(middle).nPastWindow; });
        expect(eq(nPastWindow, 1UZ)) << "the override saw the end tag past its window once";
        expectAtEnd(handed, std::format("t{}", kSamples));

        std::size_t     nPublished = 0UZ;
        const EndResult declined   = runToEnd<MappedForwarder>([](Source& source, auto&) { source.endTags = {namedTag(kSamples)}; }, [&nPublished](auto& middle) { nPublished = std::get<0UZ>(middle).nPublished; });
        expect(eq(nPublished, 0UZ)) << "an override that reads only its window publishes nothing at the end";
        expect(eq(countNamed(declined.endTags, std::format("t{}", kSamples)), 0UZ)) << "no forwarder ran in place of the override";
        expect(eq(carrying(declined.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once";
    };

    "the documented forwardTags() example forwards each tag once, in its chunk and at the end index"_test = [] {
        constexpr std::size_t kTagAt = 20UZ;
        const EndResult       result = runToEnd<DocumentedForwarder>(
            [](Source& source, auto&) {
                source.tagAt   = {kTagAt};
                source.endTags = {namedTag(kSamples)};
            },
            [](auto&) {});
        expect(eq(result.nSamples, kSamples));
        const std::string interior = std::format("t{}", kTagAt);
        expect(eq(countNamed(result.tags, interior), 1UZ)) << "the interior tag crosses once";
        for (const TagRecord& record : result.tags) {
            if (countNamed({record}, interior) == 1UZ) {
                expect(eq(record.index, kTagAt)) << "the interior tag keeps its offset";
            }
        }
        expectAtEnd(result, std::format("t{}", kSamples));
        const std::vector<TagRecord> marked = carrying(result.endTags, "my_key");
        expect(eq(marked.size(), 1UZ)) << "the override's key rides the end tag once";
        expect(carrying(marked, gr::tag::END_OF_STREAM.key()).empty()) << "the override forwards no end_of_stream key";
    };

    "an epilogue that consumes part of its tail places each tail tag at its end index, and both tags cross the next block"_test = [] {
        constexpr std::size_t kTotal = 62UZ; // seven chunks of kChunk, then a six-sample tail
        constexpr std::size_t kTagAt = 59UZ; // in the part of the tail the epilogue leaves unconsumed
        const EndResult       result = runToEnd<PartialEpilogue, Relay>(
            [](Source& source, auto& middle) {
                source.nTotal          = kTotal;
                source.tagAt           = {kTagAt};
                source.endTags         = {namedTag(kTotal)};
                PartialEpilogue& block = std::get<0UZ>(middle);
                block.in.min_samples   = kChunk;
                block.in.max_samples   = kChunk;
            },
            [](auto&) {});
        expect(eq(result.nSamples, kTotal - kTotal % kChunk + kEpilogueTake)) << "the epilogue consumed its whole tail, so this case discriminates nothing";

        expectAtEnd(result, std::format("t{}", kTagAt)); // the epilogue published only part of its tail, so the tag moved to its end index
        expectAtEnd(result, std::format("t{}", kTotal));
    };

    "a block whose inputs end at different indices forwards the tags up to the first end only"_test = [] {
        constexpr std::size_t                  kLongTotal = 8UZ * kSamples;
        constexpr std::array<std::size_t, 2UZ> kLongTagAt{100UZ, 300UZ}; // past the short input's end

        gr::Graph flow;
        auto&     shortSource = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("short")}});
        shortSource.endTags   = {namedTag(kSamples)};
        auto& longSource      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("long")}});
        longSource.nTotal     = kLongTotal;
        longSource.tagAt      = {kLongTagAt[0], kLongTagAt[1]};
        auto& add             = flow.emplaceBlock<Add2>(gr::property_map{{"name", std::string("add")}});
        auto& sink            = flow.emplaceBlock<EndSink>(gr::property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in0">(shortSource, add).has_value());
        expect(flow.connect<"out", "in1">(longSource, add).has_value());
        expect(flow.connect<"out", "in">(add, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        const EndResult result{sink.nSamples, sink.tags, sink.endTags};
        expect(eq(result.nSamples, kSamples));
        expectAtEnd(result, std::format("t{}", kSamples));
        for (const std::size_t at : kLongTagAt) {
            const std::string name = std::format("t{}", at);
            expect(eq(countNamed(result.tags, name) + countNamed(result.endTags, name), 0UZ)) << std::format("{} lies past the first input's end and leaves nowhere", name);
        }
    };
};

int main() { /* not needed by the UT framework */ }
