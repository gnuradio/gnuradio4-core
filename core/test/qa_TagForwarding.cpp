#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
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

    std::size_t              nTotal = kSamples;
    std::vector<std::size_t> tagAt; // ordered stream indices

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
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct Sink : Block<Sink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(Sink, in);

    std::vector<TagRecord> tags;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
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

} // namespace qa_tag_forwarding

const boost::ut::suite<"tag forwarding"> _tagForwarding = [] {
    using namespace boost::ut;
    using namespace qa_tag_forwarding;

    const std::vector<std::size_t> kInteriorTags{0UZ, 1UZ, 2UZ, 5UZ, 6UZ, 9UZ};

    "a decimator's interior tags arrive through a negative relative index"_test = [&] {
        runChain<Decimate>(kInteriorTags, [&](Decimate& middle, Sink& sink) {
            for (std::size_t at : kInteriorTags) {
                expect(eq(countNamed(sink.tags, std::format("t{}", at)), 1UZ)) << std::format("tag t{} arrives exactly once", at);
            }
            expect(eq(middle.counts.deferred + middle.counts.atStart, kInteriorTags.size())) << "every tag is forwarded from exactly one sighting";
            expect(gt(middle.counts.deferred, 0UZ)) << "input_chunk_size forbids a boundary at every tag, and dropping the deferred ones would lose exactly those";
        });
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
};

int main() { /* not needed by the UT framework */ }
