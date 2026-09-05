#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

/**
 * @brief Where a resampling block's sample_rate lives.
 *
 * A block's own settings hold the rate it is fed at. The chunk ratio belongs to the value it publishes — the
 * forwarded parameters and the tags it substitutes its own value into. Scaling the stored
 * value instead makes every further application scale again, so the rate drops further with each of the save/load or
 * re-apply cycles that a running graph performs routinely. The forwarded map therefore waits for an output span
 * rather than going back through the block's own settings, which a work call that moves nothing would otherwise do
 * once per call.
 *
 * The blocks are defined here: gnuradio4-core carries no standard block library, so a core test may not depend on one.
 */

namespace qa_settings {

using namespace gr;

inline constexpr gr::Size_t  kDecimation = 4U;
inline constexpr float       kInputRate  = 1000.0f;
inline constexpr float       kOutputRate = kInputRate / static_cast<float>(kDecimation);
inline constexpr std::size_t kSamples    = 256UZ;

struct Decimator : Block<Decimator, Resampling<>> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "sample rate"> sample_rate = kInputRate;

    GR_MAKE_REFLECTABLE(Decimator, in, out, sample_rate);

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t ratio = static_cast<std::size_t>(input_chunk_size.value);
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * ratio];
        }
        return work::Status::OK;
    }
};

/// the same decimator without a rate of its own: the chunk ratio is all it knows about the rate it publishes at
struct RatelessDecimator : Block<RatelessDecimator, Resampling<>> {
    PortIn<float>  in;
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(RatelessDecimator, in, out);

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
            outSpan.publishTag(property_map{{"sample_rate", kInputRate}}, 0UZ);
        }
        _emitted += n;
        outSpan.publish(n);
        return work::Status::OK;
    }
};

inline constexpr std::size_t kStarvedCalls = 100UZ;
inline constexpr std::size_t kDecayCalls   = 200UZ;
inline constexpr std::size_t kCallsToZero  = 81UZ; ///< kInputRate scaled by the 4:1 chunk ratio once per call, in float

/// the same decimator, recording every rate it is asked to run at rather than only the last; the
/// default differs from the rate it is built with, so that first application is a genuine change
struct WatchingDecimator : Block<WatchingDecimator, Resampling<>> {
    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "sample rate"> sample_rate = 1.0f;

    GR_MAKE_REFLECTABLE(WatchingDecimator, in, out, sample_rate);

    std::vector<float> appliedRates;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        const std::size_t ratio = static_cast<std::size_t>(input_chunk_size.value);
        for (std::size_t i = 0UZ; i < outSpan.size(); ++i) {
            outSpan[i] = inSpan[i * ratio];
        }
        return work::Status::OK;
    }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings) {
        if (newSettings.contains(gr::tag::SAMPLE_RATE.shortKey())) {
            appliedRates.push_back(sample_rate.value);
        }
    }
};

/// publishes nothing for its first kStarvedCalls calls, so the chain behind it is driven with calls
/// that can move no sample — what a source whose reader thread has not yet delivered leaves behind it
struct LateSource : Block<LateSource> {
    PortOut<float> out;

    GR_MAKE_REFLECTABLE(LateSource, out);

    std::size_t idleCalls = 0UZ;

    std::size_t _emitted = 0UZ;

    work::Status processBulk(OutputSpanLike auto& outSpan) {
        if (idleCalls < kStarvedCalls) {
            ++idleCalls;
            outSpan.publish(0UZ);
            return work::Status::INSUFFICIENT_INPUT_ITEMS;
        }
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
            outSpan.publishTag(property_map{{"sample_rate", kInputRate}}, 0UZ);
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
        auto it = map.find(gr::tag::SAMPLE_RATE.shortKey());
        if (it == map.end()) {
            return std::nullopt;
        }
        const float* value = it->second.get_if<float>();
        return value != nullptr ? std::optional<float>(*value) : std::nullopt;
    }
};

[[nodiscard]] float rateOf(const property_map& map) { return RateTagSink::rateOf(map).value_or(0.0f); }

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
                outSpan.publishTag(property_map{{"sample_rate", index + kTagStride >= nSamples ? finalRate : kInputRate}}, i);
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

    Annotated<float, "sample rate"> sample_rate = 1.0f;

    GR_MAKE_REFLECTABLE(RateCountingBlock, in, out, sample_rate);

    std::size_t nSettingsChanged = 0UZ;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { nSettingsChanged++; }
};

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
    auto& sink       = flow.emplaceBlock<RateTagSink>();
    expect(flow.connect<"out", "in">(source, middle).has_value());
    expect(flow.connect<"out", "in">(middle, sink).has_value());

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
    expect(scheduler.exchange(std::move(flow)).has_value());
    expect(scheduler.runAndWait().has_value());

    return {middle.nSettingsChanged, middle.sample_rate.value};
}

[[nodiscard]] Decimator makeDecimator();

const boost::ut::suite<"staged type refusals"> stagedRefusalTests = [] {
    using namespace boost::ut;

    // setStaged, set and the tag auto-update leaves each type-check before staging, so a
    // wrong-typed value reaches the apply path only through the unchecked entrances (raw
    // context activation of deserialized state). The extraction itself must therefore
    // report the mismatch rather than terminate the process.
    "a wrong-typed staged value yields an error, not termination"_test = [] {
        const pmt::Value wrong{std::string("not a number")};
        expect(!gr::settings::extractStagedValue<gr::Size_t>(wrong, "input_chunk_size").has_value()) << "scalar mismatch reports";
        expect(!gr::settings::extractStagedValue<std::vector<float>>(wrong, "taps").has_value()) << "tensor mismatch reports";
    };
};

[[nodiscard]] Decimator makeDecimator() {
    Decimator block;
    block.init(std::make_shared<gr::Sequence>());
    std::ignore = block.settings().set({{"input_chunk_size", kDecimation}, {"output_chunk_size", gr::Size_t(1)}});
    std::ignore = block.settings().activateContext();
    std::ignore = block.settings().applyStagedParameters();
    return block;
}

} // namespace qa_settings

const boost::ut::suite<"settings"> _settings = [] {
    using namespace boost::ut;
    using namespace qa_settings;

    "a decimator keeps its input rate and forwards the output rate on every application"_test = [] {
        Decimator block = makeDecimator();

        const property_map rateUpdate{{"sample_rate", kInputRate}};
        for (std::size_t nApplication = 1UZ; nApplication <= 3UZ; ++nApplication) {
            std::ignore       = block.settings().setStaged(rateUpdate);
            const auto result = block.settings().applyStagedParameters();

            expect(eq(rateOf(result.forwardParameters), kOutputRate)) << std::format("application {} forwards the output rate", nApplication);
            expect(eq(rateOf(block.settings().get()), kInputRate)) << std::format("application {} leaves the stored rate at the input rate", nApplication);
            expect(eq(block.sample_rate.value, kInputRate)) << std::format("application {} leaves the member at the input rate", nApplication);
        }
    };

    "a decimator's rate survives repeated save and load cycles"_test = [] {
        Decimator block = makeDecimator();

        std::ignore = block.settings().setStaged({{"sample_rate", kInputRate}});
        std::ignore = block.settings().applyStagedParameters();

        for (std::size_t nCycle = 1UZ; nCycle <= 3UZ; ++nCycle) {
            const property_map saved = block.settings().get();
            std::ignore              = block.settings().setStaged(saved);
            const auto result        = block.settings().applyStagedParameters();

            expect(eq(rateOf(saved), kInputRate)) << std::format("cycle {} saves the input rate", nCycle);
            expect(eq(rateOf(result.forwardParameters), kOutputRate)) << std::format("cycle {} forwards the output rate", nCycle);
        }
    };

    "a rate tag through a decimator arrives scaled"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RateTagSource>(property_map{{"name", std::string("src")}});
        auto&     middle = flow.emplaceBlock<Decimator>(property_map{{"name", std::string("mid")}, {"input_chunk_size", kDecimation}, {"output_chunk_size", gr::Size_t(1)}});
        auto&     sink   = flow.emplaceBlock<RateTagSink>(property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(ge(sink.rates.size(), 1UZ)) << "the sink must see the rate the decimator publishes at";
        expect(std::ranges::all_of(sink.rates, [](float rate) { return rate == kOutputRate; })) << "every forwarded rate must be the output rate";
        expect(eq(middle.sample_rate.value, kInputRate)) << "the tag leaves the block's own setting at the input rate";
    };

    "a rate tag through a decimator that declares no sample_rate arrives scaled"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<RateTagSource>(property_map{{"name", std::string("src")}});
        auto&     middle = flow.emplaceBlock<RatelessDecimator>(property_map{{"name", std::string("mid")}, {"input_chunk_size", kDecimation}, {"output_chunk_size", gr::Size_t(1)}});
        auto&     sink   = flow.emplaceBlock<RateTagSink>(property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(ge(sink.rates.size(), 1UZ)) << "the sink must see the rate the decimator publishes at";
        expect(std::ranges::all_of(sink.rates, [](float rate) { return rate == kOutputRate; })) << "the chunk ratio must scale the forwarded rate whether or not the block owns one";
    };

    "a decimator starved by its source keeps the rate it is fed at"_test = [] {
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<LateSource>(property_map{{"name", std::string("src")}});
        auto&     middle = flow.emplaceBlock<WatchingDecimator>(property_map{{"name", std::string("mid")}, {"sample_rate", kInputRate}, {"input_chunk_size", kDecimation}, {"output_chunk_size", gr::Size_t(1)}});
        auto&     sink   = flow.emplaceBlock<RateTagSink>(property_map{{"name", std::string("snk")}});
        expect(flow.connect<"out", "in">(source, middle).has_value());
        expect(flow.connect<"out", "in">(middle, sink).has_value());

        gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler{};
        expect(scheduler.exchange(std::move(flow)).has_value());
        expect(scheduler.runAndWait().has_value());

        expect(eq(source.idleCalls, kStarvedCalls)) << "the source must have driven the chain while it could publish nothing";
        expect(eq(middle.sample_rate.value, kInputRate)) << "a call that moves nothing must leave the block's own rate alone";
        expect(eq(rateOf(middle.settings().get()), kInputRate)) << "and must leave the stored rate alone with it";
        expect(eq(middle.appliedRates.size(), 1UZ)) << "the rate it is built at applies once, not once per call that moved nothing";
        expect(std::ranges::all_of(middle.appliedRates, [](float rate) { return rate == kInputRate; })) << "the block must never be run at the rate it publishes at";
        expect(ge(sink.rates.size(), 1UZ)) << "the forwarded rate must still reach the sink once there is a span to publish into";
        expect(std::ranges::all_of(sink.rates, [](float rate) { return rate == kOutputRate; })) << "and that tag carries the output rate";
    };

    // the instrument, against what it is for: staging the forwarded map back into the block runs it
    // at the rate it publishes at, and every further application scales again from there
    "staging the forwarded rate back into a decimator decays it to zero"_test = [] {
        Decimator block = makeDecimator();
        std::ignore     = block.settings().setStaged({{"sample_rate", kInputRate}});

        std::size_t nCallsToZero = 0UZ;
        for (std::size_t nCall = 1UZ; nCall <= kDecayCalls && nCallsToZero == 0UZ; ++nCall) {
            const auto result = block.settings().applyStagedParameters();
            if (nCall == 1UZ) {
                expect(eq(block.sample_rate.value, kInputRate)) << "the first application is the honest one";
                expect(eq(rateOf(result.forwardParameters), kOutputRate)) << "and it forwards the output rate";
            }
            if (block.sample_rate.value == 0.0f) {
                nCallsToZero = nCall;
            }
            std::ignore = block.settings().setStaged(result.forwardParameters);
        }

        expect(eq(nCallsToZero, kCallsToZero)) << "the chunk ratio applied once per call reaches zero in float";
    };

    "an unchanged setting tag costs one apply however often it repeats"_test = [] {
        const RateStreamResult shortRun = runRateStream(kShortStream, kInputRate);
        const RateStreamResult longRun  = runRateStream(kLongStream, kInputRate);

        expect(le(shortRun.nSettingsChanged, 1UZ)) << "an unchanged value must apply at most once";
        expect(eq(shortRun.nSettingsChanged, longRun.nSettingsChanged)) << "the apply count must not scale with the number of identical tags";
        expect(eq(shortRun.finalRate, kInputRate));
        expect(eq(longRun.finalRate, kInputRate));
    };

    "a new value in the same stream still applies"_test = [] {
        const RateStreamResult unchanged = runRateStream(kLongStream, kInputRate);
        const RateStreamResult retuned   = runRateStream(kLongStream, kRetunedRate);

        expect(eq(retuned.nSettingsChanged, unchanged.nSettingsChanged + 1UZ)) << "the differing tag must cost exactly one further apply";
        expect(eq(retuned.finalRate, kRetunedRate)) << "the differing tag must reach the block's setting";
    };
};

int main() { /* not needed by the UT framework */ }
