#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <optional>
#include <ranges>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <gnuradio-4.0/CircularBuffer.hpp>

using CircularBufferSingle = gr::CircularBuffer<std::int32_t, std::dynamic_extent, gr::ProducerType::Single>;
using CircularBufferMulti  = gr::CircularBuffer<std::int32_t, std::dynamic_extent, gr::ProducerType::Multi>;

namespace {

// claims and immediately publishes one chunk, returns false if the claim was refused
template<typename TWriter>
[[nodiscard]] bool writeChunk(TWriter& writer, std::size_t chunkSize) {
    auto span = writer.template tryReserve<gr::SpanReleasePolicy::ProcessAll>(chunkSize);
    return span.size() == chunkSize;
}

template<typename TWriter>
void publishRamp(TWriter& writer, std::size_t nSamples) {
    auto span = writer.template tryReserve<gr::SpanReleasePolicy::ProcessNone>(nSamples);
    std::iota(span.begin(), span.end(), 0);
    span.publish(nSamples);
}

template<typename TBuffer>
void testUngatedWithoutReaders(std::string_view name) {
    using namespace boost::ut;

    TBuffer buffer(1024UZ);
    auto    writer = buffer.new_writer();

    const std::size_t size    = buffer.size();
    const std::size_t chunk   = 64UZ;
    const std::size_t nChunks = 3UZ * size / chunk;

    for (std::size_t i = 0UZ; i < nChunks; ++i) {
        expect(le(writer.available(), size)) << name << ": writer capacity exceeds the buffer size at claim " << i;
        if (!writeChunk(writer, chunk)) {
            expect(false) << name << ": zero-reader buffer refused claim " << i << " of " << nChunks;
            return;
        }
    }
    expect(le(writer.available(), size)) << name << ": writer capacity exceeds the buffer size after the last claim";
}

template<typename TBuffer>
void testGatingResumesWhenReaderAttaches(std::string_view name) {
    using namespace boost::ut;

    TBuffer buffer(1024UZ);
    auto    writer = buffer.new_writer();

    const std::size_t size  = buffer.size();
    const std::size_t chunk = 64UZ;

    for (std::size_t i = 0UZ; i < 3UZ * size / chunk; ++i) {
        if (!writeChunk(writer, chunk)) {
            expect(false) << name << ": zero-reader buffer refused claim " << i;
            return;
        }
    }

    auto reader = buffer.new_reader();
    expect(eq(writer.available(), size)) << name << ": a freshly attached reader must leave the full buffer claimable";

    for (std::size_t i = 0UZ; i < size / chunk; ++i) {
        expect(writeChunk(writer, chunk)) << name << ": claim " << i << " refused although the reader has not consumed";
    }
    expect(eq(writer.available(), 0UZ)) << name << ": an unconsumed full buffer must gate the writer";
    expect(!writeChunk(writer, chunk)) << name << ": a full buffer must refuse further claims";

    auto readSpan = reader.template get<gr::SpanReleasePolicy::ProcessAll>(chunk);
    expect(eq(readSpan.size(), chunk)) << name << ": reader did not see the published samples";
    expect(readSpan.consume(chunk));
}

template<typename TBuffer>
void testReaderDetachUngates(std::string_view name) {
    using namespace boost::ut;

    TBuffer buffer(1024UZ);
    auto    writer = buffer.new_writer();

    const std::size_t size  = buffer.size();
    const std::size_t chunk = 64UZ;

    {
        auto reader = buffer.new_reader();
        for (std::size_t i = 0UZ; i < 3UZ * size / chunk; ++i) {
            expect(writeChunk(writer, chunk)) << name << ": gated claim " << i << " refused although the reader keeps up";
            auto readSpan = reader.template get<gr::SpanReleasePolicy::ProcessAll>(chunk);
            expect(eq(readSpan.size(), chunk)) << name << ": reader did not see chunk " << i;
            expect(readSpan.consume(chunk));
        }
    }

    expect(le(writer.available(), size)) << name << ": capacity underflowed after the last reader detached";
    expect(writeChunk(writer, chunk)) << name << ": writer wedged after the last reader detached";
}

template<typename TBuffer>
void testReaderChurnAgainstProducer(std::string_view name) {
    using namespace boost::ut;

    constexpr std::size_t kChurnCycles = 500UZ;

    TBuffer                  buffer(1024UZ);
    auto                     writer = buffer.new_writer();
    std::atomic<bool>        churnDone{false};
    std::atomic<std::size_t> nPublished{0UZ};

    std::jthread producer([&writer, &churnDone, &nPublished] {
        while (!churnDone.load(std::memory_order_acquire)) {
            if (writeChunk(writer, 64UZ)) {
                nPublished.fetch_add(1UZ, std::memory_order_relaxed);
            }
        }
    });

    // wait for the producer to be live, so the churn overlaps it even on a loaded machine
    for (std::size_t i = 0UZ; i < 5000UZ && nPublished.load(std::memory_order_relaxed) == 0UZ; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (std::size_t cycle = 0UZ; cycle < kChurnCycles; ++cycle) {
        auto              reader    = buffer.new_reader();
        const std::size_t available = reader.available();
        if (available > 0UZ) {
            auto readSpan = reader.template get<gr::SpanReleasePolicy::ProcessAll>(available);
            expect(readSpan.consume(available));
        }
    }
    churnDone.store(true, std::memory_order_release);
    producer.join();

    expect(gt(nPublished.load(), 0UZ)) << name << ": the producer made no progress during the reader churn";
    expect(le(writer.available(), buffer.size())) << name << ": capacity is inconsistent after the churn";
}

} // namespace

const boost::ut::suite<"writer span publish bound"> writerSpanPublishTests = [] {
    using namespace boost::ut;

    "an under-published reservation releases its remainder"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto span = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            expect(eq(span.size(), 64UZ));
            std::ranges::fill(span, 7);
            span.publish(16UZ);
        }
        expect(eq(reader.available(), 16UZ)) << "only the published prefix may reach the reader";
        expect(eq(writer.available(), buffer.size() - 16UZ)) << "the unpublished tail must return to the writer";

        {
            auto span = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(8UZ);
            std::ranges::fill(span, 9);
            span.publish(8UZ);
        }
        expect(eq(reader.available(), 24UZ)) << "the next reservation must continue where the published prefix ended";

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(24UZ);
        expect(std::ranges::all_of(readSpan | std::views::take(16UZ), [](std::int32_t v) { return v == 7; }));
        expect(std::ranges::all_of(readSpan | std::views::drop(16UZ), [](std::int32_t v) { return v == 9; }));
        expect(readSpan.consume(24UZ));
    };

    "publishing past the reservation is clamped, not handed to the reader"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto span = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(32UZ);
            std::ranges::fill(span, 5);
            span.publish(96UZ); // reports on stderr and clamps to 32
            expect(eq(span.nRequestedSamplesToPublish(), 32UZ));
        }
        expect(eq(reader.available(), 32UZ)) << "the reader must never be offered slots the writer did not claim";
        expect(eq(writer.available(), buffer.size() - 32UZ)) << "the writer capacity must stay consistent with the claim";

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(reader.available());
        expect(std::ranges::all_of(readSpan, [](std::int32_t v) { return v == 5; })) << "no unwritten slot may surface as data";
        expect(readSpan.consume(readSpan.size()));
    };

    "an over-published increment cannot accumulate past the reservation"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto span = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(40UZ);
            std::ranges::fill(span, 3);
            span.publish(30UZ);
            span.publish(30UZ); // only 10 remain
            expect(eq(span.nRequestedSamplesToPublish(), 40UZ));
        }
        expect(eq(reader.available(), 40UZ));
    };
};

const boost::ut::suite<"nested writer reservation"> nestedWriterReservationTests = [] {
    using namespace boost::ut;

    "a nested tryReserve is refused and leaves the open reservation intact"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto open = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            std::ranges::fill(open, 7);
            open.publish(16UZ);
            const std::size_t   reserved = open.size();
            const std::int32_t* slots    = open.data();

            {
                auto nested = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(1UZ); // reports on stderr, claims nothing
                expect(eq(nested.size(), 0UZ)) << "a refused reservation must expose no slots";
                expect(nested.empty());
                nested.publish(1UZ);
            }

            expect(eq(open.size(), reserved)) << "the open span must keep its reservation";
            expect(open.data() == slots) << "the open span must keep its slots";
            expect(eq(open.nRequestedSamplesToPublish(), 16UZ)) << "the accumulated publish count must survive the refusal";
            expect(std::ranges::all_of(open, [](std::int32_t v) { return v == 7; })) << "no slot of the open span may be overwritten";
            open.publish(8UZ);
            expect(eq(open.nRequestedSamplesToPublish(), 24UZ));
        }
        expect(eq(writer.position(), 24UZ)) << "the publish cursor must advance by exactly what was published";
        expect(eq(reader.available(), 24UZ));

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(reader.available());
        expect(std::ranges::all_of(readSpan, [](std::int32_t v) { return v == 7; }));
        expect(readSpan.consume(readSpan.size()));
    };

    "a nested blocking reserve is refused rather than waiting on itself"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto open = writer.reserve<gr::SpanReleasePolicy::ProcessNone>(buffer.size());
            std::ranges::fill(open, 3);
            open.publish(32UZ);
            {
                auto nested = writer.reserve<gr::SpanReleasePolicy::ProcessNone>(1UZ);
                expect(nested.empty()) << "a refused blocking reserve must return without claiming";
            }
            expect(eq(open.size(), buffer.size()));
            expect(eq(open.nRequestedSamplesToPublish(), 32UZ));
        }
        expect(eq(reader.available(), 32UZ));

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(reader.available());
        expect(std::ranges::all_of(readSpan, [](std::int32_t v) { return v == 3; }));
        expect(readSpan.consume(readSpan.size()));
    };

    "reservations resume once the open span closes"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto open = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            std::ranges::fill(open, 1);
            open.publish(64UZ);
            auto refused = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(8UZ);
            expect(refused.empty());
        }
        {
            auto again = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(8UZ);
            expect(eq(again.size(), 8UZ)) << "the writer must accept a reservation once no span is live";
            std::ranges::fill(again, 2);
            again.publish(8UZ);
        }
        expect(eq(reader.available(), 72UZ));

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(reader.available());
        expect(std::ranges::all_of(readSpan | std::views::take(64UZ), [](std::int32_t v) { return v == 1; }));
        expect(std::ranges::all_of(readSpan | std::views::drop(64UZ), [](std::int32_t v) { return v == 2; }));
        expect(readSpan.consume(readSpan.size()));
    };

    // the corruption guarded against: a nested claim retargets the open span, and closing it sets the publish
    // cursor from the nested claim's offset, handing the reader ring slots the writer never wrote
    "a nested claim under freed capacity cannot move the publish cursor"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();

        {
            auto primed = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(100UZ);
            std::ranges::fill(primed, 1);
            primed.publish(100UZ);
        }
        {
            auto open = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(writer.available());
            expect(eq(open.size(), buffer.size() - 100UZ));
            std::ranges::fill(open | std::views::take(8UZ), 7);
            open.publish(8UZ);

            {
                auto stale = reader.get<gr::SpanReleasePolicy::ProcessAll>(100UZ); // frees capacity under the open span
                expect(eq(stale.size(), 100UZ));
            }
            expect(gt(writer.available(), 0UZ)) << "the scenario needs capacity to have freed up";

            auto nested = writer.tryReserve<gr::SpanReleasePolicy::ProcessNone>(1UZ);
            expect(nested.empty()) << "freed capacity must not be claimed under an open span";
        }
        expect(eq(writer.position(), 108UZ)) << "the publish cursor must not jump past the open span's claim";
        expect(eq(reader.available(), 8UZ)) << "the reader must see only the published prefix, never unwritten ring slots";

        auto readSpan = reader.get<gr::SpanReleasePolicy::ProcessAll>(reader.available());
        expect(std::ranges::all_of(readSpan, [](std::int32_t v) { return v == 7; })) << "no stale ring slot may surface as data";
        expect(readSpan.consume(readSpan.size()));
    };
};

const boost::ut::suite<"reader over-consume"> readerConsumeBoundTests = [] {
    using namespace boost::ut;

    "a consume beyond the span handed out is refused and leaves the reader intact"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();
        publishRamp(writer, 512UZ);

        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            expect(eq(span.size(), 64UZ));
            expect(!span.consume(100UZ)) << "a consume past the span handed out must be refused";
            expect(eq(reader.position(), 0UZ)) << "a refused consume must not move the read index";
            expect(eq(span.front(), 0)) << "the refused span must stay readable";
            expect(eq(span.back(), 63)) << "the refused span must stay readable";
            expect(span.consume(64UZ)) << "the span must still be consumable after a refusal";
        }
        expect(eq(reader.position(), 64UZ));
        expect(eq(reader.available(), 448UZ)) << "only the samples the reader saw may be retired";

        auto next = reader.get<gr::SpanReleasePolicy::ProcessNone>(8UZ);
        expect(eq(next.front(), 64)) << "the stream must resume at the first sample never handed out";
        expect(next.consume(0UZ));
    };

    "a consume of exactly the span handed out is accepted"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();
        publishRamp(writer, 512UZ);

        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            expect(span.consume(span.size()));
        }
        expect(eq(reader.position(), 64UZ));
        expect(eq(reader.nSamplesConsumed(), 64UZ));
    };

    "the first get bounds the window a later get may consume"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();
        publishRamp(writer, 512UZ);

        {
            auto first  = reader.get<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            auto second = reader.get<gr::SpanReleasePolicy::ProcessNone>(512UZ);
            expect(eq(second.size(), 64UZ)) << "a later get cannot grow the window";
            expect(!second.consume(512UZ)) << "the window the first get opened must still bound the consume";
            expect(eq(reader.position(), 0UZ));
            expect(second.consume(64UZ));
        }
        expect(eq(reader.position(), 64UZ));
    };

    "a zero-sample get retires nothing"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();
        publishRamp(writer, 512UZ);

        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(0UZ);
            expect(eq(span.size(), 0UZ));
            expect(!span.consume(100UZ)) << "a span that showed nothing may retire nothing";
            expect(span.consume(0UZ));
        }
        expect(eq(reader.position(), 0UZ));
        expect(eq(reader.available(), 512UZ)) << "the stream must be untouched";
    };

    "consume(0) stays legal in every state"_test = [] {
        CircularBufferSingle buffer(1024UZ);
        auto                 writer = buffer.new_writer();
        auto                 reader = buffer.new_reader();
        publishRamp(writer, 512UZ);

        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(0UZ);
            expect(span.consume(0UZ)) << "on an empty span";
        }
        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            expect(span.consume(0UZ)) << "on a span that showed samples";
        }
        {
            auto span = reader.get<gr::SpanReleasePolicy::ProcessNone>(64UZ);
            expect(!span.consume(65UZ));
            expect(span.consume(0UZ)) << "after a refused consume";
        }
        expect(eq(reader.position(), 0UZ));
        expect(eq(reader.available(), 512UZ));
    };
};

// every copy of a span shares the parent's claim, and only the release of the last one retires it,
// so a span may be copied but never assigned over -- the overwritten copy would never be released
using ReaderUnderTest     = decltype(std::declval<CircularBufferSingle&>().new_reader());
using WriterUnderTest     = decltype(std::declval<CircularBufferSingle&>().new_writer());
using ReaderSpanUnderTest = decltype(std::declval<ReaderUnderTest&>().get<gr::SpanReleasePolicy::ProcessNone>(0UZ));
using WriterSpanUnderTest = decltype(std::declval<WriterUnderTest&>().reserve<gr::SpanReleasePolicy::ProcessNone>(0UZ));

static_assert(std::is_copy_constructible_v<ReaderSpanUnderTest>);
static_assert(!std::is_copy_assignable_v<ReaderSpanUnderTest>);
static_assert(!std::is_move_assignable_v<ReaderSpanUnderTest>);
static_assert(std::is_copy_constructible_v<WriterSpanUnderTest>);
static_assert(!std::is_copy_assignable_v<WriterSpanUnderTest>);
static_assert(!std::is_move_assignable_v<WriterSpanUnderTest>);

const boost::ut::suite<"buffer zero-reader gating"> bufferZeroReaderTests = [] {
    using namespace boost::ut;

    "a buffer without readers never gates its writer"_test = [] {
        testUngatedWithoutReaders<CircularBufferSingle>("SPSC");
        testUngatedWithoutReaders<CircularBufferMulti>("MPSC");
    };

    "gating resumes when a reader attaches"_test = [] {
        testGatingResumesWhenReaderAttaches<CircularBufferSingle>("SPSC");
        testGatingResumesWhenReaderAttaches<CircularBufferMulti>("MPSC");
    };

    "detaching the last reader ungates instead of wedging"_test = [] {
        testReaderDetachUngates<CircularBufferSingle>("SPSC");
        testReaderDetachUngates<CircularBufferMulti>("MPSC");
    };

    "readers may attach and detach while a producer writes"_test = [] {
        testReaderChurnAgainstProducer<CircularBufferSingle>("SPSC");
        testReaderChurnAgainstProducer<CircularBufferMulti>("MPSC");
    };
};

int main() { /* tests are statically registered */ }
