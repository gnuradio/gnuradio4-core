#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <memory_resource>
#include <new>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

// The cases that run the writer inside a reader's registration replace the global allocation functions. Clang's thread
// and memory sanitizer runtimes define them too, and a program that also defines them fails to link. A build under
// those runtimes leaves the cases out.
#if defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define QA_TAG_STORE_NO_DEALLOCATION_HOOK 1
#endif
#endif

/**
 * @brief The segmented tag store behind a port: what it allocates, what it frees, and what each reader sees.
 *
 * A port's tags live in segments of SegmentedBuffer<Tag>::kSlotsPerSegment slots. The writer allocates a segment
 * when it first writes into it and frees it once every reader has passed it. A segment index, which doubles as the
 * segments in use outgrow it, maps a position to its segment. The cases count the store's segments and its other
 * allocations through the memory resource the port is sized with.
 */

namespace qa_tag_store {

using namespace gr;

inline constexpr std::size_t kSlotsPerSegment  = SegmentedBuffer<Tag>::kSlotsPerSegment;
inline constexpr std::size_t kSegmentAlignment = SegmentedBuffer<Tag>::kSegmentAlignment;

// counts the bytes, the segments and the other blocks a tag store holds through it; a segment is the one block
// allocated at the segment alignment
struct CountingResource : std::pmr::memory_resource {
    std::size_t allocated{0UZ};
    std::size_t held{0UZ};
    std::size_t segments{0UZ};
    std::size_t peakSegments{0UZ};
    std::size_t otherBlocks{0UZ}; // the segment index: a header and its entries per index held

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        allocated += bytes;
        held += bytes;
        if (alignment == kSegmentAlignment) {
            peakSegments = std::max(peakSegments, ++segments);
        } else {
            ++otherBlocks;
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        held -= bytes;
        if (alignment == kSegmentAlignment) {
            --segments;
        } else {
            --otherBlocks;
        }
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

[[nodiscard]] property_map numbered(std::size_t n) { return property_map{{"n", static_cast<int>(n)}}; }

[[nodiscard]] std::size_t numberOf(const Tag& tag) { return static_cast<std::size_t>(tag.map.at("n").value_or(-1)); }

// publishes nSamples samples, each carrying a tag numbered with its stream index
void publishTaggedChunk(PortOut<float>& out, std::size_t nSamples, std::size_t& written) {
    auto span = out.tryReserve<SpanReleasePolicy::ProcessAll>(nSamples);
    for (std::size_t i = 0UZ; i < nSamples; ++i) {
        span.publishTag(numbered(written + i), i);
        span[i] = static_cast<float>(written + i);
    }
    span.publish(nSamples);
    written += nSamples;
}

struct ReadCensus {
    std::size_t received{0UZ};
    std::size_t misplaced{0UZ};
};

// reads nSamples samples and checks that every tag in the span sits at the sample its number names
void readChunk(PortIn<float>& in, std::size_t nSamples, ReadCensus& census) {
    auto span = in.get<SpanReleasePolicy::ProcessAll>(nSamples);
    for (const Tag& tag : span.rawTags) {
        const std::size_t n = numberOf(tag);
        census.misplaced += (tag.index != n || span[tag.index - span.streamIndex] != static_cast<float>(n)) ? 1UZ : 0UZ;
        ++census.received;
    }
}

inline constexpr std::size_t kGraphSamples   = 16384UZ;
inline constexpr std::size_t kGraphTagStride = 2UZ;
inline constexpr std::size_t kSlowChunk      = 1024UZ;
static_assert(kGraphSamples % kSlowChunk == 0UZ);

struct TaggingSource : Block<TaggingSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(TaggingSource, out);

    std::size_t _emitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), kGraphSamples - _emitted);
        for (std::size_t i = 0UZ; i < n; ++i) {
            if ((_emitted + i) % kGraphTagStride == 0UZ) {
                outSpan.publishTag(numbered(_emitted + i), i);
            }
            outSpan[i] = static_cast<float>(_emitted + i);
        }
        _emitted += n;
        outSpan.publish(n);
        return _emitted == kGraphSamples ? work::Status::DONE : work::Status::OK;
    }
};

struct CheckingSink : Block<CheckingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(CheckingSink, in);

    std::size_t samples{0UZ};
    std::size_t tags{0UZ};
    std::size_t misplaced{0UZ};
    std::size_t mostTagsInOneCall{0UZ};

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        for (std::size_t i = 0UZ; i < n; ++i) {
            misplaced += inSpan[i] != static_cast<float>(samples + i) ? 1UZ : 0UZ;
        }
        for (const Tag& tag : inSpan.rawTags) {
            const std::size_t number = numberOf(tag);
            misplaced += (tag.index != number || tag.index < inSpan.streamIndex || tag.index - inSpan.streamIndex >= n || inSpan[tag.index - inSpan.streamIndex] != static_cast<float>(number)) ? 1UZ : 0UZ;
        }
        tags += inSpan.rawTags.size();
        mostTagsInOneCall = std::max(mostTagsInOneCall, inSpan.rawTags.size());
        samples += n;
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

// writes n tags, each carrying its own position as its index
template<typename TWriter>
void writePositioned(TWriter& writer, std::size_t n) {
    const std::size_t first = writer.position();
    auto              span  = writer.tryReserve(n);
    for (std::size_t i = 0UZ; i < span.size(); ++i) {
        span[i].index = first + i;
    }
    span.publish(span.size());
}

// reads every waiting tag, consumes them and returns the index each carries
template<typename TReader>
[[nodiscard]] std::vector<std::size_t> readIndexes(TReader& reader) {
    auto                     span = reader.get();
    std::vector<std::size_t> indexes;
    for (std::size_t i = 0UZ; i < span.size(); ++i) {
        indexes.push_back(span[i].index);
    }
    std::ignore = span.consume(span.size());
    return indexes;
}

[[nodiscard]] std::vector<std::size_t> positionsFrom(std::size_t first, std::size_t end) {
    std::vector<std::size_t> positions(end - first);
    std::iota(positions.begin(), positions.end(), first);
    return positions;
}

[[nodiscard]] constexpr std::size_t segmentsSpanning(std::size_t first, std::size_t end) { return first < end ? (end - 1UZ) / kSlotsPerSegment - first / kSlotsPerSegment + 1UZ : 0UZ; }

#ifdef QA_TAG_STORE_NO_DEALLOCATION_HOOK
inline constexpr bool kHooksDeallocation = false;
#else
inline constexpr bool kHooksDeallocation = true;
#endif

// an action that the next global deallocation on this thread runs once
struct DeallocationHook {
    void (*run)(void*){nullptr};
    void* action{nullptr};
};
inline constinit thread_local DeallocationHook nextDeallocationHook{};

// Runs duringRegistration inside store.new_reader(), at its first deallocation. A registering reader deallocates
// after it enters the reader set and before it tells the writer the set changed.
template<typename TBuffer, typename TAction>
[[nodiscard]] auto attachRunning(TBuffer& store, TAction& duringRegistration) {
    nextDeallocationHook = {.run = [](void* action) { (*static_cast<TAction*>(action))(); }, .action = &duringRegistration};
    auto reader          = store.new_reader();
    nextDeallocationHook = {};
    return reader;
}

// true while nRegistering readers register beside nCounted counted readers: they are in the reader set and not yet
// counted
template<typename TBuffer>
[[nodiscard]] bool registersBeside(TBuffer& store, std::size_t nCounted, std::size_t nRegistering = 1UZ) {
    return gr::detail::loadSequences(store.claim_strategy()._readSequences)->size() == nCounted + nRegistering && store.n_readers() == nCounted;
}

} // namespace qa_tag_store

#ifndef QA_TAG_STORE_NO_DEALLOCATION_HOOK
void* operator new(std::size_t bytes) {
    if (void* allocated = std::malloc(bytes == 0UZ ? 1UZ : bytes)) {
        return allocated;
    }
    throw std::bad_alloc();
}

void operator delete(void* allocated) noexcept {
    std::free(allocated);
    if (const qa_tag_store::DeallocationHook hook = std::exchange(qa_tag_store::nextDeallocationHook, {}); hook.run != nullptr) {
        hook.run(hook.action);
    }
}

void operator delete(void* allocated, std::size_t) noexcept { ::operator delete(allocated); }
#endif

const boost::ut::suite<"segmented tag store"> _tagStoreTests = [] {
    using namespace boost::ut;
    using namespace qa_tag_store;

    "an edge that carries no tag allocates no tag slot, and its first tag costs the same on a deep edge"_test = [] {
        constexpr std::size_t kDeep  = 4194304UZ;
        constexpr std::size_t kChunk = 1024UZ;

        CountingResource deepMemory;
        PortOut<float>   out;
        PortIn<float>    in;
        expect(out.resizeBuffer(kDeep, nullptr, &deepMemory).has_value());
        expect(out.connect(in).has_value());
        const std::size_t deepSlots = out.buffer().tagBuffer.size();
        for (std::size_t round = 0UZ; round < 8UZ; ++round) {
            {
                auto span = out.tryReserve<SpanReleasePolicy::ProcessAll>(kChunk);
                expect(fatal(eq(span.size(), kChunk)));
                expect(eq(span.tags.size(), deepSlots)) << "every reservation reserves every free tag slot";
                std::ranges::fill(span, 1.0f);
                span.publish(kChunk);
            }
            auto span = in.get<SpanReleasePolicy::ProcessAll>(kChunk);
            expect(span.rawTags.empty());
        }
        expect(eq(deepMemory.allocated, 0UZ)) << "reserving tag slots allocates none";

        std::size_t written = 8UZ * kChunk;
        publishTaggedChunk(out, 1UZ, written);
        expect(eq(deepMemory.segments, 1UZ)) << "the first tag allocates one segment";
        expect(eq(deepMemory.otherBlocks, 2UZ)) << "the first tag allocates one segment index";

        CountingResource defaultMemory;
        PortOut<float>   defaultOut;
        PortIn<float>    defaultIn;
        expect(defaultOut.resizeBuffer(PortOut<float>::kDefaultBufferSize, nullptr, &defaultMemory).has_value());
        expect(defaultOut.connect(defaultIn).has_value());
        std::size_t defaultWritten = 0UZ;
        publishTaggedChunk(defaultOut, 1UZ, defaultWritten);
        expect(gt(deepSlots, defaultOut.buffer().tagBuffer.size())) << "the deep edge has the larger bound";
        expect(eq(deepMemory.held, defaultMemory.held)) << std::format("one tag holds {} bytes on the deep edge and {} on the default edge", deepMemory.held, defaultMemory.held);
    };

    "two readers on one edge, one lagging across several segments, see every tag at its index"_test = [] {
        constexpr std::size_t kChunk       = 37UZ; // chunks straddle segment boundaries
        constexpr std::size_t kLagChunks   = 12UZ;
        constexpr std::size_t kRounds      = 6UZ;
        constexpr std::size_t kLagSegments = kChunk * kLagChunks / kSlotsPerSegment;
        static_assert(kLagSegments >= 5UZ);

        CountingResource tagMemory;
        PortOut<float>   out;
        PortIn<float>    fast;
        PortIn<float>    slow;
        expect(out.resizeBuffer(4096UZ, nullptr, &tagMemory).has_value());
        expect(out.connect(fast).has_value());
        expect(out.connect(slow).has_value());

        std::size_t written = 0UZ;
        ReadCensus  fastCensus;
        ReadCensus  slowCensus;
        for (std::size_t round = 0UZ; round < kRounds; ++round) {
            for (std::size_t c = 0UZ; c < kLagChunks; ++c) {
                publishTaggedChunk(out, kChunk, written);
                readChunk(fast, kChunk, fastCensus);
            }
            readChunk(slow, kChunk * kLagChunks, slowCensus);
        }

        expect(eq(out.nTagsDropped, 0UZ)) << "no tag found the store full";
        expect(eq(fastCensus.received, written)) << "the leading reader sees every tag";
        expect(eq(slowCensus.received, written)) << "the lagging reader sees every tag";
        expect(eq(fastCensus.misplaced + slowCensus.misplaced, 0UZ)) << "every tag arrives at the sample it was published on";
        expect(ge(tagMemory.peakSegments, kLagSegments)) << std::format("the lagging reader held {} segments, fewer than its lag of {}", tagMemory.peakSegments, kLagSegments);

        { // a reservation frees what both readers have passed
            auto span = out.tryReserve<SpanReleasePolicy::ProcessAll>(1UZ);
            span.publish(1UZ);
        }
        expect(le(tagMemory.segments, 1UZ)) << std::format("{} segments outlive both readers", tagMemory.segments);
        expect(eq(tagMemory.otherBlocks, 2UZ)) << "the store keeps one segment index once both readers have passed every replaced one";
    };

    "a reader span keeps reading its tags while the writer replaces the segment index"_test = [] {
        CountingResource tagMemory;
        PortOut<float>   out;
        PortIn<float>    in;
        expect(out.resizeBuffer(4096UZ, nullptr, &tagMemory).has_value());
        expect(out.connect(in).has_value());

        std::size_t written = 0UZ;
        publishTaggedChunk(out, kSlotsPerSegment, written);
        expect(eq(tagMemory.otherBlocks, 2UZ)) << "one segment index";
        {
            auto held = in.get<SpanReleasePolicy::ProcessAll>(kSlotsPerSegment);
            expect(fatal(eq(held.rawTags.size(), kSlotsPerSegment)));
            constexpr std::size_t kGrowth = 2UZ * SegmentedBuffer<Tag>::kInitialIndexLength;
            for (std::size_t s = 0UZ; s < kGrowth; ++s) {
                publishTaggedChunk(out, kSlotsPerSegment, written);
            }
            expect(ge(tagMemory.peakSegments, kGrowth)) << "the waiting segments outgrow the first index";
            expect(gt(tagMemory.otherBlocks, 2UZ)) << "the store keeps the replaced index while a span may read through it";

            ReadCensus census;
            for (const Tag& tag : held.rawTags) {
                census.misplaced += (tag.index != numberOf(tag) || held[tag.index - held.streamIndex] != static_cast<float>(tag.index)) ? 1UZ : 0UZ;
                ++census.received;
            }
            expect(eq(census.received, kSlotsPerSegment));
            expect(eq(census.misplaced, 0UZ)) << "the span reads its tags through the index it was created with";
            const auto middle = std::ranges::lower_bound(held.rawTags, kSlotsPerSegment / 2UZ, std::ranges::less{}, &Tag::index);
            expect(fatal(middle != held.rawTags.end()));
            expect(eq(middle->index, kSlotsPerSegment / 2UZ)) << "a binary search over the tag span finds a tag by its index";
        }

        ReadCensus census;
        readChunk(in, written - kSlotsPerSegment, census);
        expect(eq(census.received, written - kSlotsPerSegment)) << "a span made after the replacement sees every later tag";
        expect(eq(census.misplaced, 0UZ));
        {
            auto span = out.tryReserve<SpanReleasePolicy::ProcessAll>(1UZ);
            span.publish(1UZ);
        }
        expect(eq(tagMemory.otherBlocks, 2UZ)) << "the reservation after the reader passed the replacement frees the replaced index";
        expect(le(tagMemory.segments, 1UZ));
    };

    "a tag on every sample beyond the bound is dropped and counted, and the store stops at the bound"_test = [] {
        constexpr std::size_t kDeep  = 4194304UZ;
        constexpr std::size_t kBound = PortOut<float>::kMaxTagBufferSize;
        constexpr std::size_t kChunk = kBound + kBound / 2UZ;

        CountingResource tagMemory;
        PortOut<float>   out;
        PortIn<float>    in;
        expect(out.resizeBuffer(kDeep, nullptr, &tagMemory).has_value());
        expect(out.connect(in).has_value());
        const std::size_t slots = out.buffer().tagBuffer.size();
        expect(eq(slots, kBound + 1UZ)) << "the store holds the bound and the end-of-stream slot";

        std::size_t written = 0UZ;
        publishTaggedChunk(out, kChunk, written);
        expect(eq(out.nTagsDropped, kChunk - kBound)) << "every tag past the bound is counted";
        expect(le(tagMemory.peakSegments, slots / kSlotsPerSegment + 1UZ)) << std::format("the store took {} segments for {} slots", tagMemory.peakSegments, slots);

        auto span = in.get<SpanReleasePolicy::ProcessAll>(kChunk);
        expect(fatal(eq(span.size(), kChunk))) << "every sample of the chunk passes";
        expect(fatal(eq(span.rawTags.size(), kBound))) << "the tags up to the bound arrive";
        expect(eq(numberOf(span.rawTags[kBound - 1UZ]), kBound - 1UZ)) << "the kept tags are the first ones";
    };

    "two sinks on one edge under the multi-threaded scheduler, one lagging by several segments, see every tag at its index"_test = [] {
        Graph flow;
        auto& source       = flow.emplaceBlock<TaggingSource>();
        auto& lead         = flow.emplaceBlock<CheckingSink>();
        auto& lag          = flow.emplaceBlock<CheckingSink>();
        lag.in.min_samples = kSlowChunk; // the lagging sink takes its tags a chunk at a time
        lag.in.max_samples = kSlowChunk;
        expect(flow.connect<"out", "in">(source, lead).has_value());
        expect(flow.connect<"out", "in">(source, lag).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        constexpr std::size_t kTags = kGraphSamples / kGraphTagStride;
        expect(eq(source.out.nTagsDropped, 0UZ)) << "no tag found the store full";
        for (const CheckingSink* sink : {&lead, &lag}) {
            expect(eq(sink->samples, kGraphSamples)) << "every sample arrives";
            expect(eq(sink->tags, kTags)) << "every tag arrives";
            expect(eq(sink->misplaced, 0UZ)) << "every sample and tag arrives at its index";
        }
        expect(eq(lag.mostTagsInOneCall, kSlowChunk / kGraphTagStride)) << "the lagging sink reads several segments of tags in one call";
        static_assert(kSlowChunk / kGraphTagStride >= 4UZ * kSlotsPerSegment);
    };

    "readers that attach while the writer frees segments read only live tags at their positions"_test = [] {
        constexpr std::size_t kSlots = 8UZ * kSlotsPerSegment;
        constexpr std::size_t kTags  = 65536UZ;
        constexpr std::size_t kChunk = kSlotsPerSegment / 2UZ;

        SegmentedBuffer<Tag> store(kSlots);
        auto                 writer = store.new_writer();
        auto                 steady = store.new_reader();

        // every tag carries its own position as its index
        auto countMisplaced = [](const auto& span, std::size_t first) {
            std::size_t misplaced = 0UZ;
            for (std::size_t i = 0UZ; i < span.size(); ++i) {
                misplaced += span[i].index == first + i ? 0UZ : 1UZ;
            }
            return misplaced;
        };

        std::atomic<bool>        writerDone{false};
        std::atomic<std::size_t> attachedMisplaced{0UZ};
        std::atomic<std::size_t> attachedRead{0UZ};
        std::atomic<std::size_t> attachments{0UZ};
        std::thread              attacher([&] {
            while (!writerDone.load(std::memory_order_acquire)) {
                auto              reader = store.new_reader();
                const std::size_t first  = reader.position();
                auto              span   = reader.get();
                attachedMisplaced += countMisplaced(span, first);
                attachedRead += span.size();
                ++attachments;
                std::ignore = span.consume(span.size());
            }
        });
        std::size_t              steadyMisplaced = 0UZ;
        std::size_t              steadyRead      = 0UZ;
        std::thread              consumer([&] {
            while (steadyRead < kTags) {
                const std::size_t first = steady.position();
                auto              span  = steady.get();
                steadyMisplaced += countMisplaced(span, first);
                steadyRead += span.size();
                std::ignore = span.consume(span.size());
                if (span.empty()) {
                    std::this_thread::yield();
                }
            }
        });

        for (std::size_t written = 0UZ; written < kTags;) {
            const std::size_t n    = std::min(kChunk, kTags - written);
            auto              span = writer.tryReserve(n);
            if (span.empty()) {
                std::this_thread::yield();
                continue;
            }
            for (std::size_t i = 0UZ; i < n; ++i) {
                span[i].index = written + i;
            }
            span.publish(n);
            written += n;
        }
        consumer.join();
        writerDone.store(true, std::memory_order_release);
        attacher.join();

        expect(eq(steadyRead, kTags)) << "the steady reader reads every tag";
        expect(eq(steadyMisplaced, 0UZ));
        expect(gt(attachments.load(), 0UZ)) << "readers attached while the writer ran";
        expect(eq(attachedMisplaced.load(), 0UZ)) << std::format("attached readers read {} tags, {} of them not at their position", attachedRead.load(), attachedMisplaced.load());
    };

    "a reader that attaches inside a reservation after the last reader left starts at the publish position"_test = [] {
        CountingResource                            tagMemory;
        SegmentedBuffer<Tag>                        store(8UZ * kSlotsPerSegment, &tagMemory);
        auto                                        writer = store.new_writer();
        std::optional<decltype(store.new_reader())> leaving{store.new_reader()}; // holds the first tags while the writer reserves
        writePositioned(writer, 2UZ * kSlotsPerSegment);

        const std::size_t     published = writer.position();
        constexpr std::size_t kReserved = 6UZ * kSlotsPerSegment; // outgrows the first index, so the writer frees inside the reservation
        auto                  reader    = [&] {
            auto span = writer.tryReserve(kReserved);
            expect(fatal(eq(span.size(), kReserved)));
            leaving.reset();
            for (std::size_t i = 0UZ; i < kReserved; ++i) {
                span[i].index = published + i;
            }
            auto attached = store.new_reader();
            span.publish(kReserved);
            return attached;
        }();
        expect(fatal(eq(reader.position(), published))) << "the reader starts at the position published when it attached";
        expect(fatal(ge(tagMemory.segments, segmentsSpanning(published, writer.position())))) << "the segments the reader can read are live";
        expect(readIndexes(reader) == positionsFrom(published, writer.position())) << "the reader reads the reservation's tags at their positions";
    };

    if constexpr (kHooksDeallocation) {
        "a reader that attaches while the writer frees starts where a ring reader starts and reads the same tags"_test = [] {
            struct Attached {
                bool                     ranInRegistration{false};
                std::size_t              start{0UZ};
                std::vector<std::size_t> indexes;
            };
            auto attachWhileTheWriterFrees = [](auto& store) {
                auto writer = store.new_writer();
                auto steady = store.new_reader();
                writePositioned(writer, 2UZ * kSlotsPerSegment);
                std::ignore = readIndexes(steady);

                Attached attached;
                auto     publishAndPass = [&] {
                    attached.ranInRegistration = registersBeside(store, 1UZ);
                    writePositioned(writer, 2UZ * kSlotsPerSegment);
                    std::ignore = readIndexes(steady);
                    writePositioned(writer, kSlotsPerSegment); // this reservation may free what the steady reader passed
                    std::ignore = readIndexes(steady);
                };
                auto reader      = attachRunning(store, publishAndPass);
                attached.start   = reader.position();
                attached.indexes = readIndexes(reader);
                return attached;
            };
            SegmentedBuffer<Tag> segmentedStore(8UZ * kSlotsPerSegment);
            CircularBuffer<Tag>  ringStore(8UZ * kSlotsPerSegment);
            const Attached       segmented = attachWhileTheWriterFrees(segmentedStore);
            const Attached       ring      = attachWhileTheWriterFrees(ringStore);

            expect(fatal(segmented.ranInRegistration && ring.ranInRegistration)) << "the writer ran inside each reader's registration";
            expect(eq(ring.start, 2UZ * kSlotsPerSegment)) << "the ring reader starts at the position published when it registered";
            expect(eq(ring.indexes.size(), 3UZ * kSlotsPerSegment)) << "the ring reader reads the tags the steady reader passed while it registered";
            expect(eq(segmented.start, ring.start));
            expect(segmented.indexes == ring.indexes) << "the segmented reader reads the ring reader's tags";
        };

        "a reader that the writer misses and then sees while it registers starts in a live segment"_test = [] {
            CountingResource     tagMemory;
            SegmentedBuffer<Tag> store(8UZ * kSlotsPerSegment, &tagMemory);
            auto                 writer = store.new_writer();
            auto                 steady = store.new_reader();
            writePositioned(writer, 2UZ * kSlotsPerSegment);
            std::ignore = readIndexes(steady);

            bool ranInRegistration = false;
            auto missThenSee       = [&] {
                ranInRegistration = registersBeside(store, 1UZ);
                writePositioned(writer, 2UZ * kSlotsPerSegment);
                std::ignore = readIndexes(steady);
                writePositioned(writer, kSlotsPerSegment); // this reservation may free what the steady reader passed
                std::ignore = readIndexes(steady);
                {
                    auto passing = store.new_reader(); // the writer reads the reader set anew and finds the registering reader
                }
                writePositioned(writer, kSlotsPerSegment);
                std::ignore = readIndexes(steady);
            };
            auto reader = attachRunning(store, missThenSee);
            expect(fatal(ranInRegistration)) << "the writer ran inside the reader's registration";

            const std::size_t start = reader.position();
            expect(eq(start, 2UZ * kSlotsPerSegment)) << "the reader starts at the position published when it registered";
            expect(fatal(ge(tagMemory.segments, segmentsSpanning(start, writer.position())))) << "the segments the reader can read are live";
            expect(readIndexes(reader) == positionsFrom(start, writer.position()));

            writePositioned(writer, 1UZ);
            expect(eq(tagMemory.segments, 1UZ)) << "the writer frees what both readers passed once the reader is registered";
        };

        "a reader that the writer runs more than size() slots past while it registers reads every tag from live segments"_test = [] {
            CountingResource     tagMemory;
            SegmentedBuffer<Tag> store(8UZ * kSlotsPerSegment, &tagMemory);
            auto                 writer = store.new_writer();
            auto                 steady = store.new_reader();
            writePositioned(writer, 2UZ * kSlotsPerSegment);
            std::ignore = readIndexes(steady);

            bool ranInRegistration = false;
            auto runPast           = [&] {
                ranInRegistration = registersBeside(store, 1UZ);
                for (std::size_t i = 0UZ; i < 10UZ; ++i) { // the claim holds the writer to the steady reader alone
                    writePositioned(writer, kSlotsPerSegment);
                    std::ignore = readIndexes(steady);
                }
            };
            auto reader = attachRunning(store, runPast);
            expect(fatal(ranInRegistration)) << "the writer ran inside the reader's registration";

            const std::size_t start     = reader.position();
            const std::size_t published = writer.position();
            expect(eq(start, 2UZ * kSlotsPerSegment)) << "the reader starts at the position published when it registered";
            expect(fatal(gt(published - start, store.size()))) << "the writer ran more than size() slots past the reader";

            writePositioned(writer, 1UZ); // the claim refuses the slot, after the writer's freeing round
            expect(eq(writer.position(), published)) << "the writer waits for the reader";
            expect(fatal(ge(tagMemory.segments, segmentsSpanning(start, published)))) << "the segments the reader can read are live";
            expect(readIndexes(reader) == positionsFrom(start, published)) << "the reader reads every tag at its position";

            writePositioned(writer, 1UZ);
            expect(eq(tagMemory.segments, 1UZ)) << "the writer frees what both readers passed";
        };

        "two readers that register at once, one inside the other's registration, read every tag from live segments"_test = [] {
            CountingResource     tagMemory;
            SegmentedBuffer<Tag> store(8UZ * kSlotsPerSegment, &tagMemory);
            auto                 writer = store.new_writer();
            auto                 steady = store.new_reader();
            writePositioned(writer, 2UZ * kSlotsPerSegment);
            std::ignore = readIndexes(steady);

            std::optional<decltype(store.new_reader())> inner;
            bool                                        innerRanInRegistration = false;
            bool                                        outerRanInRegistration = false;
            auto                                        duringInner            = [&] {
                innerRanInRegistration = registersBeside(store, 1UZ, 2UZ);
                writePositioned(writer, 2UZ * kSlotsPerSegment);
                std::ignore = readIndexes(steady);
                writePositioned(writer, kSlotsPerSegment); // this reservation may free what the steady reader passed
                std::ignore = readIndexes(steady);
            };
            auto duringOuter = [&] {
                outerRanInRegistration = registersBeside(store, 1UZ);
                inner.emplace(attachRunning(store, duringInner));
                writePositioned(writer, kSlotsPerSegment); // the inner reader is registered, the outer one is not
                std::ignore = readIndexes(steady);
            };
            auto outer = attachRunning(store, duringOuter);
            expect(fatal(outerRanInRegistration && innerRanInRegistration)) << "the writer ran inside both registrations";

            const std::size_t published = writer.position();
            expect(eq(outer.position(), 2UZ * kSlotsPerSegment)) << "the outer reader starts at the position published when it registered";
            expect(eq(inner->position(), 2UZ * kSlotsPerSegment)) << "the inner reader starts at the position published when it registered";
            expect(fatal(ge(tagMemory.segments, segmentsSpanning(std::min(outer.position(), inner->position()), published)))) << "the segments the readers can read are live";
            expect(readIndexes(outer) == positionsFrom(2UZ * kSlotsPerSegment, published)) << "the outer reader reads every tag at its position";
            expect(readIndexes(*inner) == positionsFrom(2UZ * kSlotsPerSegment, published)) << "the inner reader reads every tag at its position";

            writePositioned(writer, 1UZ);
            expect(eq(tagMemory.segments, 1UZ)) << "the writer frees what every reader passed once both are registered";
        };

        "a reader removed while another registers leaves the registering reader's segments live"_test = [] {
            CountingResource                            tagMemory;
            SegmentedBuffer<Tag>                        store(8UZ * kSlotsPerSegment, &tagMemory);
            auto                                        writer = store.new_writer();
            auto                                        steady = store.new_reader();
            std::optional<decltype(store.new_reader())> lagging{store.new_reader()};
            writePositioned(writer, 2UZ * kSlotsPerSegment);
            std::ignore = readIndexes(steady);

            bool ranInRegistration = false;
            auto removeAndPass     = [&] {
                ranInRegistration = registersBeside(store, 2UZ);
                lagging.reset(); // the slowest reader rises from the first position to the steady reader
                writePositioned(writer, 2UZ * kSlotsPerSegment);
                std::ignore = readIndexes(steady);
                writePositioned(writer, kSlotsPerSegment); // this reservation may free what the steady reader passed
                std::ignore = readIndexes(steady);
            };
            auto reader = attachRunning(store, removeAndPass);
            expect(fatal(ranInRegistration)) << "the writer ran inside the reader's registration";

            const std::size_t start     = reader.position();
            const std::size_t published = writer.position();
            expect(eq(start, 2UZ * kSlotsPerSegment)) << "the reader starts at the position published when it registered";
            expect(fatal(ge(tagMemory.segments, segmentsSpanning(start, published)))) << "the segments the reader can read are live";
            expect(readIndexes(reader) == positionsFrom(start, published)) << "the reader reads every tag at its position";

            writePositioned(writer, 1UZ);
            expect(eq(tagMemory.segments, 1UZ)) << "the writer frees what both readers passed";
        };
    }
};

int main() { /* tests are statically executed */ }
