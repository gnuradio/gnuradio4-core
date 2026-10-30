#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
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
 * @brief Every block forwards every tag key.
 *
 * The tests check that:
 * - every key survives, at its offset on a 1:1 block and at its mapped offset on a block declaring `Resampling<>`
 * - every tag of a chunk leaves at the chunk's first output sample under `BackwardTagPropagation`
 * - a tag on a sample two overlapping chunks read leaves once
 * - a key a setting is coupled to carries the value it arrived with, whatever the setting holds
 * - a coupled setting a tag changes hands its value on through the block's own forward map, after the tag, on a 1:1
 *   block and on a block declaring `Resampling<>`, and on the next call when the call that applies the tag publishes
 *   no sample
 * - a tag inside a chunk that an input minimum keeps whole leaves once, at its own offset
 * - a tag in the stream's trailing window keeps its offset
 * - a decimator that consumes less than its window places each tag of the samples it consumed once, in order, and
 *   applies its settings once
 * - a sink with an input minimum reads each tag of the samples it consumes once
 * - a composed block forwards no tag where a member declares `NoTagPropagation` or supplies `forwardTags()`
 *
 * They also check the multi-input dedup, the end-of-stream index and the keys a block lists in `DroppedTagKeys`, a
 * `gr:` key among them.
 *
 * A second suite checks a block declaring `Stride<>`. A tag on a sample it skips leaves at the next chunk's first
 * output sample, and every chunk starts where the stride puts it. The forward map of a tag in the stream's last skip
 * leaves at the end index, after the tag.
 *
 * The blocks are defined here: gnuradio4-core carries no standard block library, so a core test may not depend on one.
 */

namespace qa_uniform_tag_propagation {

using namespace gr;

inline constexpr std::size_t kSamples = 2048UZ;

/// an input minimum above one forbids a chunk boundary at a tag closer than that to the chunk's start
inline constexpr std::size_t kInputMinimum = 4UZ;

inline constexpr std::string_view kReservedKey = "gr:trigger_name"; ///< a gr: key no block here is coupled to
inline constexpr std::string_view kCustomKey   = "record_id";       ///< a default-domain key no setting is named after
inline constexpr std::string_view kOwnedKey    = "gain";            ///< the key a case couples a gain setting to
inline constexpr std::string_view kDroppedKey  = "phase_offset";    ///< listed by the dropping blocks
inline constexpr std::string_view kHalfRateKey = "foo:half_rate";   ///< a key a block writes into its own forward map

inline constexpr float kUpstreamGain = 10.f;
inline constexpr float kUpstreamRate = 96000.f;
inline constexpr float kGainCeiling  = 2.f;
inline constexpr float kRateCeiling  = 50000.f;

/// couples a block's gain to kOwnedKey and its sample_rate to gr:sample_rate
[[nodiscard]] gr::property_map coupledKeys() { return {{"gain", std::string(kOwnedKey)}, {"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}}; }

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

/// forwards every key but the one it lists
struct DroppingRelay : Block<DroppingRelay, DroppedTagKeys<"phase_offset">> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(DroppingRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// lists the carrier estimates, as a block that alters the carrier frequency or phase does
struct CarrierEstimateDropper : Block<CarrierEstimateDropper, DroppedTagKeys<"freq_est", "phase_est">> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(CarrierEstimateDropper, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
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

/// its own forwardTags() publishes every tag of the chunk at the offset it arrived at, and skips a tag an earlier chunk
/// handed it, at a negative relIndex
struct ChunkForwarder : Block<ChunkForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(ChunkForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        gr::for_each_reader_span(
            [&outputSpans, processedIn](auto& inSpan) {
                for (const auto& [relIndex, tagMapRef] : inSpan.tags(processedIn)) {
                    if (relIndex < 0) {
                        continue;
                    }
                    const auto offset = static_cast<std::size_t>(relIndex);
                    gr::for_each_writer_span([&tagMapRef, offset](auto& outSpan) { outSpan.publishTag(tagMapRef.get(), offset); }, outputSpans);
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

/// a processOne block whose own forwardTags() places each tag one output sample later, as a one-sample delay would
struct DelayingForwarder : Block<DelayingForwarder> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(DelayingForwarder, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        gr::for_each_reader_span(
            [&outputSpans, processedIn](auto& inSpan) {
                for (const auto& [relIndex, tagMapRef] : inSpan.tags(processedIn)) {
                    const std::size_t offset = static_cast<std::size_t>(std::max(std::ptrdiff_t(0), relIndex)) + 1UZ;
                    gr::for_each_writer_span([&tagMapRef, offset](auto& outSpan) { outSpan.publishTag(tagMapRef.get(), std::min(offset, outSpan.size())); }, outputSpans);
                }
            },
            inputSpans);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// an identity block whose trailing samples, and the tags among them, arrive through processEpilogue
struct TailRelay : Block<TailRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(TailRelay, in, out);

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

/// a 2:1 decimator that consumes one input chunk per call, fewer than its window holds
struct OneChunkDecimator : Block<OneChunkDecimator, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(OneChunkDecimator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (inSpan.size() < 2UZ || outSpan.empty()) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(2UZ);
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

/// a sink that consumes at most two samples per call and records the tags of the samples it consumes
struct TwoSampleSink : Block<TwoSampleSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(TwoSampleSink, in);

    std::vector<TagRecord> tags;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = std::min(2UZ, inSpan.size());
        for (const auto& [relIndex, tagMapRef] : inSpan.tags(n)) {
            tags.push_back(TagRecord{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(inSpan.streamIndex) + relIndex), tagMapRef.get()});
        }
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// records the tags its work calls read and, in its epilogue, the tags left in its ring
struct RecordingSink : Block<RecordingSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RecordingSink, in);

    std::size_t            nSamples = 0UZ;
    std::vector<TagRecord> tags;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        const std::size_t n = inSpan.size();
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        inSpan.consumeTags(n);
        nSamples += n;
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan) {
        for (const Tag& tag : in.tagReader().get()) {
            tags.push_back(TagRecord{tag.index, tag.map});
        }
        nSamples += inSpan.size();
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

/// caps both of its settings, so a setting coupled to a key holds a value other than the one the tag carried
struct CappingRelay : Block<CappingRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate     = 1000.f;
    float gain            = 1.f;
    float max_gain        = kGainCeiling;
    float max_sample_rate = kRateCeiling;

    GR_MAKE_REFLECTABLE(CappingRelay, in, out, sample_rate, gain, max_gain, max_sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) {
        gain        = std::min(gain, max_gain);
        sample_rate = std::min(sample_rate, max_sample_rate);
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// writes half of a changed sample rate into a block's forward map under kHalfRateKey
void forwardHalfRate(const property_map& newSettings, float sampleRate, property_map& forward) {
    if (newSettings.contains("sample_rate")) {
        forward.insert_or_assign(std::pmr::string(kHalfRateKey), sampleRate / 2.f);
    }
}

/// hands half its sample rate on under kHalfRateKey through its own forward map
struct HalfRateRelay : Block<HalfRateRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1.f;

    GR_MAKE_REFLECTABLE(HalfRateRelay, in, out, sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings, property_map& forward) { forwardHalfRate(newSettings, sample_rate, forward); }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// HalfRateRelay as a processBulk() block that consumes and publishes nothing on its first call
struct WaitingHalfRateRelay : Block<WaitingHalfRateRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1.f;

    GR_MAKE_REFLECTABLE(WaitingHalfRateRelay, in, out, sample_rate);

    bool _waited = false;

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings, property_map& forward) { forwardHalfRate(newSettings, sample_rate, forward); }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (!_waited) {
            _waited     = true;
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        std::ignore = inSpan.consume(n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// HalfRateRelay's forward map on a 2:1 decimator
struct HalfRateDecimator : Block<HalfRateDecimator, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1.f;

    GR_MAKE_REFLECTABLE(HalfRateDecimator, in, out, sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings, property_map& forward) { forwardHalfRate(newSettings, sample_rate, forward); }

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

/// lists a gr: key in DroppedTagKeys
struct ReservedKeyDropper : Block<ReservedKeyDropper, DroppedTagKeys<"gr:trigger_name">> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(ReservedKeyDropper, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a 2:1 decimator that consumes one input chunk per call and counts its resets
struct ResetCountingDecimator : Block<ResetCountingDecimator, Resampling<2U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(ResetCountingDecimator, in, out);

    std::size_t nResets = 0UZ;

    void reset() { ++nResets; }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (inSpan.size() < 2UZ || outSpan.empty()) {
            std::ignore = inSpan.consume(0UZ);
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        std::ignore  = inSpan.consume(2UZ);
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

/// has a gain and a sample_rate setting that no case couples to a tag key
struct UnmarkedRelay : Block<UnmarkedRelay> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1000.f;
    float gain        = 1.f;

    GR_MAKE_REFLECTABLE(UnmarkedRelay, in, out, sample_rate, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// keeps the first sample of every four
struct QuarterDecimator : Block<QuarterDecimator, Resampling<4U, 1U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(QuarterDecimator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), inSpan.size() / 4UZ);
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = inSpan[4UZ * i];
        }
        std::ignore = inSpan.consume(4UZ * n);
        outSpan.publish(n);
        return work::Status::OK;
    }
};

/// repeats every sample three times
struct TripleInterpolator : Block<TripleInterpolator, Resampling<1U, 3U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(TripleInterpolator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size() / 3UZ, inSpan.size());
        for (std::size_t i = 0UZ; i < 3UZ * n; ++i) {
            outSpan[i] = inSpan[i / 3UZ];
        }
        std::ignore = inSpan.consume(n);
        outSpan.publish(3UZ * n);
        return work::Status::OK;
    }
};

/// reads four samples a chunk and steps two, so each sample after the first two is read by two chunks
struct OverlappingDecimator : Block<OverlappingDecimator, Resampling<4U, 1U, true>, Stride<2U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(OverlappingDecimator, in, out);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (inSpan.empty() || outSpan.empty()) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

struct Strided : Block<Strided, Stride<2U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(Strided, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// reads four samples, then skips six. Each output sample is the first sample of its chunk, which on a ramp is the
/// chunk's stream index. The block caps the rate it is fed.
struct SkippingDecimator : Block<SkippingDecimator, Resampling<4U, 1U, true>, Stride<10U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate     = 1000.f;
    float max_sample_rate = kRateCeiling;

    GR_MAKE_REFLECTABLE(SkippingDecimator, in, out, sample_rate, max_sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { sample_rate = std::min(sample_rate, max_sample_rate); }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (inSpan.empty() || outSpan.empty()) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

/// the frames of SkippingDecimator, handing half its sample rate on through its own forward map
struct SkippingHalfRateDecimator : Block<SkippingHalfRateDecimator, Resampling<4U, 1U, true>, Stride<10U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    float sample_rate = 1.f;

    GR_MAKE_REFLECTABLE(SkippingHalfRateDecimator, in, out, sample_rate);

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings, property_map& forward) { forwardHalfRate(newSettings, sample_rate, forward); }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        if (inSpan.empty() || outSpan.empty()) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

/// the frames of SkippingDecimator, with a forwardTags() that republishes every tag at relIndex <= 0 whole
struct SkippingForwarder : Block<SkippingForwarder, Resampling<4U, 1U, true>, Stride<10U, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(SkippingForwarder, in, out);

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
        if (inSpan.empty() || outSpan.empty()) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        outSpan[0UZ] = inSpan[0UZ];
        outSpan.publish(1UZ);
        return work::Status::OK;
    }
};

template<typename TPolicy>
struct PolicyRelay : Block<PolicyRelay<TPolicy>, TPolicy> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(PolicyRelay, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// passes its input through, and takes the previous output on a second input inside a feedback merge
struct FeedbackRelay : Block<FeedbackRelay> {
    PortIn<float>  in;
    PortIn<float>  previous;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(FeedbackRelay, in, previous, out);

    [[nodiscard]] constexpr float processOne(float value, float /*previousOutput*/) const noexcept { return value; }
};

using SilentRelay       = PolicyRelay<NoTagPropagation>;
using MergedPlain       = MergeByIndex<Relay, 0UZ, Relay, 0UZ>;
using MergedSilent      = MergeByIndex<Relay, 0UZ, SilentRelay, 0UZ>;
using SplitPlain        = SplitMergeCombine<Relay, Relay>;
using SplitSilent       = SplitMergeCombine<Relay, SilentRelay>;
using FeedbackPlain     = FeedbackMergeByIndex<FeedbackRelay, 0UZ, Relay, 0UZ, 1UZ, void>;
using FeedbackSilent    = FeedbackMergeByIndex<FeedbackRelay, 0UZ, SilentRelay, 0UZ, 1UZ, void>;
using FeedbackMonitored = FeedbackMergeByIndex<FeedbackRelay, 0UZ, Relay, 0UZ, 1UZ, SilentRelay>;
using MergedDropping    = MergeByIndex<Relay, 0UZ, DroppingRelay, 0UZ>;
using MergedForwarding  = MergeByIndex<Relay, 0UZ, DelayingForwarder, 0UZ>;

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

// source -> TMiddle... -> sink, emplaced and connected in that order. firstCouplings is the first TMiddle's tag coupling table.
template<typename... TMiddle>
[[nodiscard]] Sink* buildSeries(gr::Graph& flow, const std::vector<TagRecord>& tags, const std::array<gr::property_map, sizeof...(TMiddle)>& middleSettings = {}, const gr::property_map& firstCouplings = {}) {
    using namespace boost::ut;

    auto& source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;

    // a braced initializer fixes the emplacement order
    std::tuple<TMiddle&...> middle = [&]<std::size_t... I>(std::index_sequence<I...>) { return std::tuple<TMiddle&...>{flow.emplaceBlock<TMiddle>(middleSettings[I])...}; }(std::index_sequence_for<TMiddle...>{});
    auto&                   sink   = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    if (!firstCouplings.empty()) {
        std::get<0UZ>(middle).settings().setTagCouplings(firstCouplings);
    }

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

    auto& first          = flow.emplaceBlock<Relay>(gr::property_map{{"name", std::string("m0")}});
    auto& second         = flow.emplaceBlock<Relay>(gr::property_map{{"name", std::string("m1")}});
    auto& third          = flow.emplaceBlock<Relay>(gr::property_map{{"name", std::string("m2")}});
    first.in.min_samples = minSamples;

    auto&      sink = flow.emplaceBlock<Sink>(gr::property_map{{"name", std::string("snk")}});
    const auto link = [&flow](auto& from, auto& to) { expect(flow.connect<"out", "in">(from, to).has_value()); };
    link(source, first);
    link(first, second);
    link(second, third);
    link(third, sink);
    return std::addressof(sink);
}

/// what a recording sink read: its sample count and every tag, in arrival order
struct RecordedRun {
    std::size_t            nSamples{};
    std::vector<TagRecord> tags;
};

/// source -> TMiddle -> recording sink, with the input window set on TMiddle where minSamples is above zero
template<typename TMiddle>
[[nodiscard]] RecordedRun runRecorded(const std::vector<TagRecord>& tags, std::size_t nTotal, std::size_t minSamples, std::size_t maxSamples) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source  = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;
    source.nTotal     = nTotal;
    auto& middle      = flow.emplaceBlock<TMiddle>(gr::property_map{{"name", std::string("mid")}});
    if (minSamples > 0UZ) {
        middle.in.min_samples = minSamples;
        middle.in.max_samples = maxSamples;
    }
    auto& sink = flow.emplaceBlock<RecordingSink>(gr::property_map{{"name", std::string("snk")}});
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());
    return RecordedRun{.nSamples = sink.nSamples, .tags = sink.tags};
}

/// what an end sink recorded
struct EndResult {
    std::size_t            nSamples{};
    std::vector<TagRecord> endTags;
};

/// source -> TMiddle -> end sink, with `tags` published by the source and `couplings` as TMiddle's tag coupling table
template<typename TMiddle>
[[nodiscard]] EndResult runToEnd(const std::vector<TagRecord>& tags, const gr::property_map& couplings = {}) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source  = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
    source.tagsToEmit = tags;
    auto& middle      = flow.emplaceBlock<TMiddle>(gr::property_map{{"name", std::string("mid")}});
    auto& sink        = flow.emplaceBlock<EndSink>(gr::property_map{{"name", std::string("snk")}});
    if (!couplings.empty()) {
        middle.settings().setTagCouplings(couplings);
    }
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return EndResult{sink.nSamples, sink.endTags};
}

// a composition of blocks that all forward every key, against the same composition with one member that the composed
// block cannot forward through
template<typename TPlain, typename TSilent>
void expectComposedSilenceRule(std::string_view composition) {
    using namespace boost::ut;

    const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
    const RunResult              plain  = runOnce([&tags](gr::Graph& flow) { return buildSeries<TPlain>(flow, tags); });
    const RunResult              silent = runOnce([&tags](gr::Graph& flow) { return buildSeries<TSilent>(flow, tags); });

    const std::vector<std::size_t> offsets{tags[0].index, tags[1].index};
    expect(indicesCarrying(plain.tags, kCustomKey) == offsets) << std::format("{}: members forwarding every key compose into a block forwarding every key", composition);
    expect(carrying(silent.tags, kCustomKey).empty() && carrying(silent.tags, kReservedKey).empty()) << std::format("{}: a tag crosses the composed block", composition);
}

void expectTagIndices(std::string_view scenario, const std::vector<TagRecord>& tags, const std::vector<std::size_t>& expected) {
    using namespace boost::ut;

    const std::vector<std::size_t> actual = indicesCarrying(tags, kCustomKey);
    expect(eq(actual.size(), expected.size())) << std::format("{}: {} tags carrying {}, expected {}", scenario, actual.size(), kCustomKey, expected.size());
    for (std::size_t i = 0UZ; i < std::min(actual.size(), expected.size()); ++i) {
        expect(eq(actual[i], expected[i])) << std::format("{}: tag {} arrived at output index {}, expected {}", scenario, i, actual[i], expected[i]);
    }
}

/// each sent tag arrives once, at its own index or a later one, and the sink reads every tag in index order
void expectOnceInOrder(std::string_view scenario, const std::vector<TagRecord>& tags, const std::vector<std::size_t>& sent) {
    using namespace boost::ut;

    const std::vector<std::size_t> actual = indicesCarrying(tags, kCustomKey);
    expect(eq(actual.size(), sent.size())) << std::format("{}: {} tags carrying {}, sent {}", scenario, actual.size(), kCustomKey, sent.size());
    for (std::size_t i = 0UZ; i < std::min(actual.size(), sent.size()); ++i) {
        expect(ge(actual[i], sent[i])) << std::format("{}: tag {} sent at {} arrived at output index {}", scenario, i, sent[i], actual[i]);
    }
    expect(std::ranges::is_sorted(tags, std::ranges::less{}, &TagRecord::index)) << std::format("{}: the sink reads the tags in index order", scenario);
}

/// a strided block fed a ramp of nTotal samples emits one sample per chunk, and chunk k starts at sample k * stride
template<typename TStrided>
void expectFramesOnStride(std::string_view scenario, const std::vector<float>& samples, std::size_t nTotal) {
    using namespace boost::ut;

    constexpr std::size_t kChunk  = TStrided::ResamplingControl::kInputChunkSize;
    constexpr std::size_t kStep   = TStrided::StrideControl::kStride;
    const std::size_t     nFrames = (nTotal - kChunk) / kStep + 1UZ;
    expect(eq(samples.size(), nFrames)) << std::format("{}: one output sample per chunk", scenario);
    const std::size_t nCompared = std::min(samples.size(), nFrames);
    std::size_t       onStride  = 0UZ; // the chunks before the first one off the stride
    while (onStride < nCompared && samples[onStride] == static_cast<float>(onStride * kStep)) {
        ++onStride;
    }
    expect(eq(onStride, nCompared)) << std::format("{}: every chunk starts a stride after the one before", scenario);
}

/// two gr:sample_rate tags cross TRelay, coupled to them. Each change leaves TRelay's forward map once, at the output
/// sample its tag's input sample maps to, and after that tag.
template<typename TRelay>
void expectHalfRateAfterEachTag(std::string_view scenario) {
    using namespace boost::ut;

    constexpr std::size_t        kIn  = TRelay::ResamplingControl::kInputChunkSize;
    constexpr std::size_t        kOut = TRelay::ResamplingControl::kOutputChunkSize;
    const std::array<float, 2UZ> rates{1000.f, 3000.f};
    std::vector<TagRecord>       tags;
    for (std::size_t i = 0UZ; i < rates.size(); ++i) {
        property_map map;
        gr::tag::put(map, gr::tag::SAMPLE_RATE, rates[i]);
        tags.push_back(TagRecord{i * 700UZ, map});
    }
    const property_map couplings{{"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}};
    const RunResult    result = runOnce([&](gr::Graph& flow) { return buildSeries<TRelay>(flow, tags, {}, couplings); });

    const std::vector<TagRecord> halves = carrying(result.tags, kHalfRateKey);
    expect(eq(halves.size(), tags.size())) << std::format("{}: each tag-driven change of the rate leaves the block's forward map once", scenario);
    for (std::size_t i = 0UZ; i < std::min(halves.size(), tags.size()); ++i) {
        expect(eq(halves[i].index, tags[i].index * kOut / kIn)) << std::format("{}: the forward map of change {} leaves at the sample its tag maps to", scenario, i);
        const pmt::Value* half = valueOf(halves[i].map, kHalfRateKey);
        expect(half != nullptr && eq(half->value_or<float>(0.f), rates[i] / 2.f)) << std::format("{}: the forward map of change {} carries half the tag's rate", scenario, i);
    }
    const std::vector<TagRecord> passed = carrying(result.tags, gr::tag::SAMPLE_RATE.key());
    expect(eq(passed.size(), tags.size())) << std::format("{}: the tags themselves cross the block", scenario);
    for (std::size_t i = 0UZ; i < std::min(passed.size(), tags.size()); ++i) {
        const auto atTag = std::ranges::find_if(result.tags, [&](const TagRecord& record) { return record.index == passed[i].index && record.map.contains(kHalfRateKey); });
        const auto tagAt = std::ranges::find_if(result.tags, [&](const TagRecord& record) { return record.index == passed[i].index && record.map.contains(gr::tag::SAMPLE_RATE.key()); });
        expect(tagAt <= atTag) << std::format("{}: the forward map of change {} precedes its tag", scenario, i);
    }
}

/// the output index at which SkippingDecimator forwards a tag on a skipped sample
[[nodiscard]] constexpr std::size_t nextChunkOutput(std::size_t inputIndex) noexcept { return inputIndex / SkippingDecimator::StrideControl::kStride + 1UZ; }

} // namespace qa_uniform_tag_propagation

const boost::ut::suite<"uniform tag propagation"> _uniformTagPropagation = [] {
    using namespace boost::ut;
    using namespace qa_uniform_tag_propagation;

    static_assert(ConstrainedForwarder::hasForwardTagsOverride(), "the probe passes the work path's own span tuples");
    static_assert(ChunkForwarder::hasForwardTagsOverride() && !Relay::hasForwardTagsOverride());
    static_assert(!MergedPlain::noTagPropagation && MergedSilent::noTagPropagation, "a merge forwards no tag where a member forwards none");
    static_assert(!SplitPlain::noTagPropagation && SplitSilent::noTagPropagation);
    static_assert(!FeedbackPlain::noTagPropagation && FeedbackSilent::noTagPropagation && FeedbackMonitored::noTagPropagation);
    static_assert(std::ranges::equal(MergedDropping::DroppedTagKeysControl::kKeys, std::array<std::string_view, 1UZ>{kDroppedKey}));
    static_assert(MergedPlain::DroppedTagKeysControl::kKeys.empty() && std::is_same_v<MergedPlain::base_t, Block<MergedPlain>>, "a merge of members listing no key declares no list");

    "a block forwards every key with its value and offset intact"_test = [] {
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

    "a block with an input minimum forwards a tag inside its first chunk once, at its own offset"_test = [] {
        constexpr std::size_t        kTotal      = 64UZ;
        constexpr std::size_t        kAt         = 1UZ; // closer to the chunk start than the input minimum, so no boundary can fall on it
        constexpr std::size_t        kMaxSamples = 7UZ; // the first call reads this many samples, the source's first publish
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};

        const auto windowed = [&tags](std::size_t minSamples) { return [&tags, minSamples](gr::Graph& flow) { return buildWindowed<BulkRelay>(flow, tags, kTotal, minSamples, kMaxSamples); }; };
        expectTagIndices("input minimum of one, the chunk breaks at the tag", runOnce(windowed(1UZ)).tags, {kAt});
        expectTagIndices(std::format("input minimum of {}", kInputMinimum), runOnce(windowed(kInputMinimum)).tags, {kAt});
        expectTagIndices(std::format("processOne block, input minimum of {}", kInputMinimum), runOnce([&tags](gr::Graph& flow) { return buildWindowed<Relay>(flow, tags, kTotal, kInputMinimum, kMaxSamples); }).tags, {kAt});
    };

    "a block declaring Resampling<> places each tag on the output sample its input sample maps to"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              halved = runOnce([&tags](gr::Graph& flow) { return buildSeries<HalvingDecimator>(flow, tags); });
        expect(indicesCarrying(halved.tags, kCustomKey) == std::vector<std::size_t>{tags[0].index / 2UZ, tags[1].index / 2UZ}) << "a custom key does not cross a decimator at the decimated offset";

        constexpr std::size_t        kInterior = 6UZ; // inside the second chunk of four
        const std::vector<TagRecord> interior{TagRecord{kInterior, protocolTag("r0")}};
        const RunResult              quartered = runOnce([&interior](gr::Graph& flow) { return buildSeries<QuarterDecimator>(flow, interior); });
        expectTagIndices("4:1 decimator, a tag inside a chunk", quartered.tags, {kInterior / 4UZ});

        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 2UZ; // inside the first chunk, which the input minimum keeps whole
        const std::vector<TagRecord> early{TagRecord{kAt, protocolTag("r0")}};
        const RunResult              tripled = runOnce([&early](gr::Graph& flow) { return buildWindowed<TripleInterpolator>(flow, early, kTotal, kInputMinimum, 8UZ); });
        expectTagIndices("1:3 interpolator, a tag inside a chunk", tripled.tags, {3UZ * kAt});
    };

    "a block declaring BackwardTagPropagation places every tag of a chunk on its first output sample"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 2UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<PolicyRelay<BackwardTagPropagation>>(flow, tags, kTotal, kInputMinimum, 8UZ); });
        expectTagIndices("BackwardTagPropagation, a tag inside a chunk", result.tags, {0UZ});
    };

    "a block with an overlapping stride forwards a tag on a sample two chunks read once"_test = [] {
        constexpr std::size_t        kAt = 3UZ; // read by the chunk at 0 and by the chunk at 2
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<OverlappingDecimator>(flow, tags); });
        // the chunk that consumes the sample publishes the output sample with the chunk's own index
        expectTagIndices("4:1 decimator stepping two", result.tags, {kAt / OverlappingDecimator::StrideControl::kStride});
    };

    "a block declaring Stride<> forwards every key"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Strided>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), 1UZ)) << "a custom key does not cross a strided block";
    };

    "a block with an asynchronous output forwards every key"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<AsyncOutputRelay>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), tags.size())) << "a custom key does not cross the block";
    };

    "a block supplying forwardTags() that skips a negative relIndex forwards a tag inside a chunk once, at its offset"_test = [] {
        constexpr std::size_t        kTotal = 64UZ;
        constexpr std::size_t        kAt    = 1UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("r0")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<ChunkForwarder>(flow, tags, kTotal, kInputMinimum, 7UZ); });
        expectTagIndices("forwardTags() override, a tag inside a chunk", result.tags, {kAt});
    };

    "a compile-time merge forwards no tag where a member forwards none"_test = [] { expectComposedSilenceRule<MergedPlain, MergedSilent>("MergeByIndex"); };

    "a compile-time merge forwards no tag where a member supplies forwardTags()"_test = [] { expectComposedSilenceRule<MergedPlain, MergedForwarding>("MergeByIndex with a forwardTags() member"); };

    "a split-merge-combine forwards no tag where a path forwards none"_test = [] { expectComposedSilenceRule<SplitPlain, SplitSilent>("SplitMergeCombine"); };

    "a feedback merge forwards no tag where its feedback block forwards none"_test = [] { expectComposedSilenceRule<FeedbackPlain, FeedbackSilent>("FeedbackMergeByIndex"); };

    "a feedback merge forwards no tag where its monitor forwards none"_test = [] { expectComposedSilenceRule<FeedbackPlain, FeedbackMonitored>("FeedbackMergeByIndex with a monitor"); };

    "the default policy carries the transmit burst keys through blocks that declare nothing about them"_test = [] {
        constexpr std::size_t   kBurstFirst  = 100UZ;
        constexpr std::size_t   kBurstLast   = 899UZ;
        constexpr std::uint64_t kBurstTimeNs = 1'720'000'000'123'456'789ULL;

        property_map burstStart;
        gr::tag::put(burstStart, gr::tag::TX_SOB, true);
        gr::tag::put(burstStart, gr::tag::TX_TIME, kBurstTimeNs);
        property_map burstEnd;
        gr::tag::put(burstEnd, gr::tag::TX_EOB, true);
        const std::vector<TagRecord> tags{TagRecord{kBurstFirst, burstStart}, TagRecord{kBurstLast, burstEnd}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); });

        expect(indicesCarrying(result.tags, gr::tag::TX_SOB.key()) == std::vector{kBurstFirst}) << "tx_sob stays on the burst's first sample";
        expect(indicesCarrying(result.tags, gr::tag::TX_TIME.key()) == std::vector{kBurstFirst}) << "tx_time stays on the burst's first sample";
        expect(indicesCarrying(result.tags, gr::tag::TX_EOB.key()) == std::vector{kBurstLast}) << "tx_eob stays on the burst's last sample";

        for (const TagRecord& record : result.tags) {
            if (const pmt::Value* start = valueOf(record.map, gr::tag::TX_SOB.key()); start != nullptr) {
                expect(start->holds<bool>() && *start->get_if<bool>()) << "tx_sob keeps its type and value";
            }
            if (const pmt::Value* time = valueOf(record.map, gr::tag::TX_TIME.key()); time != nullptr) {
                expect(time->holds<std::uint64_t>() && *time->get_if<std::uint64_t>() == kBurstTimeNs) << "tx_time keeps its type and value";
            }
            if (const pmt::Value* end = valueOf(record.map, gr::tag::TX_EOB.key()); end != nullptr) {
                expect(end->holds<bool>() && *end->get_if<bool>()) << "tx_eob keeps its type and value";
            }
        }
    };

    "a block listing DroppedTagKeys drops that key and forwards every other key at its offset"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kDroppedKey, 0.5f);
        const std::vector<TagRecord>   tags{TagRecord{0UZ, map}, TagRecord{700UZ, map}};
        const std::vector<std::size_t> offsets{tags[0].index, tags[1].index};

        const RunResult plain = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay>(flow, tags); });
        expect(indicesCarrying(plain.tags, kDroppedKey) == offsets) << "the key crosses a block that lists nothing";

        const RunResult dropping = runOnce([&tags](gr::Graph& flow) { return buildSeries<DroppingRelay, Relay>(flow, tags); });
        expect(carrying(dropping.tags, kDroppedKey).empty()) << "the listed key stops at the block";
        expect(indicesCarrying(dropping.tags, kCustomKey) == offsets) << "a custom key the block does not list crosses at its offset";
        expect(indicesCarrying(dropping.tags, kReservedKey) == offsets) << "the reserved key crosses at its offset";
    };

    "a block listing a gr: key in DroppedTagKeys drops that key and forwards the bare name"_test = [] {
        const std::string_view bareKey = gr::tag::nameOf(kReservedKey);
        property_map           map     = protocolTag("r0");
        gr::tag::put(map, bareKey, std::string("bare"));
        const std::vector<TagRecord>   tags{TagRecord{0UZ, map}, TagRecord{700UZ, map}};
        const std::vector<std::size_t> offsets{tags[0].index, tags[1].index};

        const RunResult dropping = runOnce([&tags](gr::Graph& flow) { return buildSeries<ReservedKeyDropper, Relay>(flow, tags); });
        expect(carrying(dropping.tags, kReservedKey).empty()) << "the listed gr: key stops at the block";
        expect(indicesCarrying(dropping.tags, bareKey) == offsets) << "the bare name of the listed key crosses at its offset";
        expect(indicesCarrying(dropping.tags, kCustomKey) == offsets) << "a key the block does not list crosses at its offset";
    };

    "a compile-time merge drops the keys its members list"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kDroppedKey, 0.5f);
        const std::vector<TagRecord> tags{TagRecord{0UZ, map}, TagRecord{700UZ, map}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<MergedDropping>(flow, tags); });
        expect(carrying(result.tags, kDroppedKey).empty()) << "a key one member lists stops at the composed block";
        expect(indicesCarrying(result.tags, kCustomKey) == std::vector<std::size_t>{tags[0].index, tags[1].index}) << "the other keys cross the composed block at their offsets";
    };

    "a custom key crosses a chain of blocks with its value and offset intact"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); });

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

    "a key a setting is coupled to crosses with the value it arrived with, not the setting's value"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);
        gr::tag::put(map, gr::tag::SAMPLE_RATE, kUpstreamRate);

        const std::vector<TagRecord> tags{TagRecord{0UZ, map}};
        const RunResult              result = runOnce([&tags](gr::Graph& flow) { return buildSeries<CappingRelay, Relay>(flow, tags, {}, coupledKeys()); });

        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(eq(carried.size(), 1UZ));
        if (carried.empty()) {
            return;
        }
        const property_map& arrived = carried.front().map;

        const pmt::Value* owned = valueOf(arrived, kOwnedKey);
        expect(owned != nullptr) << "a coupled key does not cross the block";
        if (owned != nullptr) {
            expect(eq(owned->value_or<float>(0.f), kUpstreamGain)) << "the block's capped gain replaces the value the tag carried";
        }

        const pmt::Value* rate = valueOf(arrived, gr::tag::SAMPLE_RATE.key());
        expect(rate != nullptr);
        if (rate != nullptr) {
            expect(eq(rate->value_or<float>(0.f), kUpstreamRate)) << "the block's capped rate replaces the value the tag carried";
        }

        const pmt::Value* custom = valueOf(arrived, kCustomKey);
        expect(custom != nullptr);
        if (custom != nullptr) {
            expect(eq(custom->value_or(std::string_view{}), std::string_view("r0"))) << "a key no block owns passes through unchanged";
        }
    };

    "a coupled setting a tag changes hands its value on through the block's own forward map, after the tag"_test = [] { expectHalfRateAfterEachTag<HalfRateRelay>("1:1 block"); };

    "a forward map leaves on the next call when the call that fills it publishes no sample"_test = [] { expectHalfRateAfterEachTag<WaitingHalfRateRelay>("processBulk() block that waits on its first call"); };

    "a block declaring Resampling<> hands a tag-driven change on through its own forward map, after the tag"_test = [] { expectHalfRateAfterEachTag<HalfRateDecimator>("2:1 decimator"); };

    "a key a block's setting is named after but does not follow crosses with the value it arrived with"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);
        gr::tag::put(map, gr::tag::SAMPLE_RATE, kUpstreamRate);

        const std::vector<TagRecord>            tags{TagRecord{0UZ, map}};
        const std::array<gr::property_map, 2UZ> set{gr::property_map{{"gain", kUpstreamGain / 2.f}, {"sample_rate", kUpstreamRate / 2.f}}, gr::property_map{}};
        const RunResult                         result = runOnce([&tags, &set](gr::Graph& flow) { return buildSeries<UnmarkedRelay, Relay>(flow, tags, set); });

        const std::vector<TagRecord> carried = carrying(result.tags, kCustomKey);
        expect(fatal(eq(carried.size(), 1UZ)));
        const pmt::Value* gain = valueOf(carried.front().map, kOwnedKey);
        const pmt::Value* rate = valueOf(carried.front().map, gr::tag::SAMPLE_RATE.key());
        expect(fatal(gain != nullptr && rate != nullptr)) << "a key no setting follows does not cross";
        expect(eq(gain->value_or<float>(0.f), kUpstreamGain)) << "the block's gain replaces the value of a key it does not follow";
        expect(eq(rate->value_or<float>(0.f), kUpstreamRate)) << "the block's rate replaces the value of a key it does not follow";
    };

    "a key named after a setting Block<> declares for every block is not substituted"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, "name", 7.f); // the upstream protocol's own use of the name, not a block name

        const std::vector<TagRecord> tags{TagRecord{0UZ, map}};
        const RunResult              forwarded = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); });

        const std::vector<TagRecord> carried = carrying(forwarded.tags, kCustomKey);
        expect(eq(carried.size(), 1UZ));
        if (!carried.empty()) {
            const pmt::Value* name = valueOf(carried.front().map, "name");
            expect(name != nullptr) << "the key still crosses the block";
            if (name != nullptr) {
                expect(name->get_if<float>() != nullptr) << "the upstream value, not the block's own name";
                expect(eq(name->value_or<float>(0.f), 7.f));
            }
        }
    };

    "a coupled key crosses a chain of two capping blocks with the value it arrived with"_test = [] {
        property_map map = protocolTag("r0");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);

        const std::vector<TagRecord>            tags{TagRecord{0UZ, map}};
        const std::array<gr::property_map, 2UZ> caps{gr::property_map{{"max_gain", kGainCeiling}}, gr::property_map{{"max_gain", 1.f}}};

        const RunResult result  = runOnce([&tags, &caps](gr::Graph& flow) { return buildSeries<CappingRelay, CappingRelay>(flow, tags, caps, coupledKeys()); });
        const auto      carried = carrying(result.tags, kOwnedKey);
        expect(eq(carried.size(), 1UZ));
        if (!carried.empty()) {
            expect(eq(valueOf(carried.front().map, kOwnedKey)->value_or<float>(0.f), kUpstreamGain)) << "a block's capped gain reaches the sink";
        }
    };

    "a processBulk block carries custom keys through its own work path"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{0UZ, protocolTag("r0")}, TagRecord{700UZ, protocolTag("r1")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, BulkRelay, Relay>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), tags.size())) << "the custom key crosses a processBulk block between two processOne blocks";
    };

    "a chain of three blocks with an input minimum delivers each tag once, in order"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{1UZ, protocolTag("r0")}, TagRecord{701UZ, protocolTag("r1")}, TagRecord{702UZ, protocolTag("r2")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildChainWindowed(flow, tags, kInputMinimum); });
        expectOnceInOrder("three-block chain", result.tags, tags | std::views::transform(&TagRecord::index) | std::ranges::to<std::vector>());
        expect(eq(result.samples.size(), kSamples)) << "every sample reaches the sink";
    };

    "a tag inside the stream's last full chunk and a tag in the trailing window keep their offsets"_test = [] {
        constexpr std::size_t        kTotal      = 10UZ; // seven samples, then a three-sample tail below the input minimum
        constexpr std::size_t        kMaxSamples = 7UZ;  // the one full chunk, which the trailing window follows
        const std::vector<TagRecord> tags{TagRecord{3UZ, protocolTag("r0")}, TagRecord{8UZ, protocolTag("r1")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildWindowed<TailRelay>(flow, tags, kTotal, kInputMinimum, kMaxSamples); });
        expectTagIndices("stream tail", result.tags, {tags[0].index, tags[1].index});
    };

    "a sink with an input minimum reads each tag of the samples it consumes once"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{1UZ, protocolTag("a")}, TagRecord{3UZ, protocolTag("b")}, TagRecord{6UZ, protocolTag("c")}, TagRecord{9UZ, protocolTag("d")}};
        gr::Graph                    flow;
        auto&                        source = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        source.tagsToEmit                   = tags;
        source.nTotal                       = 20UZ;
        auto& sink                          = flow.emplaceBlock<TwoSampleSink>(gr::property_map{{"name", std::string("snk")}});
        sink.in.min_samples                 = kInputMinimum;
        sink.in.max_samples                 = 7UZ;
        expect(flow.connect<"out", "in">(source, sink).has_value());
        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());
        expectTagIndices("two samples consumed per call", sink.tags, tags | std::views::transform(&TagRecord::index) | std::ranges::to<std::vector>());
    };

    "a decimator consuming one chunk per call places each tag of the samples it consumed at floor(i / 2), once and in order"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{1UZ, protocolTag("a")}, TagRecord{3UZ, protocolTag("b")}, TagRecord{4UZ, protocolTag("c")}, TagRecord{9UZ, protocolTag("d")}, TagRecord{15UZ, protocolTag("e")}};
        constexpr std::size_t        kTotal    = 16UZ;
        constexpr std::size_t        kWindow   = 6UZ;                    // three chunks per window; a call consumes one
        constexpr std::size_t        kConsumed = kTotal - kWindow + 2UZ; // the last full window's call consumes one chunk

        const RecordedRun        sink = runRecorded<OneChunkDecimator>(tags, kTotal, kWindow, kWindow);
        std::vector<std::size_t> expected;
        for (const TagRecord& tag : tags) {
            expected.push_back(tag.index < kConsumed ? tag.index / 2UZ : sink.nSamples); // a tag no call consumes leaves at the end index
        }
        expectTagIndices("2:1 decimator, one chunk per call", sink.tags, expected);
        expect(std::ranges::is_sorted(sink.tags, std::ranges::less{}, &TagRecord::index)) << "the sink reads the tags in index order";
    };

    "a decimator consuming one chunk per call applies a reset_default tag inside its window once"_test = [] {
        property_map reset;
        gr::tag::put(reset, gr::tag::RESET_DEFAULTS, true);
        const std::vector<TagRecord> tags{TagRecord{5UZ, reset}};
        constexpr std::size_t        kWindow = 6UZ; // three chunks per window; a call consumes one

        gr::Graph flow;
        auto&     source      = flow.emplaceBlock<Source>(gr::property_map{{"name", std::string("src")}});
        source.tagsToEmit     = tags;
        source.nTotal         = 16UZ;
        auto& middle          = flow.emplaceBlock<ResetCountingDecimator>(gr::property_map{{"name", std::string("mid")}});
        middle.in.min_samples = kWindow;
        middle.in.max_samples = kWindow;
        auto& sink            = flow.emplaceBlock<RecordingSink>(gr::property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());
        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(middle.nResets, tags.size())) << "the reset runs once per reset_default tag";
        expect(indicesCarrying(sink.tags, gr::tag::RESET_DEFAULTS.key()) == std::vector<std::size_t>{tags[0].index / 2UZ}) << "the reset_default tag leaves once, at the sample its input sample maps to";
    };

    "at the end index a custom key crosses a block"_test = [] {
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

        expectCustomKept(expectOnce(runToEnd<Relay>(tags), "processOne block"), "processOne block");
        expectCustomKept(expectOnce(runToEnd<BulkRelay>(tags), "processBulk block"), "processBulk block");
    };

    "at the end index a key a setting is named after leaves with the value it arrived with"_test = [] {
        property_map map = protocolTag("end");
        gr::tag::put(map, kOwnedKey, kUpstreamGain);
        gr::tag::put(map, gr::tag::SAMPLE_RATE, kUpstreamRate);

        const EndResult              result  = runToEnd<CappingRelay>({TagRecord{kSamples, map}});
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
            expect(eq(owned->value_or<float>(0.f), kUpstreamGain)) << "the block forwards its own gain in place of the end tag's";
        }
        const pmt::Value* rate = valueOf(arrived, gr::tag::SAMPLE_RATE.key());
        expect(rate != nullptr);
        if (rate != nullptr) {
            expect(eq(rate->value_or<float>(0.f), kUpstreamRate)) << "the block forwards its own rate in place of the end tag's";
        }
    };

    "at the end index a listed key stops and an unlisted key crosses"_test = [] {
        property_map map = protocolTag("end");
        gr::tag::put(map, kDroppedKey, 0.5f);
        const std::vector<TagRecord> tags{TagRecord{kSamples, map}};

        const EndResult plain = runToEnd<Relay>(tags);
        expect(eq(carrying(plain.endTags, kDroppedKey).size(), 1UZ)) << "the key crosses a block that lists nothing";

        const EndResult              dropping = runToEnd<DroppingRelay>(tags);
        const std::vector<TagRecord> carried  = carrying(dropping.endTags, kCustomKey);
        expect(eq(carried.size(), 1UZ)) << "an unlisted custom key crosses once";
        for (const TagRecord& record : carried) {
            expect(eq(record.index, dropping.nSamples)) << "the unlisted key stands at the end index";
            expect(record.map.contains(kReservedKey)) << "the reserved key crosses with it";
        }
        expect(carrying(dropping.endTags, kDroppedKey).empty()) << "the listed key stops at the block";
        expect(eq(carrying(dropping.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once";
    };

    "a stale carrier estimate stops at a block that lists it, in a chunk and at the end index"_test = [] {
        property_map map = protocolTag("stale");
        gr::tag::put(map, "freq_est", 1250.f);
        gr::tag::put(map, "phase_est", 0.75f);
        const std::vector<TagRecord>   interior{TagRecord{0UZ, map}, TagRecord{700UZ, map}};
        const std::vector<TagRecord>   atEnd{TagRecord{kSamples, map}};
        const std::vector<std::size_t> offsets{interior[0].index, interior[1].index};

        const RunResult plainChunks = runOnce([&interior](gr::Graph& flow) { return buildSeries<Relay>(flow, interior); });
        const EndResult plainEnd    = runToEnd<Relay>(atEnd);
        for (std::string_view key : {std::string_view("freq_est"), std::string_view("phase_est")}) {
            expect(indicesCarrying(plainChunks.tags, key) == offsets) << std::format("{} crosses a block that lists nothing", key);
            expect(eq(carrying(plainEnd.endTags, key).size(), 1UZ)) << std::format("{} crosses a block that lists nothing at the end index", key);
        }

        const RunResult droppingChunks = runOnce([&interior](gr::Graph& flow) { return buildSeries<CarrierEstimateDropper>(flow, interior); });
        const EndResult droppingEnd    = runToEnd<CarrierEstimateDropper>(atEnd);
        for (std::string_view key : {std::string_view("freq_est"), std::string_view("phase_est")}) {
            expect(carrying(droppingChunks.tags, key).empty()) << std::format("{} stops at the block that lists it", key);
            expect(carrying(droppingEnd.endTags, key).empty()) << std::format("{} stops at the block that lists it at the end index", key);
        }
        expect(indicesCarrying(droppingChunks.tags, kCustomKey) == offsets) << "an unlisted key crosses at its offsets";
        expect(eq(carrying(droppingEnd.endTags, kCustomKey).size(), 1UZ)) << "an unlisted key crosses at the end index";
        expect(eq(carrying(plainEnd.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once past a block that lists nothing";
        expect(eq(carrying(droppingEnd.endTags, gr::tag::END_OF_STREAM.key()).size(), 1UZ)) << "the end_of_stream key stands once past the listing block";
    };

    "a listed key inside trigger_meta_info stops at the listing block and an unlisted one crosses"_test = [] {
        property_map meta;
        meta.insert_or_assign(std::pmr::string("phase_est"), pmt::Value(0.75f));
        meta.insert_or_assign(std::pmr::string("snr_db"), pmt::Value(12.f));
        property_map map = protocolTag("meta");
        map.insert_or_assign(std::pmr::string(gr::tag::TRIGGER_META_INFO.key()), pmt::Value(meta));
        const std::vector<TagRecord> tags{TagRecord{0UZ, map}, TagRecord{700UZ, map}};

        const auto metaKeysAt = [](const RunResult& result, std::string_view key) {
            std::vector<std::size_t> indices;
            for (const TagRecord& record : carrying(result.tags, gr::tag::TRIGGER_META_INFO.key())) {
                const pmt::Value*   value  = valueOf(record.map, gr::tag::TRIGGER_META_INFO.key());
                const property_map* nested = value == nullptr ? nullptr : value->get_if<property_map>();
                if (nested != nullptr && nested->contains(std::pmr::string(key))) {
                    indices.push_back(record.index);
                }
            }
            return indices;
        };
        const std::vector<std::size_t> offsets{tags[0].index, tags[1].index};

        const RunResult plain = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay>(flow, tags); });
        expect(metaKeysAt(plain, "phase_est") == offsets) << "the nested key crosses a block that lists nothing";

        const RunResult dropping = runOnce([&tags](gr::Graph& flow) { return buildSeries<CarrierEstimateDropper>(flow, tags); });
        expect(metaKeysAt(dropping, "phase_est").empty()) << "the listed key inside trigger_meta_info stops at the block";
        expect(metaKeysAt(dropping, "snr_db") == offsets) << "an unlisted key inside trigger_meta_info crosses at its offsets";
        expect(indicesCarrying(dropping.tags, kCustomKey) == offsets) << "the tag's other keys cross";
    };

    "tags two samples apart all keep their custom keys"_test = [] {
        std::vector<TagRecord> tags;
        for (std::size_t i = 0UZ; i < 64UZ; ++i) {
            tags.push_back(TagRecord{300UZ + 2UZ * i, protocolTag(std::format("dense-{}", i))});
        }

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<Relay, Relay>(flow, tags); });
        expect(eq(carrying(result.tags, kCustomKey).size(), tags.size())) << "every custom key survives, none is duplicated";
    };
};

const boost::ut::suite<"strided tag propagation"> _stridedTagPropagation = [] {
    using namespace boost::ut;
    using namespace qa_uniform_tag_propagation;

    constexpr std::size_t kChunk = SkippingDecimator::ResamplingControl::kInputChunkSize;
    constexpr std::size_t kStep  = SkippingDecimator::StrideControl::kStride;
    constexpr float       kRatio = static_cast<float>(SkippingDecimator::ResamplingControl::kOutputChunkSize) / static_cast<float>(kChunk);
    static_assert(kStep > kChunk + 2UZ, "the cases place a tag two samples into a skip");

    "a tag on the first skipped sample leaves at the next chunk's first output sample"_test = [] {
        property_map map = protocolTag("first-skipped");
        gr::tag::put(map, gr::tag::SAMPLE_RATE.key(), kUpstreamRate);
        const std::vector<TagRecord> tags{TagRecord{kChunk, map}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<SkippingDecimator>(flow, tags); });
        expect(indicesCarrying(result.tags, kReservedKey) == std::vector<std::size_t>{nextChunkOutput(kChunk)}) << "the reserved key leaves once, at the next chunk";
        expect(indicesCarrying(result.tags, kCustomKey) == std::vector<std::size_t>{nextChunkOutput(kChunk)}) << "a custom key leaves with the skipped tag";

        const std::vector<TagRecord> rates = carrying(result.tags, gr::tag::SAMPLE_RATE.key());
        expect(eq(rates.size(), 1UZ)) << "the rate leaves once";
        if (!rates.empty()) {
            expect(eq(rates.front().index, nextChunkOutput(kChunk))) << "the rate leaves at the next chunk";
            const pmt::Value* rate = valueOf(rates.front().map, gr::tag::SAMPLE_RATE.key());
            expect(rate != nullptr && rate->value_or(0.f) == kUpstreamRate * kRatio) << "the rate is not the upstream rate at the output rate";
        }
        expectFramesOnStride<SkippingDecimator>("first skipped sample", result.samples, kSamples);
    };

    "a tag inside a skip leaves every chunk where the stride puts it"_test = [] {
        constexpr std::size_t        kAt = kChunk + 2UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("inside-skip")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<SkippingDecimator>(flow, tags); });
        expectFramesOnStride<SkippingDecimator>("tag inside a skip", result.samples, kSamples);
        expect(indicesCarrying(result.tags, kReservedKey) == std::vector<std::size_t>{nextChunkOutput(kAt)}) << "the reserved key leaves at the next chunk";
    };

    "a tag after a chunk's first sample leaves at that chunk's output sample"_test = [] {
        constexpr std::size_t        kAt = kStep + 2UZ;
        const std::vector<TagRecord> tags{TagRecord{kAt, protocolTag("inside-chunk")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<SkippingDecimator>(flow, tags); });
        expect(indicesCarrying(result.tags, kReservedKey) == std::vector<std::size_t>{kAt / kStep}) << "the chunk that reads the tag forwards it";
        expectFramesOnStride<SkippingDecimator>("tag inside a chunk", result.samples, kSamples);
    };

    "a skip that spans two calls forwards the tag it covers"_test = [] {
        constexpr std::size_t kFirstFeed = kChunk + 3UZ;         // the chunk, and three samples of its skip
        constexpr std::size_t kAt        = kFirstFeed - 1UZ;     // the last sample of the first feed
        constexpr std::size_t kTotal     = kStep + kChunk + 6UZ; // up to and past the second chunk
        constexpr std::size_t kOwed      = kStep - kFirstFeed;   // the skip still owed after the second call
        static_assert(kAt >= kChunk && kAt < kStep && kOwed > 0UZ);

        PortOut<float>    upstream;
        PortIn<float>     downstream;
        SkippingDecimator block;
        expect(upstream.connect(block.in).has_value());
        expect(block.out.connect(downstream).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const auto feed = [&upstream](std::size_t first, std::size_t last) {
            auto span = upstream.reserve<SpanReleasePolicy::ProcessAll>(last - first);
            for (std::size_t i = 0UZ; i < span.size(); ++i) {
                span[i] = static_cast<float>(first + i);
            }
            span.publish(span.size());
        };
        upstream.publishTag(protocolTag("two-calls"), kAt);
        feed(0UZ, kFirstFeed);

        expect(block.work(kTotal).status == work::Status::OK) << "the first call reads the first chunk";
        std::ignore = block.work(kTotal); // skips the rest of the first feed
        expect(eq(static_cast<std::size_t>(block.strideCounter), kOwed)) << "the skip crosses the call boundary";
        expect(eq(block.in.streamReader().position(), kFirstFeed)) << "the second call skips every sample it holds";

        feed(kFirstFeed, kTotal);
        expect(block.work(kTotal).status == work::Status::OK) << "the third call ends the skip and reads the second chunk";

        auto out = downstream.get<SpanReleasePolicy::ProcessAll>(downstream.streamReader().available());
        expect(eq(out.size(), 2UZ)) << "one output sample per chunk";
        if (out.size() == 2UZ) {
            expect(out[0UZ] == 0.f && out[1UZ] == static_cast<float>(kStep)) << "the second chunk starts one stride after the first";
        }
        std::vector<TagRecord> received;
        for (const Tag& tag : out.rawTags) {
            received.push_back(TagRecord{tag.index, tag.map});
        }
        expect(indicesCarrying(received, kReservedKey) == std::vector<std::size_t>{1UZ}) << "the tag leaves at the second chunk's output sample";
        std::ignore = out.consume(out.size());
    };

    "a tag in a skip the stream ends in leaves at the end index"_test = [] {
        constexpr std::size_t kLastChunk = (kSamples - kChunk) / kStep * kStep;
        constexpr std::size_t kAt        = kSamples - 2UZ;
        static_assert(kAt >= kLastChunk + kChunk, "the tag lies in the skip after the last chunk");

        const EndResult result = runToEnd<SkippingDecimator>({TagRecord{kAt, protocolTag("last-skip")}});
        expect(eq(result.nSamples, kLastChunk / kStep + 1UZ)) << "one output sample per chunk";
        expect(indicesCarrying(result.endTags, kReservedKey) == std::vector<std::size_t>{result.nSamples}) << "the reserved key leaves at the end index";
    };

    "the forward map of a tag in the stream's last skip leaves at the end index, after the tag"_test = [] {
        constexpr std::size_t kLastChunk = (kSamples - kChunk) / kStep * kStep;
        constexpr std::size_t kAt        = kSamples - 2UZ;
        static_assert(kAt >= kLastChunk + kChunk, "the tag lies in the skip after the last chunk");
        constexpr float kRate = 2000.f;
        property_map    map   = protocolTag("last-skip");
        gr::tag::put(map, gr::tag::SAMPLE_RATE, kRate);

        const EndResult result = runToEnd<SkippingHalfRateDecimator>({TagRecord{kAt, map}}, property_map{{"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}});
        const auto      tagAt  = std::ranges::find_if(result.endTags, [](const TagRecord& record) { return record.map.contains(kCustomKey); });
        const auto      atTag  = std::ranges::find_if(result.endTags, [](const TagRecord& record) { return record.map.contains(kHalfRateKey); });
        expect(atTag != result.endTags.end()) << "the forward map does not reach the end index";
        if (atTag == result.endTags.end()) {
            return;
        }
        expect(eq(atTag->index, result.nSamples)) << "the forward map leaves at the end index";
        const pmt::Value* half = valueOf(atTag->map, kHalfRateKey);
        expect(half != nullptr && eq(half->value_or<float>(0.f), kRate / 2.f)) << "the forward map carries half the tag's rate";
        expect(tagAt < atTag) << "the forward map precedes the tag";
    };

    "a block supplying forwardTags() is handed a skipped tag in the next chunk"_test = [] {
        const std::vector<TagRecord> tags{TagRecord{kChunk, protocolTag("override")}};

        const RunResult result = runOnce([&tags](gr::Graph& flow) { return buildSeries<SkippingForwarder>(flow, tags); });
        expect(indicesCarrying(result.tags, kCustomKey) == std::vector<std::size_t>{nextChunkOutput(kChunk)}) << "the override publishes the skipped tag at the next chunk";
        expectFramesOnStride<SkippingForwarder>("forwardTags() override", result.samples, kSamples);
    };
};

int main() { /* not needed by the UT framework */ }
