#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <thread>

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
