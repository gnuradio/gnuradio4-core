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
 * re-apply cycles that a running graph performs routinely.
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
};

int main() { /* not needed by the UT framework */ }
