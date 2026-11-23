#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockMerging.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PmtTypeHelpers.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

#include "settings_utils.hpp"

/**
 * @brief Where a resampling block's sample_rate lives.
 *
 * A block's own settings hold the rate it is fed at. The chunk ratio belongs to the rate tag it passes on. Scaling the
 * stored value instead makes every further application scale again, so the rate drops further with each of the
 * save/load or re-apply cycles that a running graph performs routinely.
 *
 * The blocks are defined here: gnuradio4-core carries no standard block library, so a core test may not depend on one.
 */

namespace qa_coupling {
/// couples `setting` to the tag key `key` through the settings API
inline void couple(gr::SettingsBase& settings, std::string_view setting, std::string_view key) { std::ignore = settings.setTagCouplings(gr::property_map{{std::pmr::string(setting), std::string(key)}}); }
} // namespace qa_coupling

namespace qa_settings {

// a tag that reaches the block's input, as the work call hands it to the settings
inline void enterTag(gr::SettingsBase& settings, const gr::Tag& tag) { std::ignore = gr::settings::detail::FrameworkAccess::tagEnters(settings, tag.map); }

using namespace gr;

inline constexpr gr::Size_t  kDecimation = 4U;
inline constexpr float       kInputRate  = 1000.0f;
inline constexpr float       kOutputRate = kInputRate / static_cast<float>(kDecimation);
inline constexpr std::size_t kSamples    = 256UZ;

struct Decimator : Block<Decimator, Resampling<>> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float> sample_rate = kInputRate;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(Decimator, in, out, settings);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t ratio = static_cast<std::size_t>(input_chunk_size.value);
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * ratio];
        }
        return work::Status::OK;
    }
};

struct RateTagSource : Block<RateTagSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(RateTagSource, out);

    std::size_t _emitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= kSamples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), kSamples - _emitted);
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        std::ranges::fill(outSpan | std::views::take(n), 1.0f);
        if (_emitted == 0UZ) {
            outSpan.publishTag(property_map{{gr::tag::SAMPLE_RATE.key(), kInputRate}}, 0UZ);
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct RateTagSink : Block<RateTagSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(RateTagSink, in);

    std::vector<float> rates;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        for (const Tag& tag : inSpan.rawTags) {
            if (auto rate = rateOf(tag.map); rate.has_value()) {
                rates.push_back(*rate);
            }
        }
        const std::size_t n = inSpan.size();
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }

    [[nodiscard]] static std::optional<float> rateOf(const property_map& map) {
        auto it = map.find(gr::tag::SAMPLE_RATE.key());
        if (it == map.end()) {
            return std::nullopt;
        }
        const float* value = it->second.get_if<float>();
        return value != nullptr ? std::optional<float>(*value) : std::nullopt;
    }
};

inline constexpr std::size_t kTagStride   = 16UZ;
inline constexpr std::size_t kShortStream = 512UZ;
inline constexpr std::size_t kLongStream  = 2UZ * kShortStream;
inline constexpr float       kRetunedRate = 2000.0f;

struct RepeatingRateSource : Block<RepeatingRateSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(RepeatingRateSource, out);

    std::size_t nSamples  = kShortStream;
    float       finalRate = kInputRate; // the last tag's value, every earlier tag carries kInputRate

    std::size_t _emitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (_emitted >= nSamples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), nSamples - _emitted);
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i]              = 1.0f;
            const std::size_t index = _emitted + i;
            if (index % kTagStride == 0UZ) {
                outSpan.publishTag(property_map{{gr::tag::SAMPLE_RATE.key(), index + kTagStride >= nSamples ? finalRate : kInputRate}}, i);
            }
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

// the default differs from the streamed rate, so the first tag is a genuine change
struct RateCountingBlock : Block<RateCountingBlock> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float> sample_rate = 1.0f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate);
    } settings;

    GR_MAKE_REFLECTABLE(RateCountingBlock, in, out, settings);

    std::size_t nSettingsChanged = 0UZ;
    bool        setUp            = false;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void applySettings(const Settings& s, const Settings& old) {
        if (setUp && s.sample_rate != old.sample_rate) { // the initialization is not counted
            nSettingsChanged++;
        }
        setUp = true;
    }
};

/// keeps both maps of every settings change, so what the callback was told can be read off the block. Every member
/// carries an initializer of its own: the block is an aggregate constructed from its base alone, and a member with
/// one setting of each kind a conversion case sends to: integers of both signs, floating point, a vector, an array
/// and a native size
struct ConversionBlock : Block<ConversionBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        std::int64_t            signed_count   = 0;
        std::uint64_t           unsigned_count = 0U;
        float                   sample_rate    = 1.0f;
        double                  frequency      = 1.0;
        std::vector<float>      taps{};
        std::array<double, 2UZ> weights{};
        std::size_t             num_filters = 1UZ;

        GR_MAKE_REFLECTABLE(Settings, signed_count, unsigned_count, sample_rate, frequency, taps, weights, num_filters);
    } settings{};

    GR_MAKE_REFLECTABLE(ConversionBlock, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// the reasons set() refuses the parameters with, empty when it accepts them all
template<typename TBlock>
[[nodiscard]] std::string refusalOf(TBlock& block, const property_map& parameters) {
    const auto result = block.set(parameters);
    return result.has_value() ? std::string{} : gr::settings::describeRejected(result.error());
}

/// no initializer is one clang reports as a missing field initializer at each such construction.
struct ChangeRecordingBlock : Block<ChangeRecordingBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float>                             sample_rate = 1.0f;
        Annotated<gr::Size_t>                        fft_size    = 1024U;
        Annotated<gr::Size_t>                        n_averages  = 1U;
        Annotated<gr::Size_t, Doc<"worker threads">> n_workers   = 1U;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, fft_size, n_averages, n_workers);
    } settings{};

    GR_MAKE_REFLECTABLE(ChangeRecordingBlock, in, out, settings);

    std::vector<property_map> oldSeen{};
    std::vector<property_map> newSeen{};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void applySettings(const Settings& s, const Settings& old) {
        property_map before;
        property_map after;
        const auto   record = [&before, &after](std::string_view key, const auto& now, const auto& was) {
            if (now.value != was.value) {
                before.insert_or_assign(std::pmr::string(key), was.value);
                after.insert_or_assign(std::pmr::string(key), now.value);
            }
        };
        record("sample_rate", s.sample_rate, old.sample_rate);
        record("fft_size", s.fft_size, old.fft_size);
        record("n_averages", s.n_averages, old.n_averages);
        record("n_workers", s.n_workers, old.n_workers);
        if (!after.empty()) {
            oldSeen.push_back(std::move(before));
            newSeen.push_back(std::move(after));
        }
    }
};

void applySettings(ChangeRecordingBlock& block, const property_map& parameters) {
    std::ignore = block.set(parameters);
    std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
}

[[nodiscard]] std::optional<gr::Size_t> sizeOf(const property_map& map, std::string_view key) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return std::nullopt;
    }
    const gr::Size_t* value = it->second.get_if<gr::Size_t>();
    return value != nullptr ? std::optional<gr::Size_t>(*value) : std::nullopt;
}

template<typename T>
[[nodiscard]] std::optional<T> valueAt(const property_map& map, std::string_view key) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return std::nullopt;
    }
    const T* value = it->second.get_if<T>();
    return value != nullptr ? std::optional<T>(*value) : std::nullopt;
}

struct OnesSource : Block<OnesSource> {
    PortOut<float> out{};

    GR_MAKE_REFLECTABLE(OnesSource, out);

    [[nodiscard]] constexpr float processOne() const noexcept { return 1.0f; }
};

struct DiscardSink : Block<DiscardSink> {
    PortIn<float> in{};

    GR_MAKE_REFLECTABLE(DiscardSink, in);

    void processOne(float) noexcept {}
};

/// passes its input on scaled. The work function counts the samples in n_seen and publishes the count after each
/// call. Nothing in the block writes or copies n_foreign. Every member carries an initializer of its own, as
/// above.
struct CopyingCounter : Block<CopyingCounter> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Doc<"output scale">>        scale     = 1.0f;
        Annotated<gr::Size_t, Doc<"samples passed">> n_seen    = 0U;
        Annotated<gr::Size_t, Doc<"external count">> n_foreign = 0U;

        GR_MAKE_REFLECTABLE(Settings, scale, n_seen, n_foreign);
    } settings{};

    GR_MAKE_REFLECTABLE(CopyingCounter, in, out, settings);

    std::atomic<std::size_t>* nCopies = nullptr;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        std::ranges::transform(inSpan, outSpan.begin(), [this](float value) { return value * settings.scale.value; });
        this->publish("n_seen", static_cast<gr::Size_t>(settings.n_seen.value + inSpan.size()));
        if (nCopies != nullptr && !inSpan.empty()) {
            nCopies->fetch_add(1UZ, std::memory_order_release);
        }
        return work::Status::OK;
    }
};

/// runs a CopyingCounter between a source and a sink on a scheduler in a thread of its own
struct RunningCounter {
    std::atomic<std::size_t> nCopies{0UZ};
    gr::scheduler::Simple<>  scheduler{};
    CopyingCounter*          counter = nullptr;
    std::thread              runner{};

    RunningCounter() {
        using namespace boost::ut;
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<OnesSource>();
        auto&     block  = flow.emplaceBlock<CopyingCounter>();
        auto&     sink   = flow.emplaceBlock<DiscardSink>();
        block.nCopies    = &nCopies;
        counter          = &block;
        expect(flow.connect<"out", "in">(source, block).has_value());
        expect(flow.connect<"out", "in">(block, sink).has_value());
        expect(scheduler.exchange(std::move(flow)).has_value());
        runner = std::thread([this] { std::ignore = scheduler.runAndWait(); });
    }

    RunningCounter(const RunningCounter&)            = delete;
    RunningCounter& operator=(const RunningCounter&) = delete;

    ~RunningCounter() {
        scheduler.requestStop();
        runner.join();
    }

    template<typename TCondition>
    [[nodiscard]] static bool awaitTrue(TCondition satisfied) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!satisfied()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    [[nodiscard]] bool awaitCopies(std::size_t nFurther) {
        const std::size_t target = nCopies.load(std::memory_order_acquire) + nFurther;
        return awaitTrue([this, target] { return nCopies.load(std::memory_order_acquire) >= target; });
    }

    /// the values a read of the running counter's settings gives, on the test's thread
    [[nodiscard]] property_map readSettings() const { return gr::settings::heldValues(counter->get()); }
};

inline constexpr std::string_view kDecimationKey    = "test:decimation";
inline constexpr std::string_view kInterpolationKey = "test:interpolation";

/// keeps the first sample of every `decimation` and follows it with `interpolation` - 1 zeros. Its chunk ratio follows
/// the two settings. A call whose spans do not hold whole chunks of that ratio is an error. nMisSizedCalls counts the
/// calls refused that way; nResets counts reset() calls.
struct RatioFollower : Block<RatioFollower, Resampling<>, Stride<>> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<gr::Size_t, Limits<Min<1U>, Max<64U>>> decimation    = 1U;
        Annotated<gr::Size_t, Limits<Min<1U>, Max<64U>>> interpolation = 1U;

        GR_MAKE_REFLECTABLE(Settings, decimation, interpolation);
    } settings;

    GR_MAKE_REFLECTABLE(RatioFollower, in, out, settings);

    std::size_t nMisSizedCalls = 0UZ;
    std::size_t nResets        = 0UZ;

    void reset() { ++nResets; }

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t nDecimation    = settings.decimation.value;
        const std::size_t nInterpolation = settings.interpolation.value;
        const std::size_t nChunks        = inSpan.size() / nDecimation;
        if (inSpan.size() != nChunks * nDecimation || outSpan.size() != nChunks * nInterpolation) {
            ++nMisSizedCalls;
            return work::Status::ERROR;
        }
        std::ranges::fill(outSpan, 0.f);
        for (std::size_t chunk = 0UZ; chunk < nChunks; ++chunk) {
            outSpan[chunk * nInterpolation] = inSpan[chunk * nDecimation];
        }
        return work::Status::OK;
    }

    void applySettings(const Settings& s) {
        input_chunk_size  = s.decimation.value;
        output_chunk_size = s.interpolation.value;
    }
};

inline constexpr std::string_view kLevelKey      = "test:level";
inline constexpr gr::Size_t       kEpilogueChunk = 4U;

/// copies its input in chunks of kEpilogueChunk samples, and the samples short of a chunk at the stream's end through
/// processEpilogue(). It holds `level` and does nothing with it.
struct EpilogueLevelCopy : Block<EpilogueLevelCopy, Resampling<kEpilogueChunk, kEpilogueChunk, true>> {
    PortIn<float>  in;
    PortOut<float> out;

    struct Settings {
        Annotated<float> level = 0.f;

        GR_MAKE_REFLECTABLE(Settings, level);
    } settings;

    GR_MAKE_REFLECTABLE(EpilogueLevelCopy, in, out, settings);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        std::ranges::copy(inSpan, outSpan.begin());
        return work::Status::OK;
    }

    work::Status processEpilogue(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(inSpan.size(), outSpan.size());
        std::ranges::copy(inSpan | std::views::take(n), outSpan.begin());
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct TagAt {
    std::size_t  index{};
    property_map map{};
    bool         pastLastSample = false; // published by the call whose samples end at its index, after its last sample
};

/// publishes the ramp 0, 1, 2, ... in calls of irregular length, each tag at its index. Once pauseAt samples are out,
/// it publishes nothing for nPauseCalls calls.
struct RampSource : Block<RampSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(RampSource, out);

    std::size_t        nSamples = 0UZ;
    std::vector<TagAt> tagsToEmit; // ordered by index, each below nSamples; a tag past a last sample may sit at nSamples
    std::size_t        pauseAt     = std::numeric_limits<std::size_t>::max();
    std::size_t        nPauseCalls = 0UZ;

    std::size_t _emitted = 0UZ;
    std::size_t _nextTag = 0UZ;
    std::size_t _nCalls  = 0UZ;
    std::size_t _nPaused = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        static constexpr std::array<std::size_t, 4> kLengths{7UZ, 13UZ, 3UZ, 29UZ};

        if (_emitted >= nSamples) {
            outSpan.publish(0UZ);
            return work::Status::DONE;
        }
        if (_emitted == pauseAt && _nPaused < nPauseCalls) {
            ++_nPaused;
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
        std::size_t n = std::min({kLengths[_nCalls++ % kLengths.size()], outSpan.size(), nSamples - _emitted});
        if (_emitted < pauseAt) {
            n = std::min(n, pauseAt - _emitted);
        }
        const auto pendingTags = tagsToEmit | std::views::drop(static_cast<std::ptrdiff_t>(_nextTag));
        if (const auto pastEnd = std::ranges::find_if(pendingTags, &TagAt::pastLastSample); pastEnd != pendingTags.end()) {
            n = std::min(n, pastEnd->index - _emitted);
        }
        if (n == 0UZ) {
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_OUTPUT_ITEMS;
        }
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<float>(_emitted + i);
        }
        for (; _nextTag < tagsToEmit.size() && (tagsToEmit[_nextTag].index < _emitted + n || (tagsToEmit[_nextTag].pastLastSample && tagsToEmit[_nextTag].index == _emitted + n)); ++_nextTag) {
            outSpan.publishTag(tagsToEmit[_nextTag].map, tagsToEmit[_nextTag].index - _emitted);
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

struct SampleSink : Block<SampleSink> {
    PortIn<float> in;

    GR_MAKE_REFLECTABLE(SampleSink, in);

    std::vector<float> samples;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        samples.insert(samples.end(), inSpan.begin(), inSpan.end());
        std::ignore = inSpan.consume(inSpan.size());
        return work::Status::OK;
    }
};

[[nodiscard]] property_map ratioTag(std::optional<gr::Size_t> nDecimation, std::optional<gr::Size_t> nInterpolation, bool resetDefaults = false) {
    property_map map;
    if (nDecimation.has_value()) {
        map.insert_or_assign(std::pmr::string(kDecimationKey), *nDecimation);
    }
    if (nInterpolation.has_value()) {
        map.insert_or_assign(std::pmr::string(kInterpolationKey), *nInterpolation);
    }
    if (resetDefaults) {
        gr::tag::put(map, gr::tag::RESET_DEFAULTS, true);
    }
    return map;
}

/// what a RatioFollower with the given stride makes of the ramp 0 .. nSamples - 1. A chunk starts every `stride`
/// samples, or every `decimation` samples for a stride of 0. A tag applies at the chunk that starts at its index, and
/// each tag sits at a chunk start under the ratio before it. The samples after the last whole chunk yield nothing.
[[nodiscard]] std::vector<float> expectedOutput(std::size_t nSamples, const std::vector<TagAt>& tags, std::size_t stride = 0UZ) {
    std::vector<float> expected;
    std::size_t        nDecimation    = 1UZ;
    std::size_t        nInterpolation = 1UZ;
    std::size_t        nextTag        = 0UZ;
    for (std::size_t start = 0UZ;; start += stride != 0UZ ? stride : nDecimation) {
        for (; nextTag < tags.size() && tags[nextTag].index == start; ++nextTag) {
            if (tags[nextTag].map.contains(gr::tag::RESET_DEFAULTS.key())) {
                nDecimation    = 1UZ;
                nInterpolation = 1UZ;
            }
            if (const std::optional<gr::Size_t> value = sizeOf(tags[nextTag].map, kDecimationKey); value.has_value()) {
                nDecimation = *value;
            }
            if (const std::optional<gr::Size_t> value = sizeOf(tags[nextTag].map, kInterpolationKey); value.has_value()) {
                nInterpolation = *value;
            }
        }
        if (start + nDecimation > nSamples) {
            return expected;
        }
        expected.push_back(static_cast<float>(start));
        expected.insert(expected.end(), nInterpolation - 1UZ, 0.f);
    }
}

struct RatioRun {
    bool               ran = false;
    std::vector<float> samples;
    std::size_t        nMisSizedCalls = 0UZ;
    std::size_t        nResets        = 0UZ;
};

struct RatioStream {
    std::size_t        nSamples = 0UZ;
    std::vector<TagAt> tags;
    gr::Size_t         stride      = 0U;
    std::size_t        pauseAt     = std::numeric_limits<std::size_t>::max();
    std::size_t        nPauseCalls = 0UZ;
};

[[nodiscard]] RatioRun runRatioStream(const RatioStream& stream) {
    gr::Graph flow;
    auto&     source   = flow.emplaceBlock<RampSource>(property_map{{"name", std::string("src")}});
    source.nSamples    = stream.nSamples;
    source.tagsToEmit  = stream.tags;
    source.pauseAt     = stream.pauseAt;
    source.nPauseCalls = stream.nPauseCalls;
    auto& follower     = flow.emplaceBlock<RatioFollower>(property_map{{"name", std::string("ratio")}, {"stride", stream.stride}});
    std::ignore        = follower.setTagCouplings(property_map{{"decimation", std::string(kDecimationKey)}, {"interpolation", std::string(kInterpolationKey)}});
    auto& sink         = flow.emplaceBlock<SampleSink>(property_map{{"name", std::string("snk")}});
    if (!flow.connect<"out", "in">(source, follower).has_value() || !flow.connect<"out", "in">(follower, sink).has_value()) {
        return {};
    }

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    if (!scheduler.exchange(std::move(flow)).has_value()) {
        return {};
    }
    const bool ran = scheduler.runAndWait().has_value();
    return {.ran = ran, .samples = sink.samples, .nMisSizedCalls = follower.nMisSizedCalls, .nResets = follower.nResets};
}

RatioRun expectRatioStream(const RatioStream& stream, std::string_view what) {
    using namespace boost::ut;
    RatioRun run = runRatioStream(stream);
    expect(run.ran) << what << ": the run ends in an error";
    expect(eq(run.nMisSizedCalls, 0UZ)) << what << ": a call is sized for a ratio other than the one at its first sample";
    expect(run.samples == expectedOutput(stream.nSamples, stream.tags, stream.stride)) << what << ": the output differs from the ratio each tag sets from its sample on";
    return run;
}

struct RateStreamResult {
    std::size_t nSettingsChanged;
    float       finalRate;
};

[[nodiscard]] RateStreamResult runRateStream(std::size_t nSamples, float finalRate) {
    using namespace boost::ut;

    gr::Graph flow;
    auto&     source = flow.emplaceBlock<RepeatingRateSource>();
    source.nSamples  = nSamples;
    source.finalRate = finalRate;
    auto& middle     = flow.emplaceBlock<RateCountingBlock>();
    qa_coupling::couple(gr::settings::detail::settingsOf(middle), "sample_rate", gr::tag::SAMPLE_RATE.key());
    auto& sink = flow.emplaceBlock<RateTagSink>();
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return {middle.nSettingsChanged, middle.settings.sample_rate.value};
}

const boost::ut::suite<"integer into floating-point settings"> exactIntegerTests = [] {
    using namespace boost::ut;
    using gr::pmt::convert_safely;

    // an integer converts when the bits between its highest and lowest set bit fit the mantissa: trailing zero bits
    // go into the exponent, so only a value that would round is refused
    "an integer a float holds exactly converts, whatever its magnitude"_test = [] {
        const auto rate = convert_safely<float>(std::int64_t{20'000'000}); // 78125 * 2^8, 17 significant bits
        expect(rate.has_value()) << (rate.has_value() ? std::string{} : rate.error());
        expect(eq(rate.value_or(0.0f), 20'000'000.0f));

        const auto large = convert_safely<double>(std::int64_t{1} << 60);
        expect(eq(large.value_or(0.0), 0x1p60)) << (large.has_value() ? std::string{} : large.error());

        const auto lowest = convert_safely<double>(std::numeric_limits<std::int64_t>::min()); // -2^63
        expect(eq(lowest.value_or(0.0), -0x1p63)) << (lowest.has_value() ? std::string{} : lowest.error());

        expect(eq(convert_safely<float>(std::int64_t{0}).value_or(-1.0f), 0.0f));
        expect(eq(convert_safely<float>(std::int64_t{-16'777'216}).value_or(0.0f), -16'777'216.0f)); // -2^24
    };

    "an integer that would round is refused, naming the bits it needs"_test = [] {
        const auto odd = convert_safely<float>(std::int64_t{16'777'217}); // 2^24 + 1
        expect(!odd.has_value());
        expect(!odd.has_value() && odd.error().contains("25 significant bits")) << (odd.has_value() ? std::string{} : odd.error());

        const auto wide = convert_safely<double>((std::int64_t{1} << 53) + 1);
        expect(!wide.has_value());
        expect(!wide.has_value() && wide.error().contains("54 significant bits")) << (wide.has_value() ? std::string{} : wide.error());

        expect(!convert_safely<double>(std::numeric_limits<std::uint64_t>::max()).has_value());
    };

    "a float setting takes an integer it holds exactly"_test = [] {
        qa_settings::RateCountingBlock block;
        block.init(std::make_shared<gr::Sequence>());
        expect(nothrow([&] { std::ignore = block.set({{"sample_rate", std::int64_t{20'000'000}}}); }));
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.sample_rate.value, 20'000'000.0f));
    };
};

const boost::ut::suite<"out-of-range numbers into integer settings"> integerRangeTests = [] {
    using namespace boost::ut;
    using gr::pmt::convert_safely;

    "an unsigned integer above a signed type's maximum is refused"_test = [] {
        constexpr std::uint64_t unsignedMax = std::numeric_limits<std::uint64_t>::max();
        expect(!convert_safely<std::int64_t>(unsignedMax).has_value()) << "uint64 max into int64";
        expect(!convert_safely<std::int32_t>(unsignedMax).has_value()) << "uint64 max into int32";
        expect(!convert_safely<std::int8_t>(unsignedMax).has_value()) << "uint64 max into int8";
        expect(!convert_safely<std::uint32_t>(unsignedMax).has_value()) << "uint64 max into uint32";
        expect(!convert_safely<std::int64_t>(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U).has_value()) << "2^63 into int64";

        constexpr std::int64_t signedMax = std::numeric_limits<std::int64_t>::max();
        expect(convert_safely<std::int64_t>(static_cast<std::uint64_t>(signedMax)) == signedMax) << "int64 max held in a uint64";
        expect(convert_safely<std::int32_t>(static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) == std::numeric_limits<std::int32_t>::max()) << "int32 max held in a uint64";
    };

    "a negative integer is refused by an unsigned type"_test = [] {
        expect(!convert_safely<std::uint64_t>(std::numeric_limits<std::int64_t>::min()).has_value()) << "int64 min into uint64";
        expect(!convert_safely<std::uint8_t>(std::int64_t{-1}).has_value()) << "-1 into uint8";
    };

    "a negative integer below a signed type's lowest value is refused"_test = [] {
        constexpr std::int64_t int32Lowest = std::numeric_limits<std::int32_t>::lowest();
        expect(convert_safely<std::int32_t>(int32Lowest) == std::numeric_limits<std::int32_t>::lowest()) << "int32 lowest held in an int64";
        expect(!convert_safely<std::int32_t>(int32Lowest - 1).has_value()) << "int32 lowest - 1 into int32";
        expect(!convert_safely<std::int32_t>(std::numeric_limits<std::int64_t>::min()).has_value()) << "int64 min into int32";
    };

    "a double at or above 2^digits is refused and the largest double below it converts"_test = [] {
        constexpr double twoTo63 = 0x1p63;
        constexpr double twoTo64 = 0x1p64;
        expect(!convert_safely<std::int64_t>(twoTo63).has_value()) << "2^63 into int64";
        expect(!convert_safely<std::uint64_t>(twoTo64).has_value()) << "2^64 into uint64";

        const double belowTwoTo63 = std::nextafter(twoTo63, 0.0);
        const double belowTwoTo64 = std::nextafter(twoTo64, 0.0);
        expect(convert_safely<std::int64_t>(belowTwoTo63) == static_cast<std::int64_t>(belowTwoTo63)) << "largest double below 2^63 into int64";
        expect(convert_safely<std::uint64_t>(belowTwoTo64) == static_cast<std::uint64_t>(belowTwoTo64)) << "largest double below 2^64 into uint64";
        expect(convert_safely<std::uint64_t>(twoTo63) == (std::uint64_t{1} << 63U)) << "2^63 into uint64";
        expect(convert_safely<std::int64_t>(-twoTo63) == std::numeric_limits<std::int64_t>::min()) << "-2^63 into int64";
        expect(!convert_safely<std::int64_t>(std::nextafter(-twoTo63, -twoTo64)).has_value()) << "the double below -2^63 into int64";
    };

    "a float at or above 2^digits is refused and the largest float below it converts"_test = [] {
        constexpr float twoTo31 = 0x1p31f;
        constexpr float twoTo32 = 0x1p32f;
        constexpr float twoTo63 = 0x1p63f;
        constexpr float twoTo64 = 0x1p64f;
        expect(!convert_safely<std::int32_t>(twoTo31).has_value()) << "2^31 into int32";
        expect(!convert_safely<std::uint32_t>(twoTo32).has_value()) << "2^32 into uint32";
        expect(!convert_safely<std::int64_t>(twoTo63).has_value()) << "2^63 into int64";
        expect(!convert_safely<std::uint64_t>(twoTo64).has_value()) << "2^64 into uint64";

        const float belowTwoTo31 = std::nextafter(twoTo31, 0.0f);
        const float belowTwoTo32 = std::nextafter(twoTo32, 0.0f);
        const float belowTwoTo63 = std::nextafter(twoTo63, 0.0f);
        const float belowTwoTo64 = std::nextafter(twoTo64, 0.0f);
        expect(convert_safely<std::int32_t>(belowTwoTo31) == static_cast<std::int32_t>(belowTwoTo31)) << "largest float below 2^31 into int32";
        expect(convert_safely<std::uint32_t>(belowTwoTo32) == static_cast<std::uint32_t>(belowTwoTo32)) << "largest float below 2^32 into uint32";
        expect(convert_safely<std::int64_t>(belowTwoTo63) == static_cast<std::int64_t>(belowTwoTo63)) << "largest float below 2^63 into int64";
        expect(convert_safely<std::uint64_t>(belowTwoTo64) == static_cast<std::uint64_t>(belowTwoTo64)) << "largest float below 2^64 into uint64";
    };

    "a floating-point value that is not a finite integer is refused"_test = [] {
        expect(!convert_safely<std::int64_t>(std::numeric_limits<double>::quiet_NaN()).has_value()) << "NaN";
        expect(!convert_safely<std::int64_t>(std::numeric_limits<double>::infinity()).has_value()) << "infinity";
        expect(!convert_safely<std::uint64_t>(-std::numeric_limits<double>::infinity()).has_value()) << "negative infinity";
        expect(!convert_safely<std::int32_t>(1.5).has_value()) << "1.5";
        expect(!convert_safely<std::uint32_t>(-0.5f).has_value()) << "-0.5";
    };

    "an integer setting refuses a value outside its type's range"_test = [] {
        ConversionBlock block;
        block.init(std::make_shared<gr::Sequence>());

        const std::string fromUnsigned = refusalOf(block, {{"signed_count", std::numeric_limits<std::uint64_t>::max()}});
        expect(fromUnsigned.contains("signed_count")) << fromUnsigned;
        const std::string fromDouble = refusalOf(block, {{"unsigned_count", 0x1p64}});
        expect(fromDouble.contains("unsigned_count")) << fromDouble;
        const std::string fraction = refusalOf(block, {{"unsigned_count", 2.5}});
        expect(fraction.contains("not a whole number")) << fraction;
        expect(eq(block.settings.signed_count, std::int64_t{0}) && eq(block.settings.unsigned_count, std::uint64_t{0})) << "a refused value reaches the member";
    };
};

const boost::ut::suite<"sequence and size settings"> sequenceTests = [] {
    using namespace boost::ut;

    // an untagged YAML list parses to a tensor of type-erased values, whatever its elements hold
    "a type-erased value sequence converts element by element"_test = [] {
        const auto      toSequence = [](const std::vector<pmt::Value>& elements) { return pmt::Value(gr::Tensor<pmt::Value>(elements.begin(), elements.end())); };
        ConversionBlock block;
        block.init(std::make_shared<gr::Sequence>());

        const std::string accepted = refusalOf(block, {{"taps", toSequence({pmt::Value(0.5), pmt::Value(std::int64_t{2}), pmt::Value(0.25f)})}, {"weights", toSequence({pmt::Value(std::int64_t{3}), pmt::Value(0.5f)})}});
        expect(accepted.empty()) << accepted;
        expect(block.settings.taps == std::vector<float>{0.5f, 2.0f, 0.25f}) << "the taps take the converted elements";
        expect(block.settings.weights == std::array<double, 2UZ>{3.0, 0.5}) << "the weights take the converted elements";

        expect(!refusalOf(block, {{"weights", toSequence({pmt::Value(3.0), pmt::Value(0.5), pmt::Value(1.0)})}}).empty()) << "a sequence whose length differs from the array's refuses";

        const std::string refused = refusalOf(block, {{"taps", toSequence({pmt::Value(0.5), pmt::Value(std::pmr::string("two"))})}});
        expect(refused.contains("taps")) << "an element the scalar rule would refuse refuses the sequence: " << refused;
        expect(block.settings.taps == std::vector<float>{0.5f, 2.0f, 0.25f}) << "a refused sequence reaches the member";
    };

    "a native size_t setting takes a value given as gr::Size_t on every platform"_test = [] {
        constexpr std::size_t expected = 37UZ;
        ConversionBlock       block;
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = refusalOf(block, {{"num_filters", static_cast<gr::Size_t>(expected)}}); // a pmt::Value holds no native size_t on every platform
        expect(refusal.empty()) << refusal;
        expect(eq(block.settings.num_filters, expected));
        expect(block.get<std::size_t>("num_filters") == expected) << "the value in force reads back as a native size_t";
    };
};

enum class Waveform { sine, square, triangle };

template<typename T>
[[nodiscard]] std::expected<T, std::string> fromText(std::string_view text) {
    return pmt::convert_safely<T>(pmt::Value{text});
}

template<typename T>
void expectConvertsAsString(std::string_view text) {
    using namespace boost::ut;
    const std::expected<T, std::string> fromValue  = fromText<T>(text);
    const std::expected<T, std::string> fromString = pmt::convert_safely<T>(std::string(text));
    expect(fromValue == fromString) << std::format("'{}' to {}: '{}' from the Value, '{}' from the std::string", text, meta::type_name<T>(), fromValue.error_or("a value"), fromString.error_or("a value"));
}

const boost::ut::suite<"text held in a Value"> textValueTests = [] {
    using namespace boost::ut;

    "text converts to a signed and an unsigned integer"_test = [] {
        const std::expected<std::int32_t, std::string> signedValue   = fromText<std::int32_t>("-42");
        const std::expected<gr::Size_t, std::string>   unsignedValue = fromText<gr::Size_t>(" 4096 # FFT size");
        expect(signedValue == std::int32_t{-42}) << signedValue.error_or("a different value");
        expect(unsignedValue == gr::Size_t{4096}) << unsignedValue.error_or("a different value");
    };

    "text converts to float and double"_test = [] {
        const std::expected<float, std::string>  floatValue  = fromText<float>("1.5");
        const std::expected<double, std::string> doubleValue = fromText<double>("-0.1");
        expect(floatValue == 1.5f) << floatValue.error_or("a different value");
        expect(doubleValue == -0.1) << doubleValue.error_or("a different value");
    };

    "text converts to bool"_test = [] {
        const std::expected<bool, std::string> trueValue  = fromText<bool>("True");
        const std::expected<bool, std::string> falseValue = fromText<bool>("0");
        expect(trueValue == true) << trueValue.error_or("a different value");
        expect(falseValue == false) << falseValue.error_or("a different value");
    };

    "text converts to an enum"_test = [] {
        const std::expected<Waveform, std::string> waveform = fromText<Waveform>("square");
        expect(waveform == Waveform::square) << waveform.error_or("a different value");
    };

    "a Value holding text converts as the same std::string does"_test = [] {
        constexpr std::array texts{"42", "-42", "1.5", " 7 # comment", "true", "0", "square", "", "fast", "1e40", "300", "-1"};
        for (std::string_view text : texts) {
            expectConvertsAsString<std::int8_t>(text);
            expectConvertsAsString<std::uint8_t>(text);
            expectConvertsAsString<std::int32_t>(text);
            expectConvertsAsString<std::uint32_t>(text);
            expectConvertsAsString<std::int64_t>(text);
            expectConvertsAsString<std::uint64_t>(text);
            expectConvertsAsString<float>(text);
            expectConvertsAsString<double>(text);
            expectConvertsAsString<bool>(text);
            expectConvertsAsString<Waveform>(text);
            expectConvertsAsString<std::string>(text);
            expectConvertsAsString<std::complex<float>>(text);
        }
    };

    "a std::string_view is parsed within its bounds"_test = [] {
        const std::string_view                  prefix = std::string_view("1.25").substr(0UZ, 3UZ);
        const std::expected<float, std::string> parsed = pmt::convert_safely<float>(prefix);
        expect(parsed == 1.2f) << parsed.error_or("a different value");
    };

    "the strict and the numeric conversions still refuse text"_test = [] {
        const pmt::Value text{std::string_view("1.5")};
        expect(!pmt::convert_safely<float, true>(text).has_value());
        expect(!pmt::convert_numerically<float>(text).has_value());
        expect(!pmt::convert_numerically<std::int32_t>(text).has_value());
    };
};

// the core's settings of a scheduler and one of ChunkScheduler's own
struct ChunkSchedulerSettings : gr::scheduler::SchedulerSettings {
    Annotated<gr::Size_t, Doc<"samples a worker takes per pass">, Limits<Min<1U>, Max<64U>>, StoppedOnly> chunk_size = 4U;

    GR_MAKE_REFLECTABLE(ChunkSchedulerSettings, chunk_size);
};

// a scheduler with a setting of its own: the check refuses a chunk of 3, and the apply hook counts its calls and keeps
// the chunk in force
struct ChunkScheduler : gr::scheduler::SchedulerBase<ChunkScheduler, gr::scheduler::ExecutionPolicy::singleThreaded, gr::profiling::null::Profiler, ChunkSchedulerSettings> {
    using Base = gr::scheduler::SchedulerBase<ChunkScheduler, gr::scheduler::ExecutionPolicy::singleThreaded, gr::profiling::null::Profiler, ChunkSchedulerSettings>;
    using Base::_pool;
    using Base::Base;

    std::size_t nApplyCalls  = 0UZ; // an apply before this member's construction is lost when it is constructed
    gr::Size_t  appliedChunk = 0U;

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (s.chunk_size == 3U) {
            return gr::Refused(s.chunk_size, "a chunk of 3 samples splits no frame");
        }
        return std::nullopt;
    }

    void applySettings(const Settings& s) {
        ++nApplyCalls;
        appliedChunk = s.chunk_size;
    }
};

} // namespace qa_settings

const boost::ut::suite<"settings of a derived scheduler"> derivedSchedulerTests = [] {
    using namespace boost::ut;
    using namespace qa_settings;

    "a derived scheduler's own setting joins the core's keys with its limits, documentation and update policy"_test = [] {
        ChunkScheduler scheduler;
        expect(fatal(scheduler.check({}).has_value()));
        expect(eq(scheduler.get<gr::Size_t>("chunk_size").value_or(0U), 4U));
        expect(eq(scheduler.get<gr::Size_t>("timeout_ms").value_or(0U), 100U)) << "a key of the core";
        const property_map& metadata = gr::settings::detail::declarationTable<ChunkScheduler>().keyMetadata;
        expect(metadata.contains("chunk_size::maximum") && metadata.contains("chunk_size::documentation") && metadata.contains("chunk_size::update_policy"));
        expect(metadata.contains("poolName::update_policy")) << "the core's keys keep their declarations";

        expect(!scheduler.set("chunk_size", gr::Size_t{65U}).has_value()) << "a value above the limit was taken";
        expect(!scheduler.set("chunk_size", gr::Size_t{3U}).has_value()) << "a value the check refuses was taken";
        expect(fatal(scheduler.set({{"chunk_size", gr::Size_t{8U}}, {"poolName", std::string(gr::thread_pool::kDefaultIoPoolId)}}).has_value()));
        expect(eq(scheduler.settings.chunk_size.value, 8U));
        expect(eq(scheduler.appliedChunk, 8U)) << "the scheduler's apply hook ran";
        expect(scheduler._pool.get() == gr::thread_pool::Manager::defaultIoPool().get()) << "the core's apply of poolName ran beside the scheduler's hook";
    };

    "a derived scheduler's settings are set up after its members are constructed"_test = [] {
        gr::SchedulerWrapper<ChunkScheduler> wrapped(property_map{{"chunk_size", gr::Size_t{8U}}});
        ChunkScheduler&                      scheduler = *static_cast<ChunkScheduler*>(wrapped.raw());
        expect(eq(scheduler.nApplyCalls, 1UZ)) << "the setup's apply ran before the member was constructed";
        expect(eq(scheduler.appliedChunk, 8U));
    };

    "a value a derived scheduler refuses at construction reaches a wrapper's caller and a scheduler's first use"_test = [] {
        gr::settings::Rejected thrown;
        try {
            gr::SchedulerWrapper<ChunkScheduler> wrapped(property_map{{"chunk_size", gr::Size_t{3U}}});
        } catch (const gr::settings::RefusedSettings& refused) {
            thrown = refused.rejected;
        }
        expect(fatal(eq(thrown.size(), 1UZ))) << "the wrapper's constructor took the value";
        expect(eq(thrown.front().key, std::string("chunk_size")));

        ChunkScheduler                                    byHand({{"chunk_size", gr::Size_t{3U}}});
        const std::expected<void, gr::settings::Rejected> checked = byHand.check({});
        expect(fatal(!checked.has_value())) << "the first use took the value";
        expect(eq(checked.error().front().key, std::string("chunk_size")));

        ChunkScheduler                       started({{"chunk_size", gr::Size_t{3U}}});
        const std::expected<void, gr::Error> run = started.runAndWait();
        expect(fatal(!run.has_value())) << "the start took the value";
        expect(run.error().message.contains("scheduler '") && run.error().message.contains("chunk_size")) << run.error().message;
        expect(!run.error().message.contains("not allowed while running")) << "a scheduler that never ran was told to stop: " << run.error().message;
    };
};

const boost::ut::suite<"settings"> _settings = [] {
    using namespace boost::ut;
    using namespace qa_settings;

    "a rate tag through a decimator arrives scaled"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RateTagSource>(property_map{{"name", std::string("src")}});
        auto&     middle = flow.emplaceBlock<Decimator>(property_map{{"name", std::string("mid")}, {"input_chunk_size", kDecimation}, {"output_chunk_size", gr::Size_t(1)}});
        auto&     sink   = flow.emplaceBlock<RateTagSink>(property_map{{"name", std::string("snk")}});
        qa_coupling::couple(gr::settings::detail::settingsOf(middle), "sample_rate", gr::tag::SAMPLE_RATE.key());
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(ge(sink.rates.size(), 1UZ)) << "the sink must see the rate the decimator publishes at";
        expect(std::ranges::all_of(sink.rates, [](float rate) { return rate == kOutputRate; })) << "every forwarded rate must be the output rate";
        expect(eq(middle.settings.sample_rate.value, kInputRate)) << "the tag leaves the block's own setting at the input rate";
    };

    "a tag that sets a block's chunk ratio sizes the call that starts at its sample"_test = [] {
        expectRatioStream({.nSamples = 200UZ, .tags = {{10UZ, ratioTag(2U, {})}, {40UZ, ratioTag(4U, {})}, {80UZ, ratioTag(1U, {})}, {83UZ, ratioTag(3U, {})}}}, "decimation 1, 2, 4, 1, 3");
        expectRatioStream({.nSamples = 120UZ, .tags = {{8UZ, ratioTag({}, 2U)}, {20UZ, ratioTag(2U, 3U)}, {60UZ, ratioTag(1U, 1U)}}}, "interpolation 2 from sample 8, 3 with decimation 2 from 20, 1:1 from 60");
        expectRatioStream({.nSamples = 1600UZ, .tags = {{0UZ, ratioTag(8U, {})}, {800UZ, ratioTag(16U, {})}}}, "decimation 8 from the stream's first sample, 16 from sample 800");
    };

    "a tag on the first sample after a stride's skip sets the ratio of the call after the skip"_test = [] { expectRatioStream({.nSamples = 100UZ, .tags = {{0UZ, ratioTag(4U, {})}, {30UZ, ratioTag(2U, {})}, {60UZ, ratioTag(5U, 2U)}}, .stride = 10U}, "stride 10, decimation 4, 2, 5"); };

    "a tag at the first sample of calls that move no sample sets its settings once"_test = [] {
        const RatioStream stream{.nSamples = 100UZ, .tags = {{0UZ, ratioTag(4U, {})}, {4UZ, ratioTag(8U, {}, true)}}, .pauseAt = 7UZ, .nPauseCalls = 50UZ};
        const RatioRun    run = expectRatioStream(stream, "decimation 4, then a reset and decimation 8 three samples before a pause");
        expect(eq(run.nResets, 1UZ)) << "the reset runs once, not once per call that waits for the samples of its first chunk";
    };

    "a tag that reaches an unmoved read position after another sets only its own settings"_test = [] {
        constexpr std::size_t kTagIndex = 20UZ;
        const RatioStream     stream{.nSamples = 100UZ, .tags = {{.index = kTagIndex, .map = ratioTag(4U, {}, true), .pastLastSample = true}, {.index = kTagIndex, .map = ratioTag({}, 2U)}}, .pauseAt = kTagIndex, .nPauseCalls = 10UZ};
        const RatioRun        run = expectRatioStream(stream, "a reset and decimation 4 at sample 20 ahead of a pause, then interpolation 2 at the same sample");
        expect(eq(run.nResets, 1UZ)) << "the reset tag acts once, not again when the second tag arrives at its sample";
    };

    "a setting in a tag at the end-of-stream index applies after an epilogue takes the trailing samples"_test = [] {
        constexpr float       kEndLevel = 7.f;
        constexpr std::size_t kTrailing = 3UZ;
        const std::size_t     nSamples  = 5UZ * kEpilogueChunk + kTrailing;
        property_map          levelTag;
        levelTag.insert_or_assign(std::pmr::string(kLevelKey), kEndLevel);

        gr::Graph flow;
        auto&     source  = flow.emplaceBlock<RampSource>(property_map{{"name", std::string("src")}});
        source.nSamples   = nSamples;
        source.tagsToEmit = {{.index = nSamples, .map = levelTag, .pastLastSample = true}};
        auto& copier      = flow.emplaceBlock<EpilogueLevelCopy>(property_map{{"name", std::string("copy")}});
        std::ignore       = copier.setTagCouplings(property_map{{"level", std::string(kLevelKey)}});
        auto& sink        = flow.emplaceBlock<SampleSink>(property_map{{"name", std::string("snk")}});
        expect(fatal(flow.connect<"out", "in">(source, copier).has_value()));
        expect(fatal(flow.connect<"out", "in">(copier, sink).has_value()));

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
        expect(scheduler.runAndWait().has_value()) << "the run ends without an error";
        expect(eq(copier.settings.level.value, kEndLevel)) << "the tag past the trailing samples sets the level";
        expect(eq(sink.samples.size(), nSamples)) << "the epilogue passes the trailing samples on";
    };

    "an unchanged setting tag costs one apply however often it repeats"_test = [] {
        const RateStreamResult shortRun = runRateStream(kShortStream, kInputRate);
        const RateStreamResult longRun  = runRateStream(kLongStream, kInputRate);

        expect(le(shortRun.nSettingsChanged, 1UZ)) << "an unchanged value must apply at most once";
        expect(eq(shortRun.nSettingsChanged, longRun.nSettingsChanged)) << "the apply count must not scale with the number of identical tags";
        expect(eq(shortRun.finalRate, kInputRate));
        expect(eq(longRun.finalRate, kInputRate));
        expect(eq(runRateStream(kLongStream, kRetunedRate).finalRate, kRetunedRate)) << "a new value in the same stream must reach the block's setting";
    };

    "a change names the key that moved and no key that was merely set before"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}, {"n_averages", gr::Size_t(8)}, {"sample_rate", 48000.0f}}};
        block.init(std::make_shared<gr::Sequence>());

        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the initialization is one change";
        expect(block.newSeen.front().contains("fft_size") && !block.newSeen.front().contains("n_workers")) << "the initialization changes the constructed settings from their defaults alone";
        block.oldSeen.clear();
        block.newSeen.clear();

        applySettings(block, {{"n_workers", gr::Size_t(4)}});

        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the new value is one change";
        const property_map& newSettings = block.newSeen.front();
        expect(eq(newSettings.size(), 1UZ)) << "only the key whose value moved is named";
        expect(eq(sizeOf(newSettings, "n_workers").value_or(0U), gr::Size_t(4))) << "the moved key is named with its new value";

        const property_map& oldSettings = block.oldSeen.front();
        expect(eq(sizeOf(oldSettings, "n_workers").value_or(0U), gr::Size_t(1))) << "the previous value of the moved key stays readable";
        expect(eq(block.settings.fft_size.value, gr::Size_t(2048))) << "a key that is not named keeps its value";
    };

    "tags carrying equal vectors compare equal"_test = [] {
        const std::vector<float> taps{0.25f, 0.5f, 0.25f};

        const Tag first{0UZ, property_map{{"taps", taps}}};
        const Tag second{0UZ, property_map{{"taps", taps}}};
        const Tag third{0UZ, property_map{{"taps", std::vector<float>{0.25f, 0.5f}}}};

        expect(first == second) << "equal payloads make equal tags";
        expect(first != third) << "differing payloads make different tags";
    };

    "numeric settings given as text are parsed and applied"_test = [] {
        ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = refusalOf(block, {{"sample_rate", std::string("48000.5")}, {"fft_size", std::string("4096")}});
        expect(refusal.empty()) << refusal;
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));

        expect(eq(block.settings.sample_rate.value, 48000.5f)) << "the float setting takes the parsed value";
        expect(eq(block.settings.fft_size.value, gr::Size_t(4096))) << "the unsigned setting takes the parsed value";
        const std::string blank = refusalOf(block, {{"sample_rate", std::string(" \t")}});
        expect(blank.contains("sample_rate")) << "text that holds no number: " << blank;
    };
};

const boost::ut::suite<"a block copies its own members into its settings"> copyTests = [] {
    using namespace boost::ut;
    using namespace qa_settings;

    "a read on another thread gives the value the block copied from its work function"_test = [] {
        RunningCounter run;
        expect(fatal(run.awaitCopies(1UZ))) << "the counter never passed a sample";

        const property_map settings = run.readSettings();

        expect(gt(valueAt<gr::Size_t>(settings, "n_seen").value_or(0U), gr::Size_t(0))) << "the read gives the declared default, not the copied member";
    };
};

namespace qa_refusals {

using namespace gr;

/// a gain the block takes between 0 and 1
struct LimitedGain : Block<LimitedGain> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Limits<Min<0.f>, Max<1.f>>> gain = 0.5f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(LimitedGain, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

/// a gain the block takes between -1 and 1
struct SignedGain : Block<SignedGain> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Limits<Min<-1.f>, Max<1.f>>> gain = 0.5f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(SignedGain, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }
};

/// a float rate the block takes between 0 and 100, with integral limits
struct IntegralLimitedRate : Block<IntegralLimitedRate> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Limits<Min<0>, Max<100>>> rate = 50.f;

        GR_MAKE_REFLECTABLE(Settings, rate);
    } settings{};

    GR_MAKE_REFLECTABLE(IntegralLimitedRate, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.rate; }
};

/// one setting per form of a declared range, and two rules no predicate states, which the block's check holds
struct LimitForms : Block<LimitForms> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<gr::Size_t, Limits<Min<gr::Size_t{2}>, Max<std::numeric_limits<gr::Size_t>::max()>>>           count      = 2U;
        Annotated<gr::Size_t, Limits<Min<gr::Size_t{0}>, Max<gr::Size_t{64}>>>                                   taps       = 16U;
        Annotated<std::int32_t, Limits<Min<std::numeric_limits<std::int32_t>::lowest()>, Max<std::int32_t{10}>>> offset     = 0;
        Annotated<double, Limits<Min<-1.5>, Max<std::numeric_limits<double>::max()>>>                            level      = 0.0;
        Annotated<float, Limits<Min<0.1f>, Max<0.45f>>>                                                          fraction   = 0.25f;
        Annotated<gr::Size_t, Limits<Min<gr::Size_t{8}>, Max<gr::Size_t{4096}>>>                                 fft_length = 64U;
        Annotated<gr::Size_t, Limits<Min<gr::Size_t{0}>, Max<gr::Size_t{100}>>>                                  pairs      = 4U;

        GR_MAKE_REFLECTABLE(Settings, count, taps, offset, level, fraction, fft_length, pairs);
    } settings{};

    GR_MAKE_REFLECTABLE(LimitForms, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const {
        if (!std::has_single_bit(s.fft_length.value)) {
            return gr::Refused(s.fft_length, "not a power of 2");
        }
        if (s.pairs % 2U != 0U) {
            return gr::Refused(s.pairs, "not even");
        }
        return std::nullopt;
    }
};

enum class Taper { rectangular, hann, hamming };

/// one setting per form of the predicates in Limits
struct PredicateForms : Block<PredicateForms> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<gr::Size_t, Limits<Min<1U>, Max<64U>>>             taps          = 16U;
        Annotated<float, Limits<Over<0.f>, Under<0.5f>>>             cutoff        = 0.25f;
        Annotated<float, Limits<Over<0.f>, Max<1.f>>>                weight        = 1.f;
        Annotated<gr::Size_t, Limits<Min<1U>>>                       length        = 8U;
        Annotated<gr::Size_t, Limits<OneOf<1U, 2U, 4U>>>             decimation    = 2U;
        Annotated<std::int32_t, Limits<Over<0>, OneOf<2, 4, 8>>>     order         = 4;
        Annotated<double, Limits<Over<-3>, Under<std::int64_t{3}>>>  integer_level = 0.0;
        Annotated<Taper, Limits<OneOf<Taper::hann, Taper::hamming>>> taper         = Taper::hann;

        GR_MAKE_REFLECTABLE(Settings, taps, cutoff, weight, length, decimation, order, integer_level, taper);
    } settings{};

    GR_MAKE_REFLECTABLE(PredicateForms, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a count a caller must give, since its default lies outside its limits
struct RequiredCount : Block<RequiredCount> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<gr::Size_t, Limits<Min<1U>>> count = 0U;

        GR_MAKE_REFLECTABLE(Settings, count);
    } settings{};

    GR_MAKE_REFLECTABLE(RequiredCount, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a gain whose default lies outside its limits, beside an offset without limits
struct OutOfLimitsDefault : Block<OutOfLimitsDefault> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Limits<Min<0.f>, Max<1.f>>> gain   = 2.f;
        float                                        offset = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain, offset);
    } settings{};

    GR_MAKE_REFLECTABLE(OutOfLimitsDefault, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain + settings.offset; }
};

/// a gain whose default lies inside its limits, with a reset() and a applySettings() that count their calls
struct CountingResetWithinLimits : Block<CountingResetWithinLimits> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float, Limits<Min<0.f>, Max<1.f>>> gain = 0.5f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(CountingResetWithinLimits, in, out, settings);

    std::size_t resets  = 0;
    std::size_t changes = 0;

    void reset() { ++resets; }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) { ++changes; }
};

/// a gain beside a setting named like the reset key, with a reset() and a applySettings() that count their calls
struct ResetNamedSetting : Block<ResetNamedSetting> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float>                                           gain          = 1.f;
        Annotated<bool, Doc<"a setting named like the reset key">> reset_default = false;

        GR_MAKE_REFLECTABLE(Settings, gain, reset_default);
    } settings{};

    GR_MAKE_REFLECTABLE(ResetNamedSetting, in, out, settings);

    std::size_t resets  = 0;
    std::size_t changes = 0;

    void reset() { ++resets; }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) { ++changes; }
};

/// a tag that carries only `key`, set to true
[[nodiscard]] Tag flagTag(std::string_view key) { return Tag{0UZ, property_map{{std::pmr::string(key), true}}}; }

[[nodiscard]] std::string setRefusal(SettingsBase& settings, const property_map& parameters) {
    const auto result = settings.set(parameters);
    return result.has_value() ? std::string{} : gr::settings::describeRejected(result.error());
}

/// publishes a fixed number of samples and puts `tag`, when it is not empty, on the first
struct FiniteTagSource : Block<FiniteTagSource> {
    PortOut<float> out{};

    GR_MAKE_REFLECTABLE(FiniteTagSource, out);

    property_map tag{};
    std::size_t  nSamples = 1000UZ;
    std::size_t  nEmitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), nSamples - nEmitted);
        std::fill_n(outSpan.begin(), n, 1.0f);
        if (nEmitted == 0UZ && n > 0UZ && !tag.empty()) {
            outSpan.publishTag(tag, 0UZ);
        }
        nEmitted += n;
        outSpan.publish(n);
        return nEmitted == nSamples ? work::Status::DONE : work::Status::OK;
    }
};

/// declares no tag policy and passes its input on
struct UndeclaredPass : Block<UndeclaredPass> {
    PortIn<float>  in{};
    PortOut<float> out{};

    GR_MAKE_REFLECTABLE(UndeclaredPass, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// a gain between 0 and 1 on a sink that counts its samples
struct LimitedSink : Block<LimitedSink> {
    PortIn<float> in{};

    struct Settings {
        Annotated<float, Limits<Min<0.f>, Max<1.f>>> gain = 0.5f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(LimitedSink, in, settings);

    std::size_t nReceived = 0UZ;

    [[nodiscard]] float held() const noexcept { return settings.gain.value; }

    work::Status processBulk(std::span<const float> input) {
        nReceived += input.size();
        return work::Status::OK;
    }
};

enum class SinkMode { fast, slow };

/// a sink that counts its samples, with a mode a host names
struct ModeSink : Block<ModeSink> {
    PortIn<float> in{};

    struct Settings {
        SinkMode mode = SinkMode::fast;

        GR_MAKE_REFLECTABLE(Settings, mode);
    } settings{};

    GR_MAKE_REFLECTABLE(ModeSink, in, settings);

    std::size_t nReceived = 0UZ;

    [[nodiscard]] SinkMode held() const noexcept { return settings.mode; }

    work::Status processBulk(std::span<const float> input) {
        nReceived += input.size();
        return work::Status::OK;
    }
};

/// a sink that counts its samples, with a count its applySettings() refuses at 0 and a level it takes at any value
struct RefusingSink : Block<RefusingSink> {
    PortIn<float> in{};

    struct Settings {
        gr::Size_t count = 4U;
        float      level = 1.f;

        GR_MAKE_REFLECTABLE(Settings, count, level);
    } settings{};

    GR_MAKE_REFLECTABLE(RefusingSink, in, settings);

    std::size_t nReceived = 0UZ;

    [[nodiscard]] gr::Size_t held() const noexcept { return settings.count; }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.count != old.count && s.count == 0U) {
            throw gr::exception("count below 1");
        }
    }

    work::Status processBulk(std::span<const float> input) {
        nReceived += input.size();
        return work::Status::OK;
    }
};

inline constexpr std::string_view kCountKey      = "qa:count";
inline constexpr std::string_view kTakenCountKey = "qa:count_taken";

/// passes its input on and hands on each count it is given under kTakenCountKey; its applySettings() refuses a count
/// of 0 after it filled the forward map
struct RefusingForwarder : Block<RefusingForwarder> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        gr::Size_t count = 4U;

        GR_MAKE_REFLECTABLE(Settings, count);
    } settings{};

    GR_MAKE_REFLECTABLE(RefusingForwarder, in, out, settings);

    void applySettings(const Settings& s, const Settings& old) {
        if (s.count == old.count) {
            return;
        }
        this->forwardTag(kTakenCountKey, s.count);
        if (s.count == 0U) {
            throw gr::exception("count below 1");
        }
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

/// keeps every tag it receives
struct TagRecordingSink : Block<TagRecordingSink> {
    PortIn<float> in{};

    GR_MAKE_REFLECTABLE(TagRecordingSink, in);

    std::vector<Tag> tags;

    work::Status processBulk(InputSpanLike auto& inSpan) {
        for (const Tag& tag : inSpan.rawTags) {
            tags.push_back(tag);
        }
        const std::size_t n = inSpan.size();
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return work::Status::OK;
    }
};

/// the error messages a port received
struct ReportedRefusals {
    std::string errors;
};

[[nodiscard]] ReportedRefusals readRefusals(gr::MsgPortIn& port) {
    ReportedRefusals    reported;
    ReaderSpanLike auto messages = port.streamReader().get<SpanReleasePolicy::ProcessAll>();
    for (const gr::Message& message : messages) {
        if (!message.data.has_value()) {
            reported.errors += message.data.error().message;
        }
    }
    std::ignore = messages.consume(messages.size());
    return reported;
}

template<typename TSink>
struct SinkRun {
    std::expected<void, gr::Error>                result;
    decltype(std::declval<const TSink&>().held()) held{};
    std::size_t                                   nReceived = 0UZ;
    std::size_t                                   nSent     = 0UZ;
    ReportedRefusals                              reported{};
    std::vector<gr::settings::Notification>       notified{};
};

/// runs a source that tags its first sample with `tag` through a block that declares no tag policy into a sink named
/// `sinkName`. The sink couples each key of `tag` to its setting of the same name. A Set message carries `set` to the
/// sink when it is not empty. A reader takes the scheduler's messages when `readMessages` is true. With no reader a
/// block's error ends the run. A notification reader at the scheduler keeps every notification when
/// `readsNotifications` is true.
template<typename TSink>
[[nodiscard]] SinkRun<TSink> runIntoSink(const std::string& sinkName, const property_map& tag, const property_map& set, bool readMessages, bool readsNotifications = false) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<FiniteTagSource>();
    auto&     pass   = flow.emplaceBlock<UndeclaredPass>();
    auto&     sink   = flow.emplaceBlock<TSink>({{"name", sinkName}});
    source.tag       = tag;
    for (const auto& [key, value] : tag) {
        qa_coupling::couple(gr::settings::detail::settingsOf(sink), key, key);
    }
    expect(flow.connect<"out", "in">(source, pass).has_value());
    expect(flow.connect<"out", "in">(pass, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    gr::MsgPortOut                                                        toScheduler;
    gr::MsgPortIn                                                         fromScheduler;
    expect(toScheduler.connect(scheduler.msgIn).has_value());
    if (readMessages) {
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
    }
    if (!set.empty()) {
        gr::sendMessage<gr::message::Command::Set>(toScheduler, sink.unique_name, gr::block::property::kSetting, set);
    }
    expect(scheduler.exchange(std::move(flow)).has_value());
    auto notified = std::make_shared<std::vector<gr::settings::Notification>>(); // the run's thread delivers, and the run ends before the read
    if (readsNotifications) {
        scheduler.installReader([notified](const gr::settings::Notification& notification) { notified->push_back(notification); });
    }

    SinkRun<TSink> run{.result = scheduler.runAndWait()};
    run.notified  = *notified;
    run.held      = sink.held();
    run.nReceived = sink.nReceived;
    run.nSent     = source.nSamples;
    if (readMessages) {
        run.reported = readRefusals(fromScheduler);
    }
    return run;
}

struct ForwarderRun {
    std::expected<void, gr::Error> result;
    gr::Size_t                     held = 0U;
    std::vector<Tag>               tags{};
    ReportedRefusals               reported{};
};

/// runs a source that tags its first sample with kCountKey = `count` through a RefusingForwarder that couples its count
/// to kCountKey, into a sink that keeps every tag
[[nodiscard]] ForwarderRun runThroughRefusingForwarder(gr::Size_t count) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source    = flow.emplaceBlock<FiniteTagSource>();
    auto&     forwarder = flow.emplaceBlock<RefusingForwarder>({{"name", std::string("refusing_forwarder")}});
    auto&     sink      = flow.emplaceBlock<TagRecordingSink>();
    source.tag          = property_map{{std::pmr::string(kCountKey), count}};
    qa_coupling::couple(gr::settings::detail::settingsOf(forwarder), "count", kCountKey);
    expect(flow.connect<"out", "in">(source, forwarder).has_value());
    expect(flow.connect<"out", "in">(forwarder, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    gr::MsgPortIn                                                         fromScheduler;
    expect(scheduler.msgOut.connect(fromScheduler).has_value());
    expect(scheduler.exchange(std::move(flow)).has_value());

    ForwarderRun run{.result = scheduler.runAndWait()};
    run.held     = forwarder.settings.count;
    run.tags     = sink.tags;
    run.reported = readRefusals(fromScheduler);
    return run;
}

/// the tags of `tags` that carry `key`, in the order they arrived
[[nodiscard]] std::vector<Tag> tagsWithKey(const std::vector<Tag>& tags, std::string_view key) {
    std::vector<Tag> found;
    std::ranges::copy_if(tags, std::back_inserter(found), [key](const Tag& tag) { return tag.map.contains(key); });
    return found;
}

inline constexpr int kPercentLower = 0;
inline constexpr int kPercentUpper = 100;
using Percent                      = Limits<Min<kPercentLower>, Max<kPercentUpper>>;

// true when Percent takes Character values at its ends and refuses values just outside them
template<typename Character>
constexpr bool percentTakesCharacterByValue() {
    const bool edgesTaken   = Percent::validate(static_cast<Character>(kPercentLower)) && Percent::validate(static_cast<Character>(kPercentUpper));
    const bool aboveRefused = !Percent::validate(static_cast<Character>(kPercentUpper + 1));
    if constexpr (std::is_signed_v<Character>) {
        return edgesTaken && aboveRefused && !Percent::validate(static_cast<Character>(kPercentLower - 1));
    } else {
        return edgesTaken && aboveRefused;
    }
}

} // namespace qa_refusals

const boost::ut::suite<"settings refusals"> settingsRefusalTests = [] {
    using namespace boost::ut;
    using namespace qa_refusals;

    "a holder of the block's base reaches its settings object through settings()"_test = [] {
        LimitedGain               block{property_map{{"name", std::string("base_holder")}}};
        const Block<LimitedGain>& base = block;
        expect(std::addressof(base.settings()) == std::addressof(gr::settings::detail::settingsOf(block)));
    };

    "integral limits compare a float before converting it"_test = [] {
        constexpr float lower = 0.f; // the ends of IntegralLimitedRate's rate
        constexpr float upper = 100.f;
        for (const float rate : {lower - 0.5f, upper + 0.5f, std::numeric_limits<float>::infinity()}) {
            IntegralLimitedRate block{property_map{{"name", std::string("rate_stage")}}};
            block.init(std::make_shared<gr::Sequence>());
            expect(!setRefusal(gr::settings::detail::settingsOf(block), {{"rate", rate}}).empty()) << std::format("set() takes rate = {}", rate);
        }

        IntegralLimitedRate block{property_map{{"name", std::string("rate_stage")}}};
        block.init(std::make_shared<gr::Sequence>());
        const std::string refusal = setRefusal(gr::settings::detail::settingsOf(block), {{"rate", upper - 0.5f}});
        expect(refusal.empty()) << refusal;
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.rate.value, upper - 0.5f)) << "an accepted value keeps its fraction";

        constexpr std::int32_t                                            narrowUpper = std::numeric_limits<std::int32_t>::max();
        Annotated<double, Limits<Min<std::int32_t{0}>, Max<narrowUpper>>> narrow      = 1.0;
        expect(!narrow.validate_and_set(static_cast<double>(narrowUpper) + 0.5)) << "a fraction above an integral limit the double holds exactly is outside";
    };

    "a declared range is refused naming only the bounds a value of the setting's type can break"_test = [] {
        LimitForms block{property_map{{"name", std::string("forms")}}};
        block.init(std::make_shared<gr::Sequence>());
        SettingsBase& settings = gr::settings::detail::settingsOf(block);

        expect(eq(setRefusal(settings, {{"count", gr::Size_t{1}}}), std::string("count: out of range [2, inf): 1"))) << "an upper bound at the type's largest value";
        expect(eq(setRefusal(settings, {{"taps", gr::Size_t{65}}}), std::string("taps: out of range (-inf, 64]: 65"))) << "a lower bound at the type's smallest value";
        expect(eq(setRefusal(settings, {{"offset", std::int32_t{11}}}), std::string("offset: out of range (-inf, 10]: 11")));
        expect(eq(setRefusal(settings, {{"level", -2.0}}), std::string("level: out of range [-1.5, inf): -2")));
        expect(eq(setRefusal(settings, {{"fraction", 0.5f}}), std::string("fraction: out of range [0.1, 0.45]: 0.5"))) << "a float bound in its shortest text";
        expect(eq(setRefusal(settings, {{"fraction", 0.05}}), std::string("fraction: out of range [0.1, 0.45]: 0.05")));
        expect(eq(setRefusal(settings, {{"fft_length", gr::Size_t{4}}}), std::string("fft_length: out of range [8, 4096]: 4"))) << "a range beside a rule of the block's check";
    };

    "predicates in Limits refuse a value with one text naming each of them"_test = [] {
        PredicateForms block{property_map{{"name", std::string("predicates")}}};
        block.init(std::make_shared<gr::Sequence>());
        SettingsBase& settings = gr::settings::detail::settingsOf(block);

        expect(eq(setRefusal(settings, {{"taps", gr::Size_t{0}}}), std::string("taps: out of range [1, 64]: 0")));
        expect(eq(setRefusal(settings, {{"taps", gr::Size_t{65}}}), std::string("taps: out of range [1, 64]: 65")));
        expect(setRefusal(settings, {{"taps", gr::Size_t{1}}}).empty() && setRefusal(settings, {{"taps", gr::Size_t{64}}}).empty()) << "Min and Max include their value";
        expect(eq(setRefusal(settings, {{"cutoff", 0.f}}), std::string("cutoff: out of range (0, 0.5): 0"))) << "Over excludes its value";
        expect(eq(setRefusal(settings, {{"cutoff", 0.5f}}), std::string("cutoff: out of range (0, 0.5): 0.5"))) << "Under excludes its value";
        expect(setRefusal(settings, {{"cutoff", std::nextafter(0.5f, 0.f)}}).empty());
        expect(eq(setRefusal(settings, {{"weight", 0.f}}), std::string("weight: out of range (0, 1]: 0")));
        expect(setRefusal(settings, {{"weight", 1.f}}).empty());
        expect(eq(setRefusal(settings, {{"length", gr::Size_t{0}}}), std::string("length: out of range [1, inf): 0")));
        expect(eq(setRefusal(settings, {{"decimation", gr::Size_t{3}}}), std::string("decimation: not one of: 1, 2, 4: 3")));
        expect(setRefusal(settings, {{"decimation", gr::Size_t{4}}}).empty());
        expect(eq(setRefusal(settings, {{"order", 3}}), std::string("order: not one of: 2, 4, 8: 3")));
    };

    "OneOf on an enumeration takes the enumerators it lists and refuses the others by name"_test = [] {
        PredicateForms block{property_map{{"name", std::string("taper")}}};
        block.init(std::make_shared<gr::Sequence>());
        SettingsBase& settings = gr::settings::detail::settingsOf(block);

        expect(eq(setRefusal(settings, {{"taper", std::string("rectangular")}}), std::string("taper: not one of: hann, hamming: rectangular")));
        expect(setRefusal(settings, {{"taper", std::string("hamming")}}).empty());
        expect(block.get<std::string>("taper") == std::string("hamming"));
        expect(!setRefusal(settings, {{"taper", std::string("kaiser")}}).empty()) << "a name the enumeration lacks";
        using TaperLimits = decltype(PredicateForms::Settings::taper)::LimitType;
        static_assert(TaperLimits::validate(Taper::hann) && TaperLimits::validate(Taper::hamming) && !TaperLimits::validate(Taper::rectangular));
        static_assert(!Limits<OneOf<1, 2>>::validate(Taper::hann), "a number never equals an enumerator");
    };

    "a NaN fails every predicate, and a number of another type is compared exactly"_test = [] {
        PredicateForms block{property_map{{"name", std::string("predicates")}}};
        block.init(std::make_shared<gr::Sequence>());
        SettingsBase& settings = gr::settings::detail::settingsOf(block);

        const float nan = std::numeric_limits<float>::quiet_NaN();
        expect(eq(setRefusal(settings, {{"cutoff", nan}}), std::string("cutoff: not a number")));
        expect(eq(setRefusal(settings, {{"weight", nan}}), std::string("weight: not a number")));
        static_assert(!decltype(PredicateForms::Settings::cutoff)::LimitType::validate(std::numeric_limits<double>::quiet_NaN()));
        expect(!Limits<Max<1.f>>::validate(nan) && !Limits<OneOf<1.f>>::validate(nan)); // at run time: MSVC 19.51 folds `NaN <= 1.f` to true in a constant expression

        using IntegerEnds = decltype(PredicateForms::Settings::integer_level)::LimitType;
        static_assert(IntegerEnds::validate(-2.5) && IntegerEnds::validate(2.5));
        static_assert(!IntegerEnds::validate(-3.0) && !IntegerEnds::validate(3.0), "an excluded integer end refuses the float equal to it");
        expect(IntegerEnds::validate(std::nextafter(-3.0, 0.0)) && IntegerEnds::validate(std::nextafter(3.0, 0.0)));
        static_assert(!Limits<Over<std::uint32_t{0}>>::validate(-0.5) && Limits<Over<std::uint32_t{0}>>::validate(0.5));
        static_assert(!Limits<Under<std::int64_t{1} << 62>>::validate(0x1p62) && Limits<Under<std::int64_t{1} << 62>>::validate(0x1p61));
    };

    "a count set to its default outside the limits is refused before and after a valid value"_test = [] {
        RequiredCount block{property_map{{"name", std::string("set_zero")}}};
        SettingsBase& settings = gr::settings::detail::settingsOf(block);
        expect(eq(setRefusal(settings, {{"count", gr::Size_t{0U}}}), std::string("count: out of range [1, inf): 0"))) << "set() accepts the default outside the limits";
        expect(!block.ready().has_value()) << "a refused set() makes the block ready";
        expect(setRefusal(settings, {{"count", gr::Size_t{2U}}}).empty());
        expect(block.ready().has_value());
        expect(eq(setRefusal(settings, {{"count", gr::Size_t{0U}}}), std::string("count: out of range [1, inf): 0")));
        expect(block.get<gr::Size_t>("count") == gr::Size_t{2U}) << "the refused value is in force";
        expect(!block.check({{"count", gr::Size_t{0U}}}).has_value()) << "check() accepts the default outside the limits";

        RequiredCount given{property_map{{"name", std::string("given_zero")}, {"count", gr::Size_t{0U}}}};
        expect(!given.ready().has_value()) << "a count given at construction equal to its default outside the limits is not judged as the default";
    };

    "a character or bool value compares with integral limits by its integer value"_test = [] {
        static_assert(Percent::validate(char{5}));
        static_assert(percentTakesCharacterByValue<char>());
        static_assert(percentTakesCharacterByValue<signed char>());
        static_assert(percentTakesCharacterByValue<unsigned char>());
        static_assert(percentTakesCharacterByValue<wchar_t>());
        static_assert(percentTakesCharacterByValue<char8_t>());
        static_assert(percentTakesCharacterByValue<char16_t>());
        static_assert(percentTakesCharacterByValue<char32_t>());
        static_assert(Percent::validate(false) && Percent::validate(true));
        static_assert(!Limits<Min<2>, Max<100>>::validate(true));

        using Digit = Limits<Min<'0'>, Max<'9'>>;
        static_assert(Digit::validate(int{'5'}));
        static_assert(!Digit::validate(int{'0'} - 1) && !Digit::validate(int{'9'} + 1));

        Annotated<char, Percent> letter = char{5};
        expect(letter.validate_and_set(static_cast<char>(kPercentUpper)));
        expect(!letter.validate_and_set(static_cast<char>(kPercentUpper + 1)));
        expect(eq(letter.value, static_cast<char>(kPercentUpper))) << "the refused value does not reach the member";
    };

    "a refused set() that renames the block leaves it the name it has"_test = [] {
        LimitedGain block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = setRefusal(gr::settings::detail::settingsOf(block), {{"name", std::string("renamed")}, {"gain", 2.0f}});
        expect(refusal.contains("out of range [0, 1]: ")) << refusal;
        expect(eq(std::string(block.name), std::string("gain_stage"))) << "a refused call renames the block";
        expect(block.get<std::string>("name") == std::string("gain_stage")) << "a refused call renames the block in its settings";
    };

    "a block constructed with a value outside the limits does not join the graph"_test = [] {
        gr::Graph   flow;
        std::string refusal;
        try {
            std::ignore = flow.emplaceBlock<LimitedGain>({{"name", std::string("stage")}, {"gain", 2.0f}});
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("block 'stage'")) << "the refusal names the block by the name the map gives" << refusal;
        expect(refusal.contains("out of range [0, 1]: ")) << refusal;
        expect(flow.blocks().empty()) << "the refused block is in the graph";
    };

    "a tag value the limits refuse leaves the member, and the run completes"_test = [] {
        const property_map tag{{"gain", 2.0f}};

        const SinkRun<LimitedSink> unread = runIntoSink<LimitedSink>("limited_sink", tag, {}, false);
        expect(unread.result.has_value()) << "the refused tag value ends the run: " << (unread.result.has_value() ? std::string() : unread.result.error().message);
        expect(eq(unread.held, 0.5f)) << "the refused value reaches the member";
        expect(eq(unread.nReceived, unread.nSent)) << "the sink misses samples";

        const SinkRun<LimitedSink> read = runIntoSink<LimitedSink>("limited_sink", tag, {}, true);
        expect(read.result.has_value());
        expect(read.reported.errors.empty()) << "the refused tag value is reported as an error: " << read.reported.errors;
    };

    "a Set message whose value the limits refuse leaves the member, and the run completes"_test = [] {
        // a Set without a request id gets no reply: its refusal goes out as a notification, which ends no run
        const SinkRun<LimitedSink> run = runIntoSink<LimitedSink>("limited_sink", {}, {{"gain", 2.0f}}, false, true);
        expect(run.result.has_value()) << "a refused Set message ended the run: " << (run.result.has_value() ? std::string() : run.result.error().message);
        expect(eq(run.held, 0.5f)) << "the refused value reaches the member";
        expect(eq(run.nReceived, run.nSent)) << "the sink misses samples";
        const auto refusal = [](const gr::settings::Notification& notification) { return notification.level == gr::settings::NotificationLevel::error && notification.what == gr::settings::NotificationKind::failed && notification.key == "gain" && notification.reason == "out of range [0, 1]: 2"; };
        expect(eq(static_cast<std::size_t>(std::ranges::count_if(run.notified, refusal)), 1UZ)) << "the refusal did not reach the reader once";
    };

    "a Set message that names no enumerator is refused with the text set() gives"_test = [] {
        const SinkRun<ModeSink> run = runIntoSink<ModeSink>("mode_sink", {}, {{"mode", std::string("sideways")}}, false, true);
        expect(run.result.has_value()) << "a refused Set message ended the run: " << (run.result.has_value() ? std::string() : run.result.error().message);
        expect(run.held == SinkMode::fast) << "the refused name reaches the member";
        const auto refusal = [](const gr::settings::Notification& notification) { return notification.level == gr::settings::NotificationLevel::error && notification.what == gr::settings::NotificationKind::failed && notification.key == "mode" && notification.reason == "not one of: fast, slow: sideways"; };
        expect(eq(static_cast<std::size_t>(std::ranges::count_if(run.notified, refusal)), 1UZ)) << "the refusal did not reach the reader once";
    };

    "a coupled tag value applySettings() refuses leaves on the tag as it arrived, and the forward map stays behind"_test = [] {
        const gr::Size_t initial = RefusingForwarder{}.settings.count;
        const gr::Size_t taken   = initial + 2U;

        const ForwarderRun accepted = runThroughRefusingForwarder(taken);
        expect(fatal(accepted.result.has_value()));
        expect(eq(accepted.held, taken)) << "the coupled tag does not set the count";
        const auto arrived = std::ranges::find_if(accepted.tags, [](const Tag& tag) { return tag.map.contains(kCountKey); });
        const auto handed  = std::ranges::find_if(accepted.tags, [](const Tag& tag) { return tag.map.contains(kTakenCountKey); });
        expect(fatal(arrived != accepted.tags.end() && handed != accepted.tags.end())) << "the sink misses the tag or the forward map";
        expect(arrived < handed) << "the forward map leaves before the tag";
        expect(eq(arrived->index, 0UZ) && eq(handed->index, 0UZ)) << "the tag and the forward map leave apart from the tagged sample";
        expect(eq(arrived->map.at(std::pmr::string(kCountKey)).value_or(gr::Size_t{0U}), taken));
        expect(eq(handed->map.at(std::pmr::string(kTakenCountKey)).value_or(gr::Size_t{0U}), taken));

        const gr::Size_t   refused = 0U;
        const ForwarderRun run     = runThroughRefusingForwarder(refused);
        expect(fatal(run.result.has_value())) << "the refused tag value ends the run";
        expect(eq(run.held, initial)) << "the refused value stays in the member";
        expect(run.reported.errors.empty()) << "the refused tag value is reported as an error: " << run.reported.errors;
        const std::vector<Tag> counts = tagsWithKey(run.tags, kCountKey);
        expect(fatal(eq(counts.size(), 1UZ)));
        expect(eq(counts[0].map.at(std::pmr::string(kCountKey)).value_or(initial), refused)) << "the tag does not leave with the value it arrived with";
        expect(std::ranges::none_of(tagsWithKey(run.tags, kTakenCountKey), [](const Tag& tag) { return tag.map.at(std::pmr::string(kTakenCountKey)).value_or(gr::Size_t{1U}) == refused; })) << "the forward map of the refused change leaves";
    };

    "a value a Set message stages and applySettings() refuses leaves the member, and the run completes"_test = [] {
        const gr::Size_t            initial = RefusingSink{}.settings.count;
        const SinkRun<RefusingSink> run     = runIntoSink<RefusingSink>("refusing_sink", {}, {{"count", gr::Size_t{0U}}}, false);
        expect(run.result.has_value()) << "a failed apply ends the run: " << (run.result.has_value() ? std::string() : run.result.error().message);
        expect(eq(run.held, initial)) << "the refused value stays in the member";
    };

    "a scheduler constructed with an undeclared key is refused naming the nearest key"_test = [] {
        std::string refusal;
        try {
            gr::scheduler::Simple<> scheduler({{"timeout_mss", gr::Size_t(5)}});
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("declares no setting named 'timeout_mss'")) << refusal;
        expect(refusal.contains("'timeout_ms'")) << "the nearest key is offered" << refusal;
    };

    "a merged block given a value both its blocks refuse names both refusals"_test = [] {
        std::string refusal;
        try {
            gr::Merge<LimitedGain, "out", SignedGain, "in"> merged{property_map{{"gain", 2.0f}}};
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("out of range [0, 1]: ")) << "the first block's refusal is missing: " << refusal;
        expect(refusal.contains("out of range [-1, 1]: ")) << "the second block's refusal is missing: " << refusal;
    };

    "a reset_default tag in either spelling of its key restores the default and calls the block's reset() once"_test = [] {
        for (const std::string_view key : {std::string(gr::tag::RESET_DEFAULTS.key()), std::format(":{}", gr::tag::RESET_DEFAULTS.key())}) {
            CountingResetWithinLimits block{property_map{{"name", std::string("gain_stage")}}};
            gr::SettingsBase&         settings = gr::settings::detail::settingsOf(block);
            std::ignore                        = settings.check({});
            const float defaultGain            = block.settings.gain.value;
            std::ignore                        = settings.set({{"gain", defaultGain / 2.f}});
            std::ignore                        = gr::testing::applyStagedForTest(settings);
            expect(eq(block.settings.gain.value, defaultGain / 2.f)) << "the value to reset does not reach the member";

            const std::size_t resetsBefore = block.resets;
            qa_settings::enterTag(settings, flagTag(key));
            expect(gr::settings::detail::FrameworkAccess::hasWork(settings)) << key << " leaves the settings unchanged";
            const gr::settings::Rejected result = gr::testing::applyStagedForTest(settings);
            expect(result.empty()) << gr::settings::describeRejected(result);
            expect(eq(block.settings.gain.value, defaultGain)) << key << " leaves the member off its default";
            expect(eq(block.resets, resetsBefore + 1UZ)) << key << " calls reset() a number of times other than once";

            std::ignore = settings.resetDefaults();
            expect(eq(block.resets, resetsBefore + 2UZ)) << "resetDefaults() calls reset() a number of times other than once";
        }
    };

    "a batch with a reset_default tag and a value staged after it is reported as one change"_test = [] {
        qa_settings::ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());
        const gr::Size_t defaultSize     = block.settings.fft_size.value;
        const gr::Size_t defaultAverages = block.settings.n_averages.value;
        qa_settings::applySettings(block, {{"fft_size", 2U * defaultSize}, {"n_averages", 2U * defaultAverages}});
        expect(eq(block.settings.fft_size.value, 2U * defaultSize) && eq(block.settings.n_averages.value, 2U * defaultAverages)) << "the values to reset do not reach the members";
        block.oldSeen.clear();
        block.newSeen.clear();
        std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING); // the staged value waits for the reset tag

        qa_settings::enterTag(gr::settings::detail::settingsOf(block), flagTag(gr::tag::RESET_DEFAULTS.key()));
        std::ignore = block.set({{"n_averages", 4U * defaultAverages}}, gr::settings::noWait); // the later of the two holds the key
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.fft_size.value, defaultSize)) << "the reset leaves fft_size off its default";
        expect(eq(block.settings.n_averages.value, 4U * defaultAverages)) << "the value staged with the reset does not reach the member";
        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the batch is reported as a number of changes other than one";
        expect(eq(qa_settings::sizeOf(block.newSeen.front(), "fft_size").value_or(0U), defaultSize)) << "the change names a restored value other than the default";
        expect(eq(qa_settings::sizeOf(block.newSeen.front(), "n_averages").value_or(0U), 4U * defaultAverages)) << "the change names a value other than the staged one";
        expect(eq(qa_settings::sizeOf(block.oldSeen.front(), "fft_size").value_or(0U), 2U * defaultSize)) << "the change is not against the values before the batch";
        expect(eq(qa_settings::sizeOf(block.oldSeen.front(), "n_averages").value_or(0U), 2U * defaultAverages)) << "the change is not against the values before the batch";
    };

    "a setting named reset_default is an ordinary setting"_test = [] {
        ResetNamedSetting block;
        block.init(std::make_shared<gr::Sequence>());
        const std::size_t changesBefore = block.changes;
        const std::size_t resetsBefore  = block.resets;
        const bool        setValue      = !block.settings.reset_default.value;

        expect(block.set({{gr::tag::RESET_DEFAULTS.name(), setValue}}, gr::settings::noWait).has_value()) << "the setting is not admitted";
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.reset_default.value, setValue)) << "the value does not reach the member";
        expect(eq(block.changes, changesBefore + 1UZ)) << "applySettings() runs a number of times other than once";
        expect(eq(block.resets, resetsBefore)) << "the setting resets the block";
        qa_settings::enterTag(gr::settings::detail::settingsOf(block), flagTag(gr::tag::RESET_DEFAULTS.name()));
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.resets, resetsBefore)) << "a tag under the bare name resets the block";

        const auto notSet = block.set({{gr::tag::RESET_DEFAULTS.key(), true}}, gr::settings::noWait);
        expect(!notSet.has_value() && gr::settings::describeRejected(notSet.error()).contains(gr::tag::RESET_DEFAULTS.key())) << "set() admits the gr: form of the reset key";
    };

    "a reset to a default the limits refuse is refused and leaves every setting as it was"_test = [] {
        OutOfLimitsDefault block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());
        std::ignore = block.set({{"gain", 0.5f}, {"offset", 3.0f}});
        std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(block));
        expect(eq(block.settings.gain.value, 0.5f) && eq(block.settings.offset, 3.0f)) << "the values to reset do not reach the members";

        const auto        reset   = block.resetDefaults();
        const std::string refusal = reset.has_value() ? std::string{} : gr::settings::describeRejected(reset.error());
        expect(eq(refusal, std::string("gain: out of range [0, 1]: 2")));
        expect(eq(block.settings.gain.value, 0.5f)) << "the refused default reaches the member";
        expect(eq(block.settings.offset, 3.0f)) << "the reset applies the other defaults";
    };
};

namespace qa_follows_tag {
using namespace gr;

inline constexpr std::string_view kLevelKey = "foo:level";

/// a sample rate, a level and a gain, none coupled to a tag key until a test couples it
struct FollowingBlock : Block<FollowingBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float> sample_rate = 1.f;
        Annotated<float> level       = 0.f;
        Annotated<float> gain        = 1.f;

        GR_MAKE_REFLECTABLE(Settings, sample_rate, level, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(FollowingBlock, in, out, settings);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

[[nodiscard]] Tag tagOf(std::string_view key, float value) { return Tag{0UZ, property_map{{std::pmr::string(key), value}}}; }

/// a scheduler as a block's settings see it, holding every notification at info and above; the test thread alone
/// drives the block
struct NotificationRecorder : gr::settings::detail::SchedulerLink {
    std::vector<gr::settings::Notification> held{};
    int                                     wakes = 0;

    void                                           wake() override { ++wakes; }
    void                                           push(gr::settings::Notification notification) override { held.push_back(std::move(notification)); }
    bool                                           isWorkerThread() const override { return false; }
    std::optional<gr::settings::NotificationLevel> level() const override { return gr::settings::NotificationLevel::info; }
    std::chrono::milliseconds                      defaultTimeout() const override { return gr::settings::kDefaultTimeout; }
};

/// couples sample_rate to gr:sample_rate and level to foo:level
void coupleRateAndLevel(SettingsBase& settings) { std::ignore = settings.setTagCouplings({{"sample_rate", std::string(gr::tag::SAMPLE_RATE.key())}, {"level", std::string(kLevelKey)}}); }

/// the key a setting is coupled to, empty for none
[[nodiscard]] std::string keyOf(const SettingsBase& settings, std::string_view setting) {
    const property_map table = settings.tagCouplings();
    const auto         it    = table.find(setting);
    return it != table.end() ? it->second.value_or(std::string()) : std::string();
}

/// the reasons setTagCouplings() refuses `couplings` with, empty when it takes them
[[nodiscard]] std::string couplingRefusal(SettingsBase& settings, const property_map& couplings) {
    const std::expected<void, gr::settings::Rejected> result = settings.setTagCouplings(couplings);
    return result.has_value() ? std::string{} : gr::settings::describeRejected(result.error());
}

/// publishes `nSamples` samples and puts `tag` on the sample at `tagIndex`
struct IndexedTagSource : Block<IndexedTagSource> {
    PortOut<float> out{};

    GR_MAKE_REFLECTABLE(IndexedTagSource, out);

    property_map tag{};
    std::size_t  tagIndex = 0UZ;
    std::size_t  nSamples = 0UZ;
    std::size_t  nEmitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        const std::size_t n = std::min(outSpan.size(), nSamples - nEmitted);
        std::fill_n(outSpan.begin(), n, 1.0f);
        if (tagIndex >= nEmitted && tagIndex < nEmitted + n) {
            outSpan.publishTag(tag, tagIndex - nEmitted);
        }
        nEmitted += n;
        outSpan.publish(n);
        return nEmitted == nSamples ? work::Status::DONE : work::Status::OK;
    }
};

/// records its names setting at each sample it takes
struct NamesRecordingSink : Block<NamesRecordingSink> {
    PortIn<float> in{};

    struct Settings {
        std::vector<std::string> names = {"untagged"};

        GR_MAKE_REFLECTABLE(Settings, names);
    } settings{};

    GR_MAKE_REFLECTABLE(NamesRecordingSink, in, settings);

    std::vector<std::vector<std::string>> namesAtSample{};

    work::Status processBulk(std::span<const float> input) {
        namesAtSample.insert(namesAtSample.end(), input.size(), settings.names);
        return work::Status::OK;
    }
};

/// the names separated by commas
[[nodiscard]] std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += text.empty() ? name : ", " + name;
    }
    return text;
}

/// the names a sink coupled to `key` held at each sample, when a source puts `tag` on sample `tagIndex` of `nSamples`
[[nodiscard]] std::vector<std::vector<std::string>> namesFollowingTag(std::string_view key, const property_map& tag, std::size_t tagIndex, std::size_t nSamples) {
    using namespace boost::ut;
    Graph flow;
    auto& source    = flow.emplaceBlock<IndexedTagSource>();
    auto& sink      = flow.emplaceBlock<NamesRecordingSink>();
    source.tag      = tag;
    source.tagIndex = tagIndex;
    source.nSamples = nSamples;
    std::ignore     = sink.setTagCouplings(property_map{{"names", std::string(key)}});
    expect(fatal(flow.connect<"out", "in">(source, sink).has_value()));
    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(fatal(scheduler.exchange(std::move(flow)).has_value()));
    expect(fatal(scheduler.runAndWait().has_value()));
    return sink.namesAtSample;
}
} // namespace qa_follows_tag

const boost::ut::suite<"keys that follow tags"> autoUpdateKeyTests = [] {
    using namespace boost::ut;
    using namespace qa_refusals;

    "a tag sets a setting only through the key the setting is coupled to"_test = [] {
        using namespace qa_follows_tag;
        FollowingBlock    coupled;
        gr::SettingsBase& coupledSettings = gr::settings::detail::settingsOf(coupled);
        FollowingBlock    uncoupled;
        gr::SettingsBase& uncoupledSettings = gr::settings::detail::settingsOf(uncoupled);
        std::ignore                         = coupledSettings.check({});
        std::ignore                         = uncoupledSettings.check({});
        coupleRateAndLevel(coupledSettings);

        const float taggedRate = 2.f * coupled.settings.sample_rate.value;
        qa_settings::enterTag(coupledSettings, tagOf(gr::tag::SAMPLE_RATE.key(), taggedRate));
        qa_settings::enterTag(uncoupledSettings, tagOf(gr::tag::SAMPLE_RATE.key(), taggedRate));
        std::ignore = gr::testing::applyStagedForTest(coupledSettings);
        std::ignore = gr::testing::applyStagedForTest(uncoupledSettings);
        expect(eq(coupled.settings.sample_rate.value, taggedRate)) << "the coupled setting does not take its key";
        expect(neq(uncoupled.settings.sample_rate.value, taggedRate)) << "a setting named sample_rate on a block with no coupling takes the gr: key";

        const float bareValue = 2.f * taggedRate;
        for (const std::string_view bareKey : {std::string_view(gr::tag::SAMPLE_RATE.name()), gr::tag::nameOf(kLevelKey), std::string_view("gain")}) {
            qa_settings::enterTag(coupledSettings, tagOf(bareKey, bareValue));
        }
        std::ignore = gr::testing::applyStagedForTest(coupledSettings);
        expect(eq(coupled.settings.sample_rate.value, taggedRate)) << "a bare sample_rate key sets the setting coupled to gr:sample_rate";
        expect(neq(coupled.settings.level.value, bareValue)) << "a bare level key sets the setting coupled to foo:level";
        expect(neq(coupled.settings.gain.value, bareValue)) << "a key equal to an uncoupled setting's name sets it";

        const float foreignLevel = coupled.settings.level.value + 1.f;
        qa_settings::enterTag(coupledSettings, tagOf(kLevelKey, foreignLevel));
        std::ignore = gr::testing::applyStagedForTest(coupledSettings);
        expect(eq(coupled.settings.level.value, foreignLevel)) << "a setting is not coupled to a key in another domain";
    };

    "one key sets every setting coupled to it, and a setting follows one key"_test = [] {
        using namespace qa_follows_tag;
        FollowingBlock    block;
        gr::SettingsBase& settings = gr::settings::detail::settingsOf(block);
        std::ignore                = settings.check({});
        std::ignore                = settings.setTagCouplings({{"level", std::string(kLevelKey)}, {"gain", std::string(kLevelKey)}});
        expect(eq(settings.tagCouplings().size(), 2UZ));

        const float shared = block.settings.gain.value + block.settings.level.value + 1.f;
        qa_settings::enterTag(settings, tagOf(kLevelKey, shared));
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.level.value, shared) && eq(block.settings.gain.value, shared)) << "the key does not set both settings coupled to it";

        constexpr std::string_view kGainKey = "foo:gain";
        std::ignore                         = settings.setTagCouplings({{"gain", std::string(kGainKey)}});
        expect(eq(keyOf(settings, "gain"), std::string(kGainKey))) << "a second coupling does not replace the first";
        const float levelOnly = shared + 1.f;
        const float gainOnly  = shared + 2.f;
        qa_settings::enterTag(settings, tagOf(kLevelKey, levelOnly));
        qa_settings::enterTag(settings, tagOf(kGainKey, gainOnly));
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.level.value, levelOnly)) << "the setting left on the key stops following it";
        expect(eq(block.settings.gain.value, gainOnly)) << "the setting moved to another key follows the key it left";

        std::ignore = settings.setTagCouplings({{"gain", std::string()}, {"level", std::string(":") + std::string(gr::tag::nameOf(kLevelKey))}});
        expect(eq(keyOf(settings, "gain"), std::string())) << "an empty key leaves the coupling in place";
        expect(eq(keyOf(settings, "level"), std::string(gr::tag::nameOf(kLevelKey)))) << "a key with a leading colon is held in another spelling than the bare name";
        qa_settings::enterTag(settings, tagOf(kGainKey, gainOnly + 1.f));
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.gain.value, gainOnly)) << "a setting whose coupling is removed follows its old key";
    };

    "a coupling to a key the framework acts on, to a non-writable setting or to a malformed key is refused"_test = [] {
        using namespace qa_follows_tag;
        FollowingBlock    block;
        gr::SettingsBase& settings = gr::settings::detail::settingsOf(block);
        std::ignore                = settings.check({});
        coupleRateAndLevel(settings);
        const property_map before = settings.tagCouplings();

        for (const std::string_view controlKey : gr::tag::kControlKeys) {
            const std::string refusal = couplingRefusal(settings, {{"gain", std::string(controlKey)}});
            expect(refusal.contains("gain") && refusal.contains(controlKey) && refusal.contains("reserved by the framework")) << "a coupling to " << controlKey << " is not refused by name: " << refusal;
        }
        for (const std::string_view setting : {std::string_view("unique_name"), std::string_view("no_such_setting")}) {
            const std::string refusal = couplingRefusal(settings, {{std::pmr::string(setting), std::string("foo:x")}});
            expect(refusal.contains(setting) && refusal.contains("not writable")) << "a coupling of " << setting << " is not refused by name: " << refusal;
        }
        for (const std::string_view malformed : {std::string_view(":"), std::string_view("gr:"), std::string_view("::x"), std::string_view("foo:a b"), std::string_view("foo:\tx")}) {
            const std::string refusal = couplingRefusal(settings, {{"gain", std::string(malformed)}});
            expect(refusal.contains("gain") && refusal.contains("not domain:name")) << "the malformed key '" << malformed << "' is not refused: " << refusal;
        }
        const std::string notText = couplingRefusal(settings, {{"gain", 42}});
        expect(notText.contains("gain") && notText.contains("tag key not a text")) << "a key that is not a string is not refused: " << notText;

        const std::string mixed = couplingRefusal(settings, {{"gain", std::string("foo:gain")}, {"level", std::string(gr::tag::kControlKeys.front())}});
        expect(!mixed.empty()) << "a batch with one refused entry is taken";
        expect(settings.tagCouplings() == before) << "a refused call changes the table";
    };

    "a setting coupled before init() follows its key, whatever value the constructor's map gives it"_test = [] {
        using namespace qa_follows_tag;
        const float       constructedRate = 2.f * FollowingBlock{}.settings.sample_rate.value;
        FollowingBlock    block{property_map{{"sample_rate", constructedRate}}};
        gr::SettingsBase& settings = gr::settings::detail::settingsOf(block);
        coupleRateAndLevel(settings);
        std::ignore = settings.check({});
        expect(eq(block.settings.sample_rate.value, constructedRate)) << "the constructor's value does not reach the member";

        const float taggedRate = 2.f * constructedRate;
        qa_settings::enterTag(settings, tagOf(gr::tag::SAMPLE_RATE.key(), taggedRate));
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.sample_rate.value, taggedRate)) << "the tag does not set a setting coupled before init()";
    };

    "the framework writes no tag for an applied setting"_test = [] {
        using namespace qa_follows_tag;
        FollowingBlock    block;
        gr::SettingsBase& settings = gr::settings::detail::settingsOf(block);
        std::ignore                = settings.check({});
        coupleRateAndLevel(settings);
        const float setRate = 2.f * block.settings.sample_rate.value;
        std::ignore         = settings.set({{"sample_rate", setRate}, {"level", block.settings.level.value + 1.f}, {"gain", 2.f * block.settings.gain.value}});
        std::ignore         = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.sample_rate.value, setRate)) << "the caller's value is not applied";
        expect(gr::settings::detail::FrameworkAccess::takeForwarded(settings).empty()) << "a value a caller sets leaves as a tag";

        FollowingBlock    tagged;
        gr::SettingsBase& taggedSettings = gr::settings::detail::settingsOf(tagged);
        std::ignore                      = taggedSettings.check({});
        coupleRateAndLevel(taggedSettings);
        const float taggedRate = 3.f * tagged.settings.sample_rate.value;
        qa_settings::enterTag(taggedSettings, tagOf(gr::tag::SAMPLE_RATE.key(), taggedRate));
        std::ignore = gr::testing::applyStagedForTest(taggedSettings);
        expect(eq(tagged.settings.sample_rate.value, taggedRate));
        expect(gr::settings::detail::FrameworkAccess::takeForwarded(taggedSettings).empty()) << "a tag's value leaves as a second tag";
    };

    "a list of strings coupled to a tag key takes the tag's list at the tag's sample"_test = [] {
        using namespace qa_follows_tag;
        constexpr std::string_view     kNamesKey = "foo:names";
        const std::vector<std::string> tagged{"first", "second"};
        Tensor<std::pmr::string>       textTensor(extents_from, {tagged.size()});
        std::ranges::copy(tagged, textTensor.begin());
        constexpr std::size_t kTagIndex = 5UZ;
        constexpr std::size_t kSamples  = 2UZ * kTagIndex;

        const std::vector<std::pair<std::string_view, pmt::Value>> carriers{{"Tensor<pmt::Value>", pmt::Value(tagged)}, {"Tensor<std::pmr::string>", pmt::Value(textTensor)}};

        const std::vector<std::string> untagged = NamesRecordingSink{}.settings.names;
        for (const auto& [carrier, list] : carriers) {
            const auto namesAtSample = namesFollowingTag(kNamesKey, property_map{{std::pmr::string(kNamesKey), list}}, kTagIndex, kSamples);
            expect(fatal(eq(namesAtSample.size(), kSamples))) << carrier;
            for (std::size_t i = 0UZ; i < kSamples; ++i) {
                expect(namesAtSample[i] == (i < kTagIndex ? untagged : tagged)) << std::format("the list a {} carries: '{}' at sample {}, the tag at sample {}", carrier, joined(namesAtSample[i]), i, kTagIndex);
            }
        }

        const std::vector<pmt::Value> mixed{pmt::Value(std::string("first")), pmt::Value(1.f)};
        const auto                    namesOfMixed = namesFollowingTag(kNamesKey, property_map{{std::pmr::string(kNamesKey), pmt::Value(Tensor<pmt::Value>(mixed.begin(), mixed.end()))}}, kTagIndex, kSamples);
        expect(std::ranges::all_of(namesOfMixed, [&untagged](const std::vector<std::string>& names) { return names == untagged; })) << "a list with an element that is no string sets the setting";
    };
};

namespace qa_reset_callbacks {
using namespace gr;

/// a gain whose applySettings() and reset() read the block's own settings
struct SettingsReadingReset : Block<SettingsReadingReset> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float> gain = 1.f;

        GR_MAKE_REFLECTABLE(Settings, gain);
    } settings{};

    GR_MAKE_REFLECTABLE(SettingsReadingReset, in, out, settings);

    std::optional<float> gainReadByLastChange; // the gain that get() returned in the last applySettings()
    std::optional<float> gainReadByLastReset;  // the gain that get() returned in the last reset()
    std::size_t          resets = 0;

    void reset() {
        ++resets;
        gainReadByLastReset = readGain();
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain; }

    void applySettings([[maybe_unused]] const Settings& s) { gainReadByLastChange = readGain(); }

    [[nodiscard]] std::optional<float> readGain() {
        const std::expected<float, gr::settings::Rejected> value = this->get<float>("gain");
        return value.has_value() ? std::optional<float>(*value) : std::nullopt;
    }
};

/// the time a reset gets to return. A reset that returns takes microseconds.
constexpr std::chrono::seconds kResetBound{5};

/// runs `resetBlock` on its own thread and returns whether it returned within kResetBound. A reset that does not
/// return keeps its thread and its block, and both are leaked.
[[nodiscard]] bool resetReturns(std::unique_ptr<SettingsReadingReset>& block, std::function<void(SettingsReadingReset&)> resetBlock) {
    std::packaged_task<void()> task([&target = *block, resetBlock = std::move(resetBlock)] { resetBlock(target); });
    std::future<void>          done = task.get_future();
    std::thread                runner(std::move(task));
    if (done.wait_for(kResetBound) != std::future_status::ready) {
        runner.detach();
        std::ignore = block.release();
        return false;
    }
    runner.join();
    done.get();
    return true;
}

} // namespace qa_reset_callbacks

const boost::ut::suite<"a reset whose callbacks read the settings"> resetCallbackTests = [] {
    using namespace boost::ut;
    using namespace qa_reset_callbacks;

    "a reset by tag or by call returns when applySettings() and reset() read the settings"_test = [] {
        for (const bool byTag : {true, false}) {
            const std::string_view path  = byTag ? "a reset_default tag" : "a resetDefaults() call";
            auto                   block = std::make_unique<SettingsReadingReset>();
            block->init(std::make_shared<gr::Sequence>());
            const float defaultGain = block->settings.gain.value;
            std::ignore             = block->set({{"gain", defaultGain / 2.f}});
            std::ignore             = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(*block));
            expect(eq(block->settings.gain.value, defaultGain / 2.f)) << "the value to reset does not reach the member";
            block->gainReadByLastChange.reset();
            const std::size_t resetsBefore = block->resets;

            const bool returned = resetReturns(block, [byTag](SettingsReadingReset& target) {
                if (byTag) {
                    qa_settings::enterTag(gr::settings::detail::settingsOf(target), qa_refusals::flagTag(gr::tag::RESET_DEFAULTS.key()));
                    std::ignore = gr::testing::applyStagedForTest(gr::settings::detail::settingsOf(target));
                } else {
                    std::ignore = target.resetDefaults();
                }
            });
            expect(returned) << path << " does not return within " << kResetBound.count() << " s";
            if (!returned) {
                continue;
            }
            expect(eq(block->settings.gain.value, defaultGain)) << path << " leaves the member off its default";
            expect(eq(block->resets, resetsBefore + 1UZ)) << path << " calls reset() a number of times other than once";
            expect(block->gainReadByLastChange == std::optional<float>(defaultGain)) << "applySettings() reads a gain other than the default after " << path;
            expect(block->gainReadByLastReset == std::optional<float>(defaultGain)) << "reset() reads a gain other than the default after " << path;
        }
    };
};

namespace qa_callback_staging {
using namespace gr;

/// a gain and offset stage whose reset() or applySettings() sets an offset on its own settings once, when armed
struct StagingCallbacks : Block<StagingCallbacks> {
    PortIn<float>  in{};
    PortOut<float> out{};

    struct Settings {
        Annotated<float> gain   = 1.f;
        Annotated<float> offset = 0.f;

        GR_MAKE_REFLECTABLE(Settings, gain, offset);
    } settings{};

    GR_MAKE_REFLECTABLE(StagingCallbacks, in, out, settings);

    std::optional<float>                                                       offsetFromReset;  // the offset the next reset() sets
    std::optional<float>                                                       offsetFromChange; // the offset the next applySettings() sets
    std::optional<std::expected<gr::settings::Values, gr::settings::Rejected>> ownCall{};

    void reset() { stageOffsetOnce(offsetFromReset); }

    void applySettings([[maybe_unused]] const Settings& s) { stageOffsetOnce(offsetFromChange); }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * settings.gain.value + settings.offset.value; }

    void stageOffsetOnce(std::optional<float>& armed) {
        if (const std::optional<float> value = std::exchange(armed, std::nullopt); value.has_value()) {
            ownCall = this->set({{"offset", *value}}, gr::settings::noWait);
        }
    }
};

enum class StagingPath { resetTag, changeTag, resetCall };

[[nodiscard]] constexpr std::string_view describe(StagingPath path) noexcept {
    switch (path) {
    case StagingPath::resetTag: return "a reset_default tag whose reset() sets";
    case StagingPath::changeTag: return "a coupled tag whose applySettings() sets";
    case StagingPath::resetCall: return "a resetDefaults() call whose reset() sets";
    }
    return "";
}

} // namespace qa_callback_staging

const boost::ut::suite<"a callback sets its own block during an apply"> callbackStagingTests = [] {
    using namespace boost::ut;
    using namespace qa_callback_staging;

    "a callback's set on its own block during an apply is refused, and nothing waits for the next apply"_test = [] {
        for (const StagingPath path : {StagingPath::resetTag, StagingPath::changeTag, StagingPath::resetCall}) {
            StagingCallbacks block;
            block.init(std::make_shared<gr::Sequence>());
            qa_coupling::couple(gr::settings::detail::settingsOf(block), "gain", "gain");
            const float defaultGain  = block.settings.gain.value;
            const float stagedOffset = block.settings.offset.value + 2.f;
            const float offsetBefore = block.settings.offset.value;
            if (path == StagingPath::resetTag || path == StagingPath::resetCall) {
                block.offsetFromReset = stagedOffset;
            } else {
                block.offsetFromChange = stagedOffset;
            }

            switch (path) {
            case StagingPath::resetTag:
                qa_settings::enterTag(gr::settings::detail::settingsOf(block), qa_refusals::flagTag(gr::tag::RESET_DEFAULTS.key()));
                block.applyChangedSettings();
                break;
            case StagingPath::changeTag:
                qa_settings::enterTag(gr::settings::detail::settingsOf(block), Tag{0UZ, property_map{{"gain", 2.f * defaultGain}}});
                block.applyChangedSettings();
                break;
            case StagingPath::resetCall: std::ignore = block.resetDefaults(); break;
            }

            expect(!block.offsetFromReset.has_value() && !block.offsetFromChange.has_value()) << describe(path) << " runs no callback";
            expect(fatal(block.ownCall.has_value())) << describe(path);
            expect(!block.ownCall->has_value() && block.ownCall->error().front().reason.contains("own thread")) << describe(path) << " takes the block's own call";
            expect(!gr::settings::detail::FrameworkAccess::hasWork(gr::settings::detail::settingsOf(block))) << describe(path) << " leaves a change pending";
            block.applyChangedSettings();
            expect(eq(block.settings.offset.value, offsetBefore)) << "the apply after " << describe(path) << " applies the refused offset";
            expect(block.settings.offset.value != stagedOffset);
        }
    };
};

const boost::ut::suite<"a block's work call and its settings"> workCallTests = [] {
    using namespace boost::ut;
    using namespace qa_follows_tag;

    "a work call that reads no input applies the change waiting for it first"_test = [] {
        qa_settings::ChangeRecordingBlock block;
        gr::PortOut<float>                upstream;
        gr::PortIn<float>                 downstream;
        expect(fatal(upstream.connect(block.in).has_value()));
        expect(fatal(block.out.connect(downstream).has_value()));
        block.init(std::make_shared<gr::Sequence>());
        std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);

        const gr::Size_t size = 2U * block.settings.fft_size.value;
        expect(block.set({{"fft_size", size}}, gr::settings::noWait).has_value());
        expect(neq(block.settings.fft_size.value, size)) << "a running block applies the change at the call";
        std::ignore = block.work();
        expect(eq(block.settings.fft_size.value, size)) << "a starved work call leaves the change waiting";

        // a set that waits returns once the next work call applied its change; the change is pending before that call
        const gr::Size_t                                            waitedSize = 2U * size;
        std::expected<gr::settings::Values, gr::settings::Rejected> waited;
        std::thread                                                 caller([&block, &waited, waitedSize] { waited = block.set({{"fft_size", waitedSize}}); });
        while (!gr::settings::detail::FrameworkAccess::hasWork(gr::settings::detail::settingsOf(block))) {
            std::this_thread::yield();
        }
        std::ignore = block.work();
        caller.join();
        expect(waited.has_value() && waited->empty()) << "a set waiting on a starved block does not return applied after one work call";
        expect(eq(block.settings.fft_size.value, waitedSize));
    };

    "a value a caller sets holds until the next tag, and the last of several tags is the one in force"_test = [] {
        NotificationRecorder scheduler;
        FollowingBlock       block;
        gr::SettingsBase&    settings = gr::settings::detail::settingsOf(block);
        std::ignore                   = settings.check({});
        coupleRateAndLevel(settings);
        gr::settings::detail::FrameworkAccess::attachScheduler(settings, &scheduler);
        std::ignore = block.changeStateTo(gr::lifecycle::State::RUNNING);

        expect(settings.set({{"sample_rate", 500.f}}, gr::settings::noWait).has_value());
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.sample_rate.value, 500.f)) << "the caller's value is not applied";
        expect(gr::settings::detail::FrameworkAccess::isGiven(settings, "sample_rate")) << "the caller's value is not given";

        for (const float taggedRate : {1000.f, 2000.f, 3000.f}) {
            qa_settings::enterTag(settings, tagOf(gr::tag::SAMPLE_RATE.key(), taggedRate));
        }
        std::ignore = gr::testing::applyStagedForTest(settings);
        expect(eq(block.settings.sample_rate.value, 3000.f)) << "the caller's value outlives the tags after it";
        expect(settings.get<float>("sample_rate") == 3000.f) << "get() reads a rate other than the last tag's";
        const bool tagNamed = std::ranges::any_of(scheduler.held, [](const gr::settings::Notification& n) { return n.key == "sample_rate" && n.what == gr::settings::NotificationKind::changed && n.reason == "a tag set the value"; });
        expect(tagNamed) << "no reader learns that a tag set the rate";
        gr::settings::detail::FrameworkAccess::attachScheduler(settings, nullptr);
    };

    "a block given a key it does not declare is refused at the add, and alone at its first call"_test = [] {
        gr::Graph   flow;
        std::string refusal;
        try {
            std::ignore = flow.emplaceBlock<FollowingBlock>({{"name", std::string("sink")}, {"aa", 1.f}});
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("block 'sink'") && refusal.contains("aa")) << refusal;
        expect(flow.blocks().empty()) << "the refused block is in the graph";

        FollowingBlock alone{property_map{{"name", std::string("sink")}, {"aa", 1.f}}};
        const auto     first = alone.set({{"gain", 2.f}});
        expect(!first.has_value() && gr::settings::describeRejected(first.error()).contains("aa")) << "the first call does not return the refusal of the constructed key";
    };
};

namespace qa_changed {

// names, at each apply, the settings whose `changed()` reads true
struct ChangeWatcher : gr::Block<ChangeWatcher> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float>              cutoff = 0.25f;
        gr::Annotated<std::int32_t>       taps   = 31;
        gr::Annotated<std::vector<float>> window = std::vector<float>{1.f};

        GR_MAKE_REFLECTABLE(Settings, cutoff, taps, window);
    } settings;

    GR_MAKE_REFLECTABLE(ChangeWatcher, in, out, settings);

    std::vector<std::vector<std::string>> seen{};
    std::optional<Settings>               copied{};
    bool                                  throwInHook = false;

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return x; }

    void applySettings(const Settings& s) {
        std::vector<std::string> names;
        if (s.cutoff.changed()) {
            names.emplace_back("cutoff");
        }
        if (s.taps.changed()) {
            names.emplace_back("taps");
        }
        if (s.window.changed()) {
            names.emplace_back("window");
        }
        seen.push_back(std::move(names));
        copied.emplace(s); // a copy constructed from `s`; an assignment to an earlier copy would keep that copy's flags
        if (throwInHook) {
            throw gr::exception("the device refuses the change");
        }
    }
};

// a hook that applies another block's settings on its own thread reads its own flags afterwards
struct NestingWatcher : gr::Block<NestingWatcher> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float>        gain = 1.f;
        gr::Annotated<std::int32_t> taps = 31;

        GR_MAKE_REFLECTABLE(Settings, gain, taps);
    } settings;

    GR_MAKE_REFLECTABLE(NestingWatcher, in, out, settings);

    ChangeWatcher*    other = nullptr;
    std::vector<bool> gainChanged{};
    std::vector<bool> tapsChanged{};

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return x * settings.gain; }

    void applySettings(const Settings& s) {
        if (other != nullptr) {
            std::ignore = other->set({{"cutoff", 0.5f}});
        }
        gainChanged.push_back(s.gain.changed());
        tapsChanged.push_back(s.taps.changed());
    }
};

// holds its settings in a member named `config`: core finds the member by its type
struct ConfigHolder : gr::Block<ConfigHolder> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    struct Settings {
        gr::Annotated<float>        gain = 1.f;
        gr::Annotated<std::int32_t> taps = 31;

        GR_MAKE_REFLECTABLE(Settings, gain, taps);
    } config;

    GR_MAKE_REFLECTABLE(ConfigHolder, in, out, config);

    bool              hookGotOwnMember = false;
    std::vector<bool> tapsChangedAfterAssign{};

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return x * config.gain; }

    std::optional<gr::Refused> checkSettings(const Settings& s) const { return s.gain > 10.f ? std::optional(gr::Refused(s.gain, "above 10")) : std::nullopt; }

    // assigns `taps` from the gain to pin that the block's own write leaves the setting's flag
    void applySettings(Settings& s) {
        hookGotOwnMember = std::addressof(s) == std::addressof(config);
        if (s.gain.changed()) {
            s.taps = static_cast<std::int32_t>(s.gain.value) * 10;
        }
        tapsChangedAfterAssign.push_back(s.taps.changed());
    }
};

} // namespace qa_changed

const boost::ut::suite<"a block asks what changed"> changedTests = [] {
    using namespace boost::ut;
    using namespace qa_changed;
    using Names = std::vector<std::string>;

    "changed() reads true for every setting at the setup, and afterwards for each setting whose value the batch changed"_test = [] {
        ChangeWatcher block;
        expect(fatal(block.check({}).has_value()));
        expect(fatal(eq(block.seen.size(), 1UZ)));
        expect(block.seen.back() == Names{"cutoff", "taps", "window"}) << "every setting, though each holds its default";

        expect(block.set({{"cutoff", 0.1f}}).has_value());
        expect(block.seen.back() == Names{"cutoff"});
        expect(block.set({{"cutoff", 0.1f}, {"taps", 15}}).has_value());
        expect(block.seen.back() == Names{"taps"}) << "a setting sent at its value is no change";
        expect(block.set({{"window", std::vector<float>{1.f, 0.5f}}}).has_value());
        expect(block.seen.back() == Names{"window"});
    };

    "changed() reads false outside the hook, after a hook that returns and after one that throws"_test = [] {
        ChangeWatcher block;
        expect(fatal(block.check({}).has_value()));
        expect(!block.settings.cutoff.changed() && !block.settings.taps.changed() && !block.settings.window.changed()) << "after the setup";
        expect(block.set({{"taps", 7}}).has_value());
        expect(!block.settings.taps.changed()) << "after a hook that returns";

        block.throwInHook = true;
        std::ignore       = block.set({{"cutoff", 0.4f}});
        expect(block.seen.back() == Names{"cutoff"}) << "the hook ran";
        expect(eq(block.settings.cutoff.value, 0.25f)) << "the key the throw failed takes back its value";
        expect(!block.settings.cutoff.changed()) << "after a hook that throws";
    };

    "a hook that applies another block's settings reads its own flags after that apply"_test = [] {
        ChangeWatcher other;
        expect(fatal(other.check({}).has_value()));
        NestingWatcher block;
        expect(fatal(block.check({}).has_value()));
        block.other = &other;
        expect(block.set({{"gain", 2.f}}).has_value());
        expect(other.seen.back() == Names{"cutoff"}) << "the other block's hook ran inside this one's";
        expect(block.gainChanged == std::vector<bool>{true, true}) << "the batch changed gain";
        expect(block.tapsChanged == std::vector<bool>{true, false}) << "the batch left taps";
    };

    "a block names its Settings member as it likes, and its hooks take that member"_test = [] {
        ConfigHolder block;
        expect(fatal(block.check({}).has_value()));
        expect(block.hookGotOwnMember) << "the hook's argument is the block's own member";
        expect(block.set({{"gain", 2.f}}).has_value());
        expect(eq(block.config.gain.value, 2.f));
        expect(eq(block.config.taps.value, 20)) << "the hook's write reached the member";
        expect(block.tapsChangedAfterAssign == std::vector<bool>{true, false}) << "the block's own write leaves the flag";
        expect(!block.set({{"gain", 20.f}}).has_value()) << "the check reads the struct core fills";
        expect(eq(block.get<float>("gain").value_or(0.f), 2.f));
    };
};
