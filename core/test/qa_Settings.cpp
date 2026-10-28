#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <future>
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
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

/**
 * @brief Where a resampling block's sample_rate lives.
 *
 * A block's own settings hold the rate it is fed at. The chunk ratio belongs to the value it publishes — the
 * forwarded parameters and the tags it substitutes its own value into. Scaling the stored
 * value instead makes every further application scale again, so the rate drops further with each of the save/load or
 * re-apply cycles that a running graph performs routinely. The forwarded map therefore waits for an output span to
 * publish it into.
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

/// a decimator recording every rate applied to it; its default differs from the rate it is
/// constructed with, so the first application is a genuine change
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

/// a source publishing nothing for its first kStarvedCalls calls, driving the chain behind it with
/// calls that can move no sample
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

/// keeps both maps of every settings change, so what the callback was told can be read off the block. Every member
/// carries an initializer of its own: the block is an aggregate constructed from its base alone, and a member with
/// no initializer is one clang reports as a missing field initializer at each such construction.
struct ChangeRecordingBlock : Block<ChangeRecordingBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "sample rate">         sample_rate = 1.0f;
    Annotated<gr::Size_t, "FFT size">       fft_size    = 1024U;
    Annotated<gr::Size_t, "averages">       n_averages  = 1U;
    Annotated<gr::Size_t, "worker threads"> n_workers   = 1U;

    GR_MAKE_REFLECTABLE(ChangeRecordingBlock, in, out, sample_rate, fft_size, n_averages, n_workers);

    std::vector<property_map> oldSeen{};
    std::vector<property_map> newSeen{};

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void settingsChanged(const property_map& oldSettings, const property_map& newSettings) {
        oldSeen.push_back(oldSettings);
        newSeen.push_back(newSettings);
    }
};

void applySettings(ChangeRecordingBlock& block, const property_map& parameters) {
    std::ignore = block.settings().set(parameters);
    std::ignore = block.settings().activateContext();
    std::ignore = block.settings().applyStagedParameters();
}

/// a block whose one setting is vector-valued: the settings map holds it as a tensor, so whether a repeat of it is a
/// change is decided by tensor comparison. Every member carries an initializer of its own, as above.
struct TapsRecordingBlock : Block<TapsRecordingBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<std::vector<float>, "filter taps"> taps = std::vector<float>{1.0f};

    GR_MAKE_REFLECTABLE(TapsRecordingBlock, in, out, taps);

    std::size_t nSettingsChanged = 0UZ;

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { nSettingsChanged++; }
};

[[nodiscard]] std::optional<gr::Size_t> sizeOf(const property_map& map, std::string_view key) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return std::nullopt;
    }
    const gr::Size_t* value = it->second.get_if<gr::Size_t>();
    return value != nullptr ? std::optional<gr::Size_t>(*value) : std::nullopt;
}

[[nodiscard]] std::string contextOf(const property_map& map) {
    const auto it = map.find(gr::tag::CONTEXT.shortKey());
    if (it == map.end()) {
        return {};
    }
    const auto text = it->second.value_or(std::string_view{});
    return text.data() != nullptr ? std::string(text) : std::string{};
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

/// passes its input on scaled. The work function counts the samples in n_seen and copies that member into its settings
/// after each call. Nothing in the block writes or copies n_foreign. Every member carries an initializer of its own, as
/// above.
struct CopyingCounter : Block<CopyingCounter> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "output scale">        scale     = 1.0f;
    Annotated<gr::Size_t, "samples passed"> n_seen    = 0U;
    Annotated<gr::Size_t, "external count"> n_foreign = 0U;

    GR_MAKE_REFLECTABLE(CopyingCounter, in, out, scale, n_seen, n_foreign);

    std::atomic<std::size_t>* nCopies = nullptr;

    work::Status processBulk(InputSpanLike auto& inSpan, OutputSpanLike auto& outSpan) {
        std::ranges::transform(inSpan, outSpan.begin(), [this](float value) { return value * scale.value; });
        n_seen.value += static_cast<gr::Size_t>(inSpan.size());
        this->updateActiveParameters({"n_seen"});
        if (nCopies != nullptr && !inSpan.empty()) {
            nCopies->fetch_add(1UZ, std::memory_order_release);
        }
        return work::Status::OK;
    }
};

/// runs a CopyingCounter between a source and a sink on a scheduler in a thread of its own, and keeps every message
/// the scheduler sends out
struct RunningCounter {
    std::atomic<std::size_t> nCopies{0UZ};
    gr::scheduler::Simple<>  scheduler{};
    gr::MsgPortOut           toScheduler{};
    gr::MsgPortIn            fromScheduler{};
    std::vector<gr::Message> received{};
    CopyingCounter*          counter = nullptr;
    std::string              counterName{};
    std::thread              runner{};

    RunningCounter() {
        using namespace boost::ut;
        gr::Graph flow;
        auto&     source = flow.emplaceBlock<OnesSource>();
        auto&     block  = flow.emplaceBlock<CopyingCounter>();
        auto&     sink   = flow.emplaceBlock<DiscardSink>();
        block.nCopies    = &nCopies;
        counter          = &block;
        counterName      = std::string(block.unique_name);
        expect(flow.connect<"out", "in">(source, block).has_value());
        expect(flow.connect<"out", "in">(block, sink).has_value());
        expect(toScheduler.connect(scheduler.msgIn).has_value());
        expect(scheduler.msgOut.connect(fromScheduler).has_value());
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

    /// the first message received so far or later that carries the client request id and data
    [[nodiscard]] std::optional<property_map> awaitData(std::string_view clientRequestID) {
        std::optional<property_map> data;
        std::ignore = awaitTrue([this, clientRequestID, &data] {
            ReaderSpanLike auto messages = fromScheduler.streamReader().get<SpanReleasePolicy::ProcessAll>();
            received.insert(received.end(), messages.begin(), messages.end());
            std::ignore   = messages.consume(messages.size());
            const auto it = std::ranges::find_if(received, [clientRequestID](const gr::Message& message) { return message.clientRequestID == clientRequestID && message.data.has_value(); });
            if (it != received.end()) {
                data = *it->data;
            }
            return data.has_value();
        });
        return data;
    }

    [[nodiscard]] std::optional<property_map> readSettings() {
        gr::sendMessage<gr::message::Command::Get>(toScheduler, counterName, gr::block::property::kSetting, property_map{}, "read");
        return awaitData("read");
    }
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
        expect(nothrow([&] { std::ignore = block.settings().set({{"sample_rate", std::int64_t{20'000'000}}}); }));
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.sample_rate.value, 20'000'000.0f));
    };
};

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

    // an untagged YAML list parses to a tensor of type-erased values, whatever its elements hold
    "a type-erased value sequence converts element by element"_test = [] {
        const auto toSequence = [](const std::vector<pmt::Value>& elements) { return pmt::Value(gr::Tensor<pmt::Value>(elements.begin(), elements.end())); };

        const auto taps = gr::settings::convertParameter<std::vector<float>>("taps", toSequence({pmt::Value(0.5), pmt::Value(std::int64_t{2}), pmt::Value(0.25f)}));
        expect(taps.has_value()) << (taps.has_value() ? std::string{} : taps.error());
        if (taps.has_value()) {
            expect(eq(taps->size(), 3UZ));
            expect(eq((*taps)[0], 0.5f));
            expect(eq((*taps)[1], 2.0f));
            expect(eq((*taps)[2], 0.25f));
        }

        const auto weights = gr::settings::convertParameter<std::array<double, 2UZ>>("weights", toSequence({pmt::Value(std::int64_t{3}), pmt::Value(0.5f)}));
        expect(weights.has_value()) << (weights.has_value() ? std::string{} : weights.error());
        if (weights.has_value()) {
            expect(eq((*weights)[0], 3.0));
            expect(eq((*weights)[1], 0.5));
        }
        expect(!gr::settings::convertParameter<std::array<double, 3UZ>>("weights", toSequence({pmt::Value(3.0), pmt::Value(0.5)})).has_value()) << "a sequence whose length differs from the array's refuses";

        const auto refused = gr::settings::convertParameter<std::vector<float>>("taps", toSequence({pmt::Value(0.5), pmt::Value(std::pmr::string("two"))}));
        expect(!refused.has_value()) << "an element the scalar rule would refuse refuses the sequence";
        if (!refused.has_value()) {
            expect(refused.error().contains("taps")) << refused.error();
            expect(refused.error().contains("two")) << refused.error();
        }
    };

    "a native size_t survives the staging representation on every platform"_test = [] {
        constexpr std::size_t expected = 37UZ;
        const pmt::Value      staged{gr::detail::castToGrSizeIfNeeded(expected)};
        const auto            extracted = gr::settings::extractStagedValue<std::size_t>(staged, "num_filters");

        expect(extracted.has_value()) << "the staging representation converts back to native size_t";
        if (extracted.has_value()) {
            expect(eq(*extracted, expected));
        }
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

/// the error set() raises for the parameters, empty when it accepts them all
[[nodiscard]] std::string refusalOf(ChangeRecordingBlock& block, const property_map& parameters) {
    try {
        std::ignore = block.settings().set(parameters);
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
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

    "a float or double setting refuses text that holds no number"_test = [] {
        constexpr std::array blankTexts{"", " ", "\t", " \t\n\r\v\f ", "  # comment"};
        constexpr std::array trailingLetterTexts{"1.5abc", "48000Hz", "0 x"};
        for (std::string_view text : blankTexts) {
            const auto asFloat  = gr::settings::convertParameter<float>("sample_rate", pmt::Value{text});
            const auto asDouble = gr::settings::convertParameter<double>("frequency", pmt::Value{text});
            expect(!asFloat.has_value() && asFloat.error().contains("sample_rate")) << std::format("'{}' as a float setting", text);
            expect(!asDouble.has_value() && asDouble.error().contains("frequency")) << std::format("'{}' as a double setting", text);

            const auto floatReason  = fromText<float>(text);
            const auto doubleReason = fromText<double>(text);
            expect(!floatReason.has_value() && floatReason.error().contains("empty")) << floatReason.error_or("a value");
            expect(!doubleReason.has_value() && doubleReason.error().contains("empty")) << doubleReason.error_or("a value");
        }
        for (std::string_view text : trailingLetterTexts) {
            expect(!gr::settings::convertParameter<float>("sample_rate", pmt::Value{text}).has_value()) << std::format("'{}' as a float setting", text);
            expect(!gr::settings::convertParameter<double>("frequency", pmt::Value{text}).has_value()) << std::format("'{}' as a double setting", text);
        }

        for (std::string_view text : {std::string_view("0"), std::string_view(" 0 ")}) {
            const auto asFloat  = gr::settings::convertParameter<float>("sample_rate", pmt::Value{text});
            const auto asDouble = gr::settings::convertParameter<double>("frequency", pmt::Value{text});
            expect(asFloat == 0.0f) << asFloat.error_or("a different value");
            expect(asDouble == 0.0) << asDouble.error_or("a different value");
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

    "a change names the key that moved and no key that was merely set before"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}, {"n_averages", gr::Size_t(8)}, {"sample_rate", 48000.0f}}};
        block.init(std::make_shared<gr::Sequence>());

        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the three construction values are one change";
        expect(eq(block.newSeen.front().size(), 3UZ)) << "each construction value moves its own key and no other";
        block.oldSeen.clear();
        block.newSeen.clear();

        applySettings(block, {{"n_workers", gr::Size_t(4)}});

        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the new value is one change";
        const property_map& newSettings = block.newSeen.front();
        expect(eq(newSettings.size(), 1UZ)) << "only the key whose value moved is named";
        expect(eq(sizeOf(newSettings, "n_workers").value_or(0U), gr::Size_t(4))) << "the moved key is named with its new value";

        const property_map& oldSettings = block.oldSeen.front();
        expect(eq(sizeOf(oldSettings, "n_workers").value_or(0U), gr::Size_t(1))) << "the previous value of the moved key stays readable";
        expect(eq(sizeOf(oldSettings, "fft_size").value_or(0U), gr::Size_t(2048))) << "the settings before the change stay complete";
        expect(eq(block.fft_size.value, gr::Size_t(2048))) << "a key that is not named keeps its value";
    };

    "a setting re-applied at the value the block holds is no change"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}}};
        block.init(std::make_shared<gr::Sequence>());
        block.oldSeen.clear();
        block.newSeen.clear();

        applySettings(block, {{"fft_size", gr::Size_t(2048)}});

        expect(eq(block.newSeen.size(), 0UZ)) << "no value moved, so there is no change to report";
        expect(eq(block.fft_size.value, gr::Size_t(2048))) << "the value stays applied";
        expect(eq(sizeOf(block.settings().get(), "fft_size").value_or(0U), gr::Size_t(2048))) << "the settings still report the value";
    };

    "a vector setting re-applied at the value the block holds is no change"_test = [] {
        TapsRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());
        const std::size_t        nChangesAtStart = block.nSettingsChanged;
        const std::vector<float> taps{0.25f, 0.5f, 0.25f};

        std::ignore = block.settings().set({{"taps", taps}});
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.nSettingsChanged, nChangesAtStart + 1UZ)) << "the new vector is one change";
        expect(block.taps.value == taps) << "and it reaches the member";

        std::ignore = block.settings().set({{"taps", taps}});
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.nSettingsChanged, nChangesAtStart + 1UZ)) << "the same vector costs no further change";
        expect(block.taps.value == taps) << "and leaves the value applied";
    };

    "tags carrying equal vectors compare equal"_test = [] {
        const std::vector<float> taps{0.25f, 0.5f, 0.25f};

        const Tag first{0UZ, property_map{{"taps", taps}}};
        const Tag second{0UZ, property_map{{"taps", taps}}};
        const Tag third{0UZ, property_map{{"taps", std::vector<float>{0.25f, 0.5f}}}};

        expect(first == second) << "equal payloads make equal tags";
        expect(first != third) << "differing payloads make different tags";
    };

    "a change names only the moved key of a pair set together"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}}};
        block.init(std::make_shared<gr::Sequence>());
        block.oldSeen.clear();
        block.newSeen.clear();

        applySettings(block, {{"fft_size", gr::Size_t(2048)}, {"n_averages", gr::Size_t(16)}});

        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "the moved key is one change";
        const property_map& newSettings = block.newSeen.front();
        expect(eq(newSettings.size(), 1UZ)) << "the key set at its current value is not named";
        expect(eq(sizeOf(newSettings, "n_averages").value_or(0U), gr::Size_t(16))) << "the moved key is named";
    };

    "activating another context is reported even when no value moves"_test = [] {
        ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());

        // both contexts hold the same definition, so after the first activation neither can move a value
        const auto         now = gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
        const property_map definition{{"fft_size", gr::Size_t(4096)}, {"n_averages", gr::Size_t(8)}};
        expect(block.settings().set(definition, gr::SettingsCtx{now, "A"}).empty());
        expect(block.settings().set(definition, gr::SettingsCtx{now, "B"}).empty());
        expect(block.settings().activateContext(gr::SettingsCtx{now, "A"}).has_value());
        std::ignore = block.settings().applyStagedParameters();
        block.oldSeen.clear();
        block.newSeen.clear();

        expect(block.settings().activateContext(gr::SettingsCtx{now, "B"}).has_value());
        std::ignore = block.settings().applyStagedParameters();
        expect(fatal(eq(block.newSeen.size(), 1UZ))) << "activating B is a change of its own";
        expect(eq(block.newSeen.back().size(), 1UZ)) << "B carries the values the block holds, so no value key is named";
        expect(eq(contextOf(block.newSeen.back()), std::string("B"))) << "the newly active context is named";

        expect(block.settings().activateContext(gr::SettingsCtx{now, "A"}).has_value());
        std::ignore = block.settings().applyStagedParameters();
        expect(fatal(eq(block.newSeen.size(), 2UZ))) << "activating A again is a further change";
        expect(eq(block.newSeen.back().size(), 1UZ)) << "returning to A moves no value either";
        expect(eq(contextOf(block.newSeen.back()), std::string("A"))) << "the context that became active is named";
        expect(eq(block.fft_size.value, gr::Size_t(4096))) << "the activated definition stays applied";
    };

    "re-staging an identical definition with no context reports nothing"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}, {"n_averages", gr::Size_t(8)}}};
        block.init(std::make_shared<gr::Sequence>());
        block.oldSeen.clear();
        block.newSeen.clear();

        const property_map definition{{"fft_size", gr::Size_t(2048)}, {"n_averages", gr::Size_t(8)}};
        applySettings(block, definition);
        applySettings(block, definition);

        expect(eq(block.newSeen.size(), 0UZ)) << "the active context does not move, so there is no event and no value to report";
        expect(eq(block.fft_size.value, gr::Size_t(2048))) << "the definition stays applied";
    };

    "numeric settings given as text are parsed and applied"_test = [] {
        ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = refusalOf(block, {{"sample_rate", std::string("48000.5")}, {"fft_size", std::string("4096")}});
        expect(refusal.empty()) << refusal;
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.sample_rate.value, 48000.5f)) << "the float setting takes the parsed value";
        expect(eq(block.fft_size.value, gr::Size_t(4096))) << "the unsigned setting takes the parsed value";
    };

    "a numeric setting given text that does not parse is refused by its key"_test = [] {
        ChangeRecordingBlock block{property_map{{"fft_size", gr::Size_t(2048)}}};
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = refusalOf(block, {{"fft_size", std::string("4096.5")}});
        expect(refusal.contains("fft_size")) << refusal;
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();

        expect(eq(block.fft_size.value, gr::Size_t(2048))) << "the refused text does not reach the member";
    };
};

const boost::ut::suite<"a block copies its own members into its settings"> copyTests = [] {
    using namespace boost::ut;
    using namespace qa_settings;

    "the call copies the named members and returns their number"_test = [] {
        CopyingCounter block;
        block.init(std::make_shared<gr::Sequence>());
        block.n_seen.value    = 5U;
        block.n_foreign.value = 7U;

        expect(eq(block.updateActiveParameters({"n_seen", "scale"}), 2UZ)) << "each named member counts once";

        const property_map copied = block.settings().get();
        expect(eq(valueAt<gr::Size_t>(copied, "n_seen").value_or(0U), gr::Size_t(5))) << "the named member reaches the map";
        expect(eq(valueAt<gr::Size_t>(copied, "n_foreign").value_or(1U), gr::Size_t(0))) << "a member not named keeps the applied value";
    };

    "a misspelled key copies nothing and the call returns 0"_test = [] {
        CopyingCounter block;
        block.init(std::make_shared<gr::Sequence>());
        block.n_seen.value = 5U;

        expect(eq(block.updateActiveParameters({"n_sen"}), 0UZ)) << "the count includes a key no member carries";

        const property_map copied = block.settings().get();
        expect(eq(valueAt<gr::Size_t>(copied, "n_seen").value_or(1U), gr::Size_t(0))) << "the member the key misses keeps the applied value";
        expect(copied.find(std::string_view("n_sen")) == copied.end()) << "a key no member carries adds nothing";
    };

    "a Get reads the value the block copied from its work function"_test = [] {
        RunningCounter run;
        expect(fatal(run.awaitCopies(1UZ))) << "the counter never passed a sample";

        const std::optional<property_map> settings = run.readSettings();

        expect(fatal(settings.has_value())) << "the Get got no reply";
        expect(gt(valueAt<gr::Size_t>(*settings, "n_seen").value_or(0U), gr::Size_t(0))) << "the Get reads the declared default, not the copied member";
    };

    "a member written on another thread and never copied reads the last applied value"_test = [] {
        RunningCounter run;
        expect(fatal(run.awaitCopies(1UZ))) << "the counter never passed a sample";
        run.counter->n_foreign.value = 7U;

        const std::optional<property_map> settings = run.readSettings();

        expect(fatal(settings.has_value())) << "the Get got no reply";
        expect(eq(valueAt<gr::Size_t>(*settings, "n_foreign").value_or(1U), gr::Size_t(0))) << "a Get copied a member the block did not name";
        expect(gt(valueAt<gr::Size_t>(*settings, "n_seen").value_or(0U), gr::Size_t(0))) << "the same reply carries the copied member";
    };

    "a subscriber hears of the next applied change and of no copy before it"_test = [] {
        RunningCounter run;
        gr::sendMessage<gr::message::Command::Subscribe>(run.toScheduler, run.counterName, gr::block::property::kSetting, property_map{}, "watch");
        // the block handles its messages in order, so the reply to this Get comes after the subscription stands
        expect(fatal(run.readSettings().has_value())) << "the Get got no reply";
        expect(fatal(run.awaitCopies(1UZ))) << "the counter copied nothing after the subscription";

        gr::sendMessage<gr::message::Command::Set>(run.toScheduler, run.counterName, gr::block::property::kSetting, property_map{{"scale", 2.0f}}, "set");
        const std::optional<property_map> notice = run.awaitData("watch");

        expect(fatal(notice.has_value())) << "the subscriber heard nothing of the applied change";
        expect(eq(valueAt<float>(*notice, "scale").value_or(1.0f), 2.0f)) << "the first notice does not carry the applied scale";
        expect(gt(valueAt<gr::Size_t>(*notice, "n_seen").value_or(0U), gr::Size_t(0))) << "the notice carries the member's value";
    };
};

namespace qa_refusals {

using namespace gr;

/// a gain the block takes between 0 and 1
struct LimitedGain : Block<LimitedGain> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<0.f, 1.f>> gain = 0.5f;

    GR_MAKE_REFLECTABLE(LimitedGain, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

/// a gain the block takes between -1 and 1
struct SignedGain : Block<SignedGain> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<-1.f, 1.f>> gain = 0.5f;

    GR_MAKE_REFLECTABLE(SignedGain, in, out, gain);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

/// a float rate the block takes between 0 and 100, with integral limits
struct IntegralLimitedRate : Block<IntegralLimitedRate> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "rate", Limits<0, 100>> rate = 50.f;

    GR_MAKE_REFLECTABLE(IntegralLimitedRate, in, out, rate);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * rate; }
};

/// a gain whose default lies outside its limits, beside an offset without limits
struct OutOfLimitsDefault : Block<OutOfLimitsDefault> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<0.f, 1.f>> gain   = 2.f;
    float                                      offset = 0.f;

    GR_MAKE_REFLECTABLE(OutOfLimitsDefault, in, out, gain, offset);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain + offset; }
};

/// a gain whose default lies outside its limits, with a reset() that counts its calls
struct CountingReset : Block<CountingReset> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<0.f, 1.f>> gain = 2.f;

    GR_MAKE_REFLECTABLE(CountingReset, in, out, gain);

    std::size_t resets = 0;

    void reset() { ++resets; }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

/// a gain whose default lies inside its limits, with a reset() and a settingsChanged() that count their calls
struct CountingResetWithinLimits : Block<CountingResetWithinLimits> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<0.f, 1.f>> gain = 0.5f;

    GR_MAKE_REFLECTABLE(CountingResetWithinLimits, in, out, gain);

    std::size_t resets  = 0;
    std::size_t changes = 0;

    void reset() { ++resets; }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { ++changes; }
};

/// a gain beside a setting named like the reset key, with a reset() and a settingsChanged() that count their calls
struct ResetNamedSetting : Block<ResetNamedSetting> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain">                              gain          = 1.f;
    Annotated<bool, "a setting named like the reset key"> reset_default = false;

    GR_MAKE_REFLECTABLE(ResetNamedSetting, in, out, gain, reset_default);

    std::size_t resets  = 0;
    std::size_t changes = 0;

    void reset() { ++resets; }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { ++changes; }
};

/// a tag that carries only `key`, set to true
[[nodiscard]] Tag flagTag(std::string_view key) { return Tag{0UZ, property_map{{std::pmr::string(key), true}}}; }

[[nodiscard]] std::string setRefusal(SettingsBase& settings, const property_map& parameters, SettingsCtx ctx = {}) {
    try {
        std::ignore = settings.set(parameters, ctx);
    } catch (const gr::exception& e) {
        return e.message;
    }
    return {};
}

/// a gain between 0 and 1 beside an offset between -1 and 1
struct LimitedGainAndOffset : Block<LimitedGainAndOffset> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain", Limits<0.f, 1.f>>    gain   = 0.5f;
    Annotated<float, "offset", Limits<-1.f, 1.f>> offset = 0.f;

    GR_MAKE_REFLECTABLE(LimitedGainAndOffset, in, out, gain, offset);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain + offset; }
};

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

    Annotated<float, "gain", Limits<0.f, 1.f>> gain = 0.5f;

    GR_MAKE_REFLECTABLE(LimitedSink, in, gain);

    std::size_t nReceived = 0UZ;

    work::Status processBulk(std::span<const float> input) {
        nReceived += input.size();
        return work::Status::OK;
    }
};

/// a sink that counts its samples, with a count its settingsChanged() refuses at 0 and a level it takes at any value
struct RefusingSink : Block<RefusingSink> {
    PortIn<float> in{};

    gr::Size_t count = 4U;
    float      level = 1.f;

    GR_MAKE_REFLECTABLE(RefusingSink, in, count, level);

    std::size_t nReceived = 0UZ;

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings) {
        if (newSettings.contains("count") && count == 0U) {
            throw gr::exception("count must be at least 1");
        }
    }

    work::Status processBulk(std::span<const float> input) {
        nReceived += input.size();
        return work::Status::OK;
    }
};

/// a count whose default lies outside its limits, which settingsChanged() refuses above kMostTaken
struct LimitedRefusingBlock : Block<LimitedRefusingBlock> {
    static constexpr gr::Size_t kMostTaken = 4U;

    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<gr::Size_t, "count", Limits<1U, 10U>> count = 0U;

    GR_MAKE_REFLECTABLE(LimitedRefusingBlock, in, out, count);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings) {
        if (newSettings.contains("count") && count.value > kMostTaken) {
            throw gr::exception(std::format("count must be at most {}", kMostTaken));
        }
    }
};

/// a count its settingsChanged() refuses at 0, after it empties `newSettings`
struct ClearingRefusingBlock : Block<ClearingRefusingBlock> {
    PortIn<float>  in{};
    PortOut<float> out{};

    gr::Size_t count = 4U;

    GR_MAKE_REFLECTABLE(ClearingRefusingBlock, in, out, count);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }

    void settingsChanged(const property_map& /*oldSettings*/, property_map& newSettings) {
        if (count == 0U) {
            newSettings.clear();
            throw gr::exception("count must be at least 1");
        }
    }
};

/// a count its settingsChanged() refuses at 0, after it removes or activates the context a test names, once each
struct ContextChangingSink : Block<ContextChangingSink> {
    PortIn<float> in{};

    gr::Size_t count = 4U;

    GR_MAKE_REFLECTABLE(ContextChangingSink, in, count);

    std::optional<SettingsCtx> removeOnRefusal{};
    std::optional<SettingsCtx> activateOnRefusal{};
    bool                       removed = false;

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings) {
        if (!newSettings.contains("count") || count != 0U) {
            return;
        }
        if (removeOnRefusal.has_value()) {
            removed = this->settings().removeContext(*std::exchange(removeOnRefusal, std::nullopt));
        }
        if (activateOnRefusal.has_value()) {
            std::ignore = this->settings().activateContext(*std::exchange(activateOnRefusal, std::nullopt));
        }
        throw gr::exception("count must be at least 1");
    }

    work::Status processBulk(std::span<const float> /*input*/) { return work::Status::OK; }
};

constexpr std::string_view kRejectedSettings = "applyChangedSettings() - rejected settings";

/// the refusals a port received, the plain messages apart from the errors
struct ReportedRefusals {
    std::string plain;
    std::string errors;
};

[[nodiscard]] ReportedRefusals readRefusals(gr::MsgPortIn& port) {
    ReportedRefusals    reported;
    ReaderSpanLike auto messages = port.streamReader().get<SpanReleasePolicy::ProcessAll>();
    for (const gr::Message& message : messages) {
        if (!message.data.has_value()) {
            reported.errors += message.data.error().message;
        } else if (message.endpoint == kRejectedSettings) {
            const auto error = message.data->find("error");
            reported.plain += error == message.data->end() ? std::string() : error->second.value_or(std::string());
        }
    }
    std::ignore = messages.consume(messages.size());
    return reported;
}

struct LimitedSinkRun {
    std::expected<void, gr::Error> result;
    float                          gain      = 0.f;
    std::size_t                    nReceived = 0UZ;
    std::size_t                    nSent     = 0UZ;
    ReportedRefusals               reported{};
};

/// runs a source that tags its first sample with `tag` through a block that declares no tag policy into a LimitedSink.
/// A Set message carries `set` to the sink when it is not empty. A reader takes the scheduler's messages when
/// `readMessages` is true. With no reader a block's error ends the run.
[[nodiscard]] LimitedSinkRun runIntoLimitedSink(const property_map& tag, const property_map& set, bool readMessages) {
    using namespace boost::ut;
    gr::Graph flow;
    auto&     source = flow.emplaceBlock<FiniteTagSource>();
    auto&     pass   = flow.emplaceBlock<UndeclaredPass>();
    auto&     sink   = flow.emplaceBlock<LimitedSink>({{"name", std::string("limited_sink")}});
    source.tag       = tag;
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

    LimitedSinkRun run{.result = scheduler.runAndWait()};
    run.gain      = sink.gain.value;
    run.nReceived = sink.nReceived;
    run.nSent     = source.nSamples;
    if (readMessages) {
        run.reported = readRefusals(fromScheduler);
    }
    return run;
}

inline constexpr auto isEven       = [](const int& value) { return value % 2 == 0; };
inline constexpr auto anyCharacter = [](const char&) { return true; };

// true when Limit takes Character values at its limits and refuses values just outside them
template<typename Limit, typename Character>
constexpr bool limitsTakeCharacterByValue() {
    const bool edgesTaken   = Limit::validate(static_cast<Character>(Limit::MinRange)) && Limit::validate(static_cast<Character>(Limit::MaxRange));
    const bool aboveRefused = !Limit::validate(static_cast<Character>(Limit::MaxRange + 1));
    if constexpr (std::is_signed_v<Character>) {
        return edgesTaken && aboveRefused && !Limit::validate(static_cast<Character>(Limit::MinRange - 1));
    } else {
        return edgesTaken && aboveRefused;
    }
}

} // namespace qa_refusals

const boost::ut::suite<"settings refusals"> settingsRefusalTests = [] {
    using namespace boost::ut;
    using namespace qa_refusals;

    "a value outside the limits is refused by set(), naming the block, the key, the value and the limit"_test = [] {
        LimitedGain block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = setRefusal(block.settings(), {{"gain", 2.0f}});
        expect(refusal.contains("block 'gain_stage'")) << refusal;
        expect(refusal.contains("gain = 2")) << refusal;
        expect(refusal.contains("[0, 1]")) << refusal;

        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.gain.value, 0.5f)) << "the refused value does not reach the member";
    };

    "integral limits compare a float before converting it"_test = [] {
        using RateLimits  = decltype(IntegralLimitedRate::rate)::LimitType;
        const float lower = static_cast<float>(RateLimits::MinRange);
        const float upper = static_cast<float>(RateLimits::MaxRange);
        for (const float rate : {lower - 0.5f, upper + 0.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            IntegralLimitedRate block{property_map{{"name", std::string("rate_stage")}}};
            block.init(std::make_shared<gr::Sequence>());
            expect(!setRefusal(block.settings(), {{"rate", rate}}).empty()) << std::format("set() takes rate = {}", rate);
        }

        IntegralLimitedRate block{property_map{{"name", std::string("rate_stage")}}};
        block.init(std::make_shared<gr::Sequence>());
        const std::string refusal = setRefusal(block.settings(), {{"rate", upper - 0.5f}});
        expect(refusal.empty()) << refusal;
        std::ignore = block.settings().activateContext();
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.rate.value, upper - 0.5f)) << "an accepted value keeps its fraction";

        Annotated<float, "wide", Limits<std::int64_t{0}, std::numeric_limits<std::int64_t>::max()>> wide = 1.f;
        expect(!wide.validate_and_set(-0.5f)) << "below a lower limit of 0";
        expect(!wide.validate_and_set(0x1p64f)) << "beyond the int64_t range";
        expect(wide.validate_and_set(0.5f));
        expect(eq(wide.value, 0.5f)) << "an accepted value keeps its fraction";
    };

    "a floating value compares exactly with an integral limit the floating type rounds"_test = [] {
        constexpr std::int64_t                                         signedUpper = std::numeric_limits<std::int64_t>::max();
        const float                                                    floatUpper  = static_cast<float>(signedUpper);
        Annotated<float, "wide", Limits<std::int64_t{0}, signedUpper>> wide        = 1.f;
        expect(!wide.validate_and_set(floatUpper)) << "the rounded limit lies above the limit";
        expect(wide.validate_and_set(std::nextafter(floatUpper, 0.f))) << "the float below it lies inside";

        constexpr std::uint64_t                                                    unsignedUpper = std::numeric_limits<std::uint64_t>::max();
        const double                                                               doubleUpper   = static_cast<double>(unsignedUpper);
        Annotated<double, "unsignedWide", Limits<std::uint64_t{0}, unsignedUpper>> unsignedWide  = 1.0;
        expect(!unsignedWide.validate_and_set(doubleUpper)) << "the rounded limit lies above the limit";
        expect(unsignedWide.validate_and_set(std::nextafter(doubleUpper, 0.0))) << "the double below it lies inside";

        constexpr std::int32_t                                            narrowUpper = std::numeric_limits<std::int32_t>::max();
        Annotated<double, "narrow", Limits<std::int32_t{0}, narrowUpper>> narrow      = 1.0;
        expect(narrow.validate_and_set(static_cast<double>(narrowUpper))) << "a limit the double holds exactly is inside";
        expect(!narrow.validate_and_set(static_cast<double>(narrowUpper) + 0.5)) << "and a fraction above it is outside";

        constexpr std::int32_t twoToFloatDigits = std::int32_t{1} << std::numeric_limits<float>::digits;

        constexpr std::int32_t                                          oddUpper     = twoToFloatDigits + 3;
        const float                                                     roundedUpper = static_cast<float>(oddUpper);
        Annotated<float, "belowOdd", Limits<std::int32_t{0}, oddUpper>> belowOdd     = 1.f;
        expect(static_cast<double>(roundedUpper) > static_cast<double>(oddUpper)) << "float rounds the upper limit up";
        expect(!belowOdd.validate_and_set(roundedUpper)) << "the rounded upper limit lies above the limit";
        expect(belowOdd.validate_and_set(std::nextafter(roundedUpper, 0.f))) << "the float below it lies inside";

        constexpr std::int32_t                                                                   oddLower     = twoToFloatDigits + 1;
        const float                                                                              roundedLower = static_cast<float>(oddLower);
        Annotated<float, "aboveOdd", Limits<oddLower, std::numeric_limits<std::int32_t>::max()>> aboveOdd     = static_cast<float>(twoToFloatDigits) * 2.f;
        expect(static_cast<double>(roundedLower) < static_cast<double>(oddLower)) << "float rounds the lower limit down";
        expect(!aboveOdd.validate_and_set(roundedLower)) << "the rounded lower limit lies below the limit";
        expect(aboveOdd.validate_and_set(std::nextafter(roundedLower, std::numeric_limits<float>::max()))) << "the float above it lies inside";
    };

    "a character or bool value compares with integral limits by its integer value"_test = [] {
        using Percent = Limits<0, 100>;
        static_assert(Percent::validate(char{5}));
        static_assert(limitsTakeCharacterByValue<Percent, char>());
        static_assert(limitsTakeCharacterByValue<Percent, signed char>());
        static_assert(limitsTakeCharacterByValue<Percent, unsigned char>());
        static_assert(limitsTakeCharacterByValue<Percent, wchar_t>());
        static_assert(limitsTakeCharacterByValue<Percent, char8_t>());
        static_assert(limitsTakeCharacterByValue<Percent, char16_t>());
        static_assert(limitsTakeCharacterByValue<Percent, char32_t>());
        static_assert(Percent::validate(false) && Percent::validate(true));
        static_assert(!Limits<2, 100>::validate(true));

        using Digit = Limits<'0', '9'>;
        static_assert(Digit::validate(int{'5'}));
        static_assert(!Digit::validate(int{'0'} - 1) && !Digit::validate(int{'9'} + 1));

        using EvenPercent = Limits<0, 100, isEven>;
        static_assert(EvenPercent::validate(char{4}) && !EvenPercent::validate(char{5}));

        using AnyCharacter = Limits<char{0}, char{0}, anyCharacter>;
        static_assert(AnyCharacter::validate(int{'a'}));
        static_assert(!AnyCharacter::validate(int{std::numeric_limits<char>::max()} + 1));

        Annotated<char, "letter", Percent> letter = char{5};
        expect(letter.validate_and_set(static_cast<char>(Percent::MaxRange)));
        expect(!letter.validate_and_set(static_cast<char>(Percent::MaxRange + 1)));
        expect(eq(letter.value, static_cast<char>(Percent::MaxRange))) << "the refused value does not reach the member";
    };

    "a refused set() leaves the keys that follow tags as they were"_test = [] {
        LimitedGain block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());
        const auto        now = gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
        const SettingsCtx night{now, "night"};
        expect(setRefusal(block.settings(), {{"gain", 0.25f}}, night).empty());
        const std::set<std::string> following = block.settings().autoUpdateParameters(night);
        const gr::Size_t            nSets     = block.settings().getNAutoUpdateParameters();
        expect(fatal(following.contains("disconnect_on_done") && following.contains("compute_domain") && following.contains("name")));

        const property_map refused{{"disconnect_on_done", false}, {"compute_domain", std::string("default_cpu")}, {"name", std::string("renamed")}, {"gain", 2.0f}};
        expect(setRefusal(block.settings(), refused, night).contains("[0, 1]"));
        expect(setRefusal(block.settings(), refused, SettingsCtx{now + 1U, "dawn"}).contains("[0, 1]"));
        expect(block.settings().autoUpdateParameters(night) == following) << "a refused map changes which keys follow tags";
        expect(eq(block.settings().getNAutoUpdateParameters(), nSets)) << "a refused map leaves a set for a context it never stored";
    };

    "a refused set() that renames the block names it by the name it keeps"_test = [] {
        LimitedGain block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());

        const std::string refusal = setRefusal(block.settings(), {{"name", std::string("renamed")}, {"gain", 2.0f}});
        expect(refusal.contains("block 'gain_stage'")) << refusal;
        expect(!refusal.contains("renamed")) << "the refusal names the block by a name it never takes" << refusal;
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
        expect(refusal.contains("[0, 1]")) << refusal;
        expect(flow.blocks().empty()) << "the refused block is in the graph";
    };

    "a block whose staged initial value is refused fails its init with the refusal"_test = [] {
        LimitedGain block{property_map{{"name", std::string("gain_stage")}}};
        std::ignore = block.settings().setStaged({{"gain", 2.0f}});
        block.init(std::make_shared<gr::Sequence>());

        expect(block.state() == lifecycle::State::ERROR) << "the block reads initialized";
        expect(fatal(block._initError.has_value())) << "the block keeps no init error";
        expect(block._initError->message.contains("block 'gain_stage'")) << block._initError->message;
        expect(block._initError->message.contains("[0, 1]")) << block._initError->message;
    };

    "a staged value refused at the apply reaches the message output with the refusal"_test = [] {
        LimitedGain   block{property_map{{"name", std::string("gain_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        std::ignore = block.settings().setStaged({{"gain", 2.0f}});
        block.applyChangedSettings();

        std::string         reported;
        ReaderSpanLike auto messages = fromBlock.streamReader().get<SpanReleasePolicy::ProcessAll>();
        for (const gr::Message& message : messages) {
            if (!message.data.has_value()) {
                reported += message.data.error().message;
            }
        }
        std::ignore = messages.consume(messages.size());
        expect(reported.contains("block 'gain_stage'")) << reported;
        expect(reported.contains("gain = 2")) << reported;
        expect(reported.contains("[0, 1]")) << reported;
        expect(eq(block.gain.value, 0.5f)) << "the refused value does not reach the member";
    };

    "a tag value the limits refuse is reported in a plain message, and the run completes"_test = [] {
        const property_map tag{{"gain", 2.0f}};

        const LimitedSinkRun unread = runIntoLimitedSink(tag, {}, false);
        expect(unread.result.has_value()) << "the refused tag value ends the run: " << (unread.result.has_value() ? std::string() : unread.result.error().message);
        expect(eq(unread.gain, 0.5f)) << "the refused value reaches the member";
        expect(eq(unread.nReceived, unread.nSent)) << "the sink misses samples";

        const LimitedSinkRun read = runIntoLimitedSink(tag, {}, true);
        expect(read.result.has_value());
        expect(read.reported.plain.contains("block 'limited_sink' refuses gain = 2")) << "no plain message names the refusal, got: " << read.reported.plain;
        expect(read.reported.plain.contains("[0, 1]")) << read.reported.plain;
        expect(read.reported.errors.empty()) << "the refused tag value is reported as an error: " << read.reported.errors;
    };

    "a value a Set message stages and the limits refuse ends the run in the error"_test = [] {
        const LimitedSinkRun run = runIntoLimitedSink({}, {{"gain", 2.0f}}, false);
        expect(fatal(!run.result.has_value())) << "the run completes past the refused value";
        expect(run.result.error().message.contains(kRejectedSettings)) << run.result.error().message;
        expect(run.result.error().message.contains("block 'limited_sink' refuses gain = 2")) << run.result.error().message;
        expect(eq(run.gain, 0.5f)) << "the refused value reaches the member";
    };

    "a value settingsChanged() refuses leaves the member and settings().get() at the value last taken"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t taken = block.count + 2U;
        std::ignore            = block.settings().setStaged({{"count", taken}});
        block.applyChangedSettings();
        expect(fatal(eq(block.count, taken))) << "the block does not take a count it accepts";

        std::ignore = block.settings().setStaged({{"count", gr::Size_t{0U}}});
        block.applyChangedSettings();
        const ReportedRefusals reported = readRefusals(fromBlock);
        expect(reported.errors.contains("count must be at least 1")) << "the refusal does not reach the message output, got: " << reported.errors;
        expect(eq(block.count, taken)) << "the refused value stays in the member";
        const std::optional<pmt::Value> got = block.settings().get("count");
        expect(fatal(got.has_value()));
        expect(eq(got->value_or(gr::Size_t{0U}), taken)) << "settings().get() returns the refused value";
    };

    "a value set() stores and settingsChanged() refuses is not applied again by a later activation"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t taken = block.count + 2U;
        std::ignore            = block.settings().set({{"count", taken}});
        std::ignore            = block.settings().activateContext();
        block.applyChangedSettings();
        expect(fatal(eq(block.count, taken))) << "the block does not take a count it accepts";

        std::ignore = block.settings().set({{"count", gr::Size_t{0U}}});
        std::ignore = block.settings().activateContext();
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the refusal does not reach the message output";
        expect(eq(block.count, taken)) << "the refused value stays in the member";
        const std::optional<pmt::Value> stored = block.settings().getStored("count");
        expect(fatal(stored.has_value()));
        expect(eq(stored->value_or(gr::Size_t{0U}), taken)) << "the refused value stays stored";

        const float level = block.level * 2.f;
        std::ignore       = block.settings().set({{"level", level}});
        std::ignore       = block.settings().activateContext();
        block.applyChangedSettings();
        const ReportedRefusals later = readRefusals(fromBlock);
        expect(later.errors.empty()) << "the activation of another key applies the refused value again: " << later.errors;
        expect(eq(block.level, level)) << "the block does not take the other key";
        expect(eq(block.count, taken));
    };

    "a refused activation of another context returns to the earlier context and leaves the refused one's stored set as set() stored it"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t   held       = block.count;
        const SettingsCtx  earlierCtx = block.settings().activeContext();
        const SettingsCtx  refusedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "refused"};
        const property_map refusedSet{{"count", gr::Size_t{0U}}, {"level", block.level * 2.f}};
        expect(block.settings().set(refusedSet, refusedCtx).empty());
        expect(block.settings().activateContext(refusedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the activation is not refused";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext() == earlierCtx) << "the refused context stays active";
        for (const auto& [key, value] : refusedSet) {
            const std::optional<pmt::Value> stored = block.settings().getStored(std::string(std::string_view(key)), refusedCtx);
            expect(stored.has_value() && *stored == value) << "the refused context's stored set changes at " << key;
        }

        expect(block.settings().activateContext(refusedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "activating the refused context again does not reach the callback";
        expect(eq(block.count, held));
        expect(block.settings().activeContext() == earlierCtx) << "the refused context stays active after a second refusal";
    };

    "a refused batch that removeContext() staged keeps the context it falls back to"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const SettingsCtx removedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "removed"};
        expect(block.settings().set({{"count", gr::Size_t{block.count + 1U}}}, removedCtx).empty());
        expect(block.settings().activateContext(removedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.empty()) << "the activation is refused";
        const gr::Size_t held = block.count;

        // the default context's newest stored set holds a count the callback refuses
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, SettingsCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), ""}).empty());
        expect(block.settings().removeContext(removedCtx));
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the fallback to the default context is not refused";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext().context.value_or(std::string_view{}).empty()) << "the removed context is active again";
    };

    "a refused activation whose earlier context removeContext() removed before the apply returns to the default context"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const SettingsCtx earlierCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "earlier"};
        expect(block.settings().set({{"count", gr::Size_t{block.count + 1U}}}, earlierCtx).empty());
        expect(block.settings().activateContext(earlierCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.empty()) << "the earlier context is refused";
        const gr::Size_t held = block.count;

        const SettingsCtx refusedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "refused"};
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, refusedCtx).empty());
        expect(block.settings().activateContext(refusedCtx).has_value());
        expect(block.settings().removeContext(earlierCtx));
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the activation is not refused";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext().context.value_or(std::string_view{}).empty()) << "the default context is not active";

        expect(block.settings().activateContext(refusedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "activating the refused context again does not reach the callback";
    };

    "a refused activation returns to the earlier context when set() stored a newer set of it before the apply"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const SettingsCtx earlierCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "earlier"};
        expect(block.settings().set({{"count", gr::Size_t{block.count + 1U}}}, earlierCtx).empty());
        expect(block.settings().activateContext(earlierCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.empty()) << "the earlier context is refused";
        const gr::Size_t held  = block.count;
        const gr::Size_t newer = held + 1U;

        const SettingsCtx refusedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "refused"};
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, refusedCtx).empty());
        expect(block.settings().activateContext(refusedCtx).has_value());
        // a newer set of the earlier context prunes the set the activation recorded
        expect(block.settings().set({{"count", newer}}, SettingsCtx{0U, "earlier"}).empty());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the activation is not refused";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext().context == earlierCtx.context) << "the earlier context is not active";

        expect(block.settings().activateContext(SettingsCtx{0U, "earlier"}).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.empty()) << "the earlier context's newer set is refused";
        expect(eq(block.count, newer)) << "activating the earlier context again does not stage its newer set";
    };

    "a refused activation whose earlier context settingsChanged() removes returns to the default context"_test = [] {
        ContextChangingSink block{property_map{{"name", std::string("removing_stage")}}};
        gr::MsgPortIn       fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const SettingsCtx earlierCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "earlier"};
        expect(block.settings().set({{"count", gr::Size_t{block.count + 1U}}}, earlierCtx).empty());
        expect(block.settings().activateContext(earlierCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.empty()) << "the earlier context is refused";
        const gr::Size_t held = block.count;

        const SettingsCtx refusedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "refused"};
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, refusedCtx).empty());
        block.removeOnRefusal = earlierCtx;
        expect(block.settings().activateContext(refusedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the activation is not refused";
        expect(block.removed) << "the callback does not remove the earlier context";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext().context.value_or(std::string_view{}).empty()) << "the default context is not active";
    };

    "a context settingsChanged() activates before it refuses does not return to the refused context when it is refused too"_test = [] {
        ContextChangingSink block{property_map{{"name", std::string("activating_stage")}}};
        gr::MsgPortIn       fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t  held       = block.count;
        const SettingsCtx earlierCtx = block.settings().activeContext();
        const SettingsCtx refusedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "refused"};
        const SettingsCtx laterCtx{refusedCtx.time + 1U, "later"};
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, refusedCtx).empty());
        expect(block.settings().set({{"count", gr::Size_t{0U}}}, laterCtx).empty());
        block.activateOnRefusal = laterCtx;
        expect(block.settings().activateContext(refusedCtx).has_value());
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the activation is not refused";
        expect(block.settings().activeContext() == laterCtx) << "the context the callback activates is not active";

        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the context the callback activates is not refused";
        expect(eq(block.count, held)) << "the refused value stays in the member";
        expect(block.settings().activeContext() == earlierCtx) << "a refused context is active";
    };

    "a value settingsChanged() refuses leaves the stored set of a context the callback activates as that context stored it"_test = [] {
        const gr::Size_t refused = 0U;
        for (const gr::Size_t storedInNext : {refused, gr::Size_t{refused + 5U}}) {
            ContextChangingSink block{property_map{{"name", std::string("activating_stage")}}};
            gr::MsgPortIn       fromBlock;
            expect(block.msgOut.connect(fromBlock).has_value());
            block.init(std::make_shared<gr::Sequence>());

            const gr::Size_t  held = block.count;
            const SettingsCtx nextCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "next"};
            expect(block.settings().set({{"count", storedInNext}}, nextCtx).empty());
            block.activateOnRefusal = nextCtx;
            std::ignore             = block.settings().setStaged({{"count", refused}});
            block.applyChangedSettings();
            expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the staged count is not refused";
            expect(eq(block.count, held)) << "the refused value stays in the member";
            expect(block.settings().activeContext() == nextCtx) << "the context the callback activates is not active";
            const std::optional<pmt::Value> stored = block.settings().getStored("count", nextCtx);
            expect(fatal(stored.has_value()));
            expect(eq(stored->value_or(gr::Size_t{held}), storedInNext)) << "the rollback rewrites the stored set of the context the callback activates";
        }
    };

    "a refused reset_default tag returns the block to the named context it was in"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const SettingsCtx namedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "named"};
        const float       namedLevel = 2.f * block.level;
        expect(block.settings().set({{"level", namedLevel}}, namedCtx).empty());
        expect(block.settings().activateContext(namedCtx).has_value());
        block.applyChangedSettings();
        expect(fatal(eq(block.level, namedLevel))) << "the block does not take the named context's level";

        block.settings().autoUpdate(Tag{0UZ, property_map{{std::pmr::string(gr::tag::RESET_DEFAULTS.key()), true}, {"count", gr::Size_t{0U}}}});
        block.applyChangedSettings();
        const ReportedRefusals reported = readRefusals(fromBlock);
        expect((reported.errors + reported.plain).contains("count must be at least 1")) << "the reset with a count of 0 is not refused";
        expect(eq(block.level, namedLevel)) << "the refused reset restores the level";
        expect(block.settings().activeContext() == namedCtx) << "the refused reset leaves the default context active";
    };

    "a value settingsChanged() refuses after a reset_default tag left a named context keeps the default context"_test = [] {
        RefusingSink  block{property_map{{"name", std::string("refusing_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const float       defaultLevel = block.level;
        const SettingsCtx namedCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now()), "named"};
        expect(block.settings().set({{"level", 2.f * defaultLevel}}, namedCtx).empty());
        expect(block.settings().activateContext(namedCtx).has_value());
        block.applyChangedSettings();

        block.settings().autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.key()));
        block.applyChangedSettings();
        const SettingsCtx resetCtx = block.settings().activeContext();
        expect(fatal(eq(block.level, defaultLevel))) << "the reset does not restore the level";
        expect(fatal(resetCtx.context.value_or(std::string_view{}).empty())) << "the reset does not return to the default context";

        std::ignore = block.settings().setStaged({{"count", gr::Size_t{0U}}});
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at least 1")) << "the count of 0 is not refused";
        expect(block.settings().activeContext() == resetCtx) << "the refused value returns the block to the named context the reset left";
    };

    "a member whose limits refuse its previous value keeps the refused value, and its stored set agrees"_test = [] {
        LimitedRefusingBlock block{property_map{{"name", std::string("limited_refusing_stage")}}};
        gr::MsgPortIn        fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t refused = LimitedRefusingBlock::kMostTaken + 1U;
        expect(block.settings().set({{"count", refused}}).empty());
        std::ignore = block.settings().activateContext();
        block.applyChangedSettings();
        expect(readRefusals(fromBlock).errors.contains("count must be at most")) << "the refusal does not reach the message output";
        expect(eq(block.count.value, refused)) << "the member takes back a previous value outside its limits";
        const std::optional<pmt::Value> stored = block.settings().getStored("count");
        expect(fatal(stored.has_value()));
        expect(eq(stored->value_or(gr::Size_t{0U}), block.count.value)) << "the stored set and the member disagree";
    };

    "a refused value is written back when settingsChanged() empties newSettings before it throws"_test = [] {
        ClearingRefusingBlock block{property_map{{"name", std::string("clearing_stage")}}};
        gr::MsgPortIn         fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        const gr::Size_t held = block.count;
        std::ignore           = block.settings().setStaged({{"count", gr::Size_t{0U}}});
        block.applyChangedSettings();
        const ReportedRefusals reported = readRefusals(fromBlock);
        expect(reported.errors.contains("count must be at least 1")) << "the refusal is not an error, got: " << reported.errors;
        expect(reported.plain.empty()) << "the refusal of a staged value is a plain message: " << reported.plain;
        expect(eq(block.count, held)) << "the refused value stays in the member";
    };

    "a batch with a tag value and a staged value the limits refuse reports each refusal by where it came from"_test = [] {
        LimitedGainAndOffset block{property_map{{"name", std::string("pair_stage")}}};
        gr::MsgPortIn        fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(Tag{0UZ, property_map{{"gain", 2.0f}}});
        std::ignore = block.settings().setStaged({{"offset", 5.0f}});
        block.applyChangedSettings();

        const ReportedRefusals reported = readRefusals(fromBlock);
        expect(reported.errors.contains("block 'pair_stage' refuses offset = 5")) << "the error does not name the staged value, got: " << reported.errors;
        expect(!reported.errors.contains("gain =")) << "the error names the tag value: " << reported.errors;
        expect(reported.plain.contains("block 'pair_stage' refuses gain = 2")) << "the plain message does not name the tag value, got: " << reported.plain;
        expect(!reported.plain.contains("offset =")) << "the plain message names the staged value: " << reported.plain;
        expect(eq(block.gain.value, 0.5f) && eq(block.offset.value, 0.f)) << "a refused value reaches its member";
    };

    "a key a tag and setStaged() both stage is reported by the later of the two"_test = [] {
        LimitedGain   block{property_map{{"name", std::string("gain_stage")}}};
        gr::MsgPortIn fromBlock;
        expect(block.msgOut.connect(fromBlock).has_value());
        block.init(std::make_shared<gr::Sequence>());

        block.settings().autoUpdate(Tag{0UZ, property_map{{"gain", 2.0f}}});
        std::ignore = block.settings().setStaged({{"gain", 3.0f}});
        block.applyChangedSettings();
        const ReportedRefusals stagedLast = readRefusals(fromBlock);
        expect(stagedLast.errors.contains("refuses gain = 3")) << stagedLast.errors;
        expect(stagedLast.plain.empty()) << "a value setStaged() replaced is reported as a tag value: " << stagedLast.plain;

        std::ignore = block.settings().setStaged({{"gain", 3.0f}});
        block.settings().autoUpdate(Tag{0UZ, property_map{{"gain", 2.0f}}});
        block.applyChangedSettings();
        const ReportedRefusals taggedLast = readRefusals(fromBlock);
        expect(taggedLast.plain.contains("refuses gain = 2")) << taggedLast.plain;
        expect(taggedLast.errors.empty()) << "a value a tag replaced is reported as an error: " << taggedLast.errors;
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
        expect(refusal.contains("[0, 1]")) << "the first block's refusal is missing: " << refusal;
        expect(refusal.contains("[-1, 1]")) << "the second block's refusal is missing: " << refusal;
    };

    "a merged block given a value its blocks cannot convert names every block that refuses it"_test = [] {
        std::string refusal;
        try {
            gr::Merge<LimitedGain, "out", SignedGain, "in"> merged{property_map{{"gain", std::string("x")}}};
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("LimitedGain' refuses gain = ")) << "the first block's refusal is missing: " << refusal;
        expect(refusal.contains("SignedGain' refuses gain = ")) << "the second block's refusal is missing: " << refusal;
        expect(refusal.contains("the value does not convert to the setting's type")) << "the refusal gives another reason: " << refusal;
    };

    "a merged block given a name tells its blocks apart in a limits refusal"_test = [] {
        std::string refusal;
        try {
            gr::Merge<LimitedGain, "out", SignedGain, "in"> merged{property_map{{"name", std::string("m")}, {"gain", 2.0f}}};
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("LimitedGain: block 'm' refuses gain = 2")) << "the first block's refusal is missing: " << refusal;
        expect(refusal.contains("SignedGain: block 'm' refuses gain = 2")) << "the second block's refusal is missing: " << refusal;
    };

    "a merged block given a name tells its blocks apart in a conversion refusal"_test = [] {
        std::string refusal;
        try {
            gr::Merge<LimitedGain, "out", SignedGain, "in"> merged{property_map{{"name", std::string("m")}, {"gain", std::string("x")}}};
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("LimitedGain: block 'm' refuses gain = ")) << "the first block's refusal is missing: " << refusal;
        expect(refusal.contains("SignedGain: block 'm' refuses gain = ")) << "the second block's refusal is missing: " << refusal;
    };

    "a reset_default tag in either key form restores the default and calls the block's reset() once"_test = [] {
        for (const std::string_view key : {std::string_view(gr::tag::RESET_DEFAULTS.shortKey()), std::string_view(gr::tag::RESET_DEFAULTS.key())}) {
            CountingResetWithinLimits              block{property_map{{"name", std::string("gain_stage")}}};
            CtxSettings<CountingResetWithinLimits> settings{block};
            settings.init();
            const float defaultGain = block.gain.value;
            std::ignore             = settings.set({{"gain", defaultGain / 2.f}});
            expect(settings.activateContext().has_value());
            std::ignore = settings.applyStagedParameters();
            expect(eq(block.gain.value, defaultGain / 2.f)) << "the value to reset does not reach the member";

            const std::size_t resetsBefore = block.resets;
            settings.autoUpdate(flagTag(key));
            expect(settings.changed()) << key << " leaves the settings unchanged";
            const ApplyStagedParametersResult result = settings.applyStagedParameters();
            expect(result.refusal.empty()) << result.refusal;
            expect(eq(block.gain.value, defaultGain)) << key << " leaves the member off its default";
            expect(eq(block.resets, resetsBefore + 1UZ)) << key << " calls reset() a number of times other than once";

            settings.resetDefaults();
            expect(eq(block.resets, resetsBefore + 2UZ)) << "resetDefaults() calls reset() a number of times other than once";
        }
    };

    "a reset_default tag still staged when resetDefaults() runs is served by that reset"_test = [] {
        CountingResetWithinLimits              block{property_map{{"name", std::string("gain_stage")}}};
        CtxSettings<CountingResetWithinLimits> settings{block};
        settings.init();
        const float defaultGain = block.gain.value;
        std::ignore             = settings.set({{"gain", defaultGain / 2.f}});
        expect(settings.activateContext().has_value());
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, defaultGain / 2.f)) << "the value to reset does not reach the member";

        const std::size_t resetsBefore  = block.resets;
        const std::size_t changesBefore = block.changes;
        settings.autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
        settings.resetDefaults();
        expect(eq(block.gain.value, defaultGain)) << "the reset leaves the member off its default";
        expect(eq(block.resets, resetsBefore + 1UZ)) << "the staged reset and the call run reset() a number of times other than once";
        expect(eq(block.changes, changesBefore + 1UZ)) << "the staged reset and the call are reported as a number of changes other than one";

        std::ignore = settings.applyStagedParameters();
        expect(eq(block.resets, resetsBefore + 1UZ)) << "the staged reset runs again at the next apply";
    };

    "a reset_default tag the limits refuse leaves the block's reset() uncalled"_test = [] {
        CountingReset              block{property_map{{"name", std::string("gain_stage")}}};
        CtxSettings<CountingReset> settings{block};
        settings.init();
        std::ignore = settings.set({{"gain", 0.5f}});
        expect(settings.activateContext().has_value());
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, 0.5f)) << "the value to reset does not reach the member";

        const std::size_t resetsBefore = block.resets;
        settings.autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
        const ApplyStagedParametersResult result = settings.applyStagedParameters();
        expect(result.refusal.contains("refuses gain = 2")) << result.refusal;
        expect(eq(block.resets, resetsBefore)) << "the refused reset calls reset()";
        expect(eq(block.gain.value, 0.5f)) << "the refused default reaches the member";
    };

    "a store_default tag in either key form makes the current values the ones a reset restores"_test = [] {
        for (const std::string_view key : {std::string_view(gr::tag::STORE_DEFAULTS.shortKey()), std::string_view(gr::tag::STORE_DEFAULTS.key())}) {
            CountingResetWithinLimits              block{property_map{{"name", std::string("gain_stage")}}};
            CtxSettings<CountingResetWithinLimits> settings{block};
            settings.init();
            const float storedGain = block.gain.value / 2.f;
            std::ignore            = settings.set({{"gain", storedGain}});
            expect(settings.activateContext().has_value());
            std::ignore = settings.applyStagedParameters();

            settings.autoUpdate(flagTag(key));
            expect(settings.changed()) << key << " leaves the settings unchanged";
            std::ignore = settings.applyStagedParameters();

            std::ignore = settings.set({{"gain", storedGain / 2.f}});
            expect(settings.activateContext().has_value());
            std::ignore = settings.applyStagedParameters();
            expect(eq(block.gain.value, storedGain / 2.f)) << "the value set after the store does not reach the member";

            settings.autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
            std::ignore = settings.applyStagedParameters();
            expect(eq(block.gain.value, storedGain)) << "a reset after " << key << " restores a value other than the stored one";
        }
    };

    "a store_default tag stores the values the same tag carries"_test = [] {
        CountingResetWithinLimits              block{property_map{{"name", std::string("gain_stage")}}};
        CtxSettings<CountingResetWithinLimits> settings{block};
        settings.init();
        const float taggedGain = block.gain.value / 2.f;
        settings.autoUpdate(Tag{0UZ, property_map{{"gain", taggedGain}, {std::pmr::string(gr::tag::STORE_DEFAULTS.shortKey()), true}}});
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, taggedGain)) << "the tagged value does not reach the member";

        std::ignore = settings.set({{"gain", taggedGain / 2.f}});
        expect(settings.activateContext().has_value());
        std::ignore = settings.applyStagedParameters();

        settings.autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, taggedGain)) << "the store does not hold the value its own tag carries";
    };

    "a value staged with a reset_default tag at the value it held before is reported as no change"_test = [] {
        qa_settings::ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());
        const gr::Size_t setSize = 2U * block.fft_size.value;
        qa_settings::applySettings(block, {{"fft_size", setSize}});
        expect(eq(block.fft_size.value, setSize)) << "the value to reset does not reach the member";
        block.oldSeen.clear();
        block.newSeen.clear();

        std::ignore = block.settings().setStaged({{"fft_size", setSize}});
        block.settings().autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.fft_size.value, setSize)) << "the value staged with the reset does not reach the member";
        for (const property_map& reported : block.newSeen) {
            expect(!qa_settings::sizeOf(reported, "fft_size").has_value()) << "a change names fft_size, which ends at the value it began with";
        }
    };

    "a batch with a reset_default tag and a value of its own is reported as one change"_test = [] {
        qa_settings::ChangeRecordingBlock block;
        block.init(std::make_shared<gr::Sequence>());
        const gr::Size_t defaultSize     = block.fft_size.value;
        const gr::Size_t defaultAverages = block.n_averages.value;
        qa_settings::applySettings(block, {{"fft_size", 2U * defaultSize}, {"n_averages", 2U * defaultAverages}});
        expect(eq(block.fft_size.value, 2U * defaultSize) && eq(block.n_averages.value, 2U * defaultAverages)) << "the values to reset do not reach the members";
        block.oldSeen.clear();
        block.newSeen.clear();

        std::ignore = block.settings().setStaged({{"n_averages", 4U * defaultAverages}});
        block.settings().autoUpdate(flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.fft_size.value, defaultSize)) << "the reset leaves fft_size off its default";
        expect(eq(block.n_averages.value, 4U * defaultAverages)) << "the value staged with the reset does not reach the member";
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
        const bool        setValue      = !block.reset_default.value;

        expect(block.settings().setStaged({{gr::tag::RESET_DEFAULTS.shortKey(), setValue}}).empty()) << "the setting is not admitted";
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.reset_default.value, setValue)) << "the value does not reach the member";
        expect(eq(block.changes, changesBefore + 1UZ)) << "settingsChanged() runs a number of times other than once";
        expect(eq(block.resets, resetsBefore)) << "the setting resets the block";

        const property_map notSet = block.settings().setStaged({{gr::tag::RESET_DEFAULTS.key(), true}});
        expect(notSet.contains(gr::tag::RESET_DEFAULTS.key())) << "setStaged() admits the gr: form of the reset key";
    };

    "a reset to a default the limits refuse throws and leaves every setting as it was"_test = [] {
        OutOfLimitsDefault block{property_map{{"name", std::string("gain_stage")}}};
        block.init(std::make_shared<gr::Sequence>());
        std::ignore = block.settings().set({{"gain", 0.5f}, {"offset", 3.0f}});
        expect(block.settings().activateContext().has_value());
        std::ignore = block.settings().applyStagedParameters();
        expect(eq(block.gain.value, 0.5f) && eq(block.offset, 3.0f)) << "the values to reset do not reach the members";

        std::string refusal;
        try {
            block.settings().resetDefaults();
        } catch (const gr::exception& e) {
            refusal = e.message;
        }
        expect(refusal.contains("block 'gain_stage' refuses gain = 2")) << refusal;
        expect(refusal.contains("[0, 1]")) << refusal;
        expect(eq(block.gain.value, 0.5f)) << "the refused default reaches the member";
        expect(eq(block.offset, 3.0f)) << "the reset applies the other defaults";
    };
};

const boost::ut::suite<"keys that follow tags"> autoUpdateKeyTests = [] {
    using namespace boost::ut;
    using namespace qa_refusals;

    "a tag on an active set that a newer set of its context pruned leaves a key the context set explicitly"_test = [] {
        LimitedGainAndOffset              block{property_map{{"name", std::string("gain_stage")}}};
        CtxSettings<LimitedGainAndOffset> settings{block};
        settings.init();
        const float       setGain = block.gain.value / 2.f;
        const SettingsCtx activeCtx{gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now() - std::chrono::milliseconds(1)), "x"};
        expect(settings.set({{"gain", setGain}}, activeCtx).empty());
        expect(settings.activateContext(activeCtx).has_value());
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, setGain)) << "the explicit value does not reach the member";

        expect(settings.set({}, SettingsCtx{0U, "x"}).empty());
        const auto stored  = settings.getStoredAll();
        const auto setsOfX = stored.find(pmt::Value(std::string("x")));
        expect(fatal(setsOfX != stored.end()));
        expect(eq(setsOfX->second.size(), 1UZ)) << "the set for now leaves more than one set of the context";
        expect(setsOfX->second.front().context != activeCtx) << "the set for now leaves the active set stored";

        const float taggedOffset = block.offset.value + 0.5f;
        settings.autoUpdate(Tag{0UZ, property_map{{"gain", setGain / 2.f}, {"offset", taggedOffset}}});
        std::ignore = settings.applyStagedParameters();
        expect(eq(block.gain.value, setGain)) << "the tag overrides the key the context set explicitly";
        expect(eq(block.offset.value, taggedOffset)) << "the tag leaves a key that follows tags";
    };
};

namespace qa_reset_callbacks {
using namespace gr;

/// a gain whose settingsChanged() and reset() read the block's own settings
struct SettingsReadingReset : Block<SettingsReadingReset> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain"> gain = 1.f;

    GR_MAKE_REFLECTABLE(SettingsReadingReset, in, out, gain);

    std::optional<float> gainReadByLastChange; // the gain that settings() held in the last settingsChanged()
    std::optional<float> gainReadByLastReset;  // the gain that settings() held in the last reset()
    std::size_t          resets = 0;

    void reset() {
        ++resets;
        gainReadByLastReset = readGain();
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { gainReadByLastChange = readGain(); }

    [[nodiscard]] std::optional<float> readGain() {
        const std::optional<pmt::Value> value     = this->settings().get(std::string("gain"));
        const float*                    gainValue = value.has_value() ? value->get_if<float>() : nullptr;
        return gainValue != nullptr ? std::optional<float>(*gainValue) : std::nullopt;
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

    "a reset by tag or by message returns when settingsChanged() and reset() read the settings"_test = [] {
        for (const bool byTag : {true, false}) {
            const std::string_view path  = byTag ? "a reset_default tag" : "a ResetDefaults message";
            auto                   block = std::make_unique<SettingsReadingReset>();
            block->init(std::make_shared<gr::Sequence>());
            const float defaultGain = block->gain.value;
            std::ignore             = block->settings().set({{"gain", defaultGain / 2.f}});
            expect(block->settings().activateContext().has_value());
            std::ignore = block->settings().applyStagedParameters();
            expect(eq(block->gain.value, defaultGain / 2.f)) << "the value to reset does not reach the member";
            block->gainReadByLastChange.reset();
            const std::size_t resetsBefore = block->resets;

            gr::MsgPortOut toBlock;
            if (!byTag) {
                expect(fatal(toBlock.connect(block->msgIn).has_value()));
                gr::sendMessage<gr::message::Command::Set>(toBlock, "", gr::block::property::kResetDefaults, gr::property_map{});
            }
            const bool returned = resetReturns(block, [byTag](SettingsReadingReset& target) {
                if (byTag) {
                    target.settings().autoUpdate(qa_refusals::flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
                    std::ignore = target.settings().applyStagedParameters();
                } else {
                    target.processScheduledMessages();
                }
            });
            expect(returned) << path << " does not return within " << kResetBound.count() << " s";
            if (!returned) {
                continue;
            }
            expect(eq(block->gain.value, defaultGain)) << path << " leaves the member off its default";
            expect(eq(block->resets, resetsBefore + 1UZ)) << path << " calls reset() a number of times other than once";
            expect(block->gainReadByLastChange == std::optional<float>(defaultGain)) << "settingsChanged() reads a gain other than the default after " << path;
            expect(block->gainReadByLastReset == std::optional<float>(defaultGain)) << "reset() reads a gain other than the default after " << path;
        }
    };
};

namespace qa_callback_staging {
using namespace gr;

/// a gain and offset stage whose reset() or settingsChanged() stages an offset once, when armed
struct StagingCallbacks : Block<StagingCallbacks> {
    PortIn<float>  in{};
    PortOut<float> out{};

    Annotated<float, "gain">   gain   = 1.f;
    Annotated<float, "offset"> offset = 0.f;

    GR_MAKE_REFLECTABLE(StagingCallbacks, in, out, gain, offset);

    std::optional<float> offsetFromReset;  // the offset the next reset() stages
    std::optional<float> offsetFromChange; // the offset the next settingsChanged() stages

    void reset() { stageOffsetOnce(offsetFromReset); }

    void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/) { stageOffsetOnce(offsetFromChange); }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain.value + offset.value; }

    void stageOffsetOnce(std::optional<float>& armed) {
        if (const std::optional<float> value = std::exchange(armed, std::nullopt); value.has_value()) {
            std::ignore = this->settings().setStaged({{"offset", *value}});
        }
    }
};

enum class StagingPath { resetTag, storeTag, contextActivation, resetCall };

[[nodiscard]] constexpr std::string_view describe(StagingPath path) noexcept {
    switch (path) {
    case StagingPath::resetTag: return "a reset_default tag whose reset() stages";
    case StagingPath::storeTag: return "a store_default tag whose settingsChanged() stages";
    case StagingPath::contextActivation: return "a context activation whose settingsChanged() stages";
    case StagingPath::resetCall: return "a resetDefaults() call whose reset() stages";
    }
    return "";
}

} // namespace qa_callback_staging

const boost::ut::suite<"a value a callback stages during an apply"> callbackStagingTests = [] {
    using namespace boost::ut;
    using namespace qa_callback_staging;

    "a value a callback stages during applyChangedSettings() is applied by the next one"_test = [] {
        for (const StagingPath path : {StagingPath::resetTag, StagingPath::storeTag, StagingPath::contextActivation, StagingPath::resetCall}) {
            StagingCallbacks block;
            block.init(std::make_shared<gr::Sequence>());
            const float defaultGain  = block.gain.value;
            const float stagedOffset = block.offset.value + 2.f;
            const float offsetBefore = block.offset.value;
            const auto  now          = gr::settings::convertTimePointToUint64Ns(std::chrono::system_clock::now());
            if (path == StagingPath::resetTag || path == StagingPath::resetCall) {
                block.offsetFromReset = stagedOffset;
            } else {
                block.offsetFromChange = stagedOffset;
            }

            switch (path) {
            case StagingPath::resetTag:
                block.settings().autoUpdate(qa_refusals::flagTag(gr::tag::RESET_DEFAULTS.shortKey()));
                block.applyChangedSettings();
                break;
            case StagingPath::storeTag:
                block.settings().autoUpdate(Tag{0UZ, property_map{{std::pmr::string(gr::tag::STORE_DEFAULTS.shortKey()), true}, {"gain", 2.f * defaultGain}}});
                block.applyChangedSettings();
                break;
            case StagingPath::contextActivation:
                expect(block.settings().set({{"gain", 2.f * defaultGain}}, gr::SettingsCtx{now, "B"}).empty());
                expect(block.settings().activateContext(gr::SettingsCtx{now, "B"}).has_value());
                block.applyChangedSettings();
                break;
            case StagingPath::resetCall: block.settings().resetDefaults(); break;
            }

            expect(!block.offsetFromReset.has_value() && !block.offsetFromChange.has_value()) << describe(path) << " runs no staging callback";
            expect(eq(block.offset.value, offsetBefore)) << describe(path) << " applies the staged offset within its own apply";
            expect(block.settings().changed()) << describe(path) << " leaves the staged offset unmarked";
            block.applyChangedSettings();
            expect(eq(block.offset.value, stagedOffset)) << "the apply after " << describe(path) << " leaves the staged offset unapplied";
        }
    };
};

int main() { /* not needed by the UT framework */ }
