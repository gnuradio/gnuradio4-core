#include <boost/ut.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

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

} // namespace

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
};

int main() { /* tests are statically registered */ }
