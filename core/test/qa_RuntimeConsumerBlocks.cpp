#include "qa_RuntimeConsumerBlocks.hpp"

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <string>

/**
 * The factory half of the demo consumer, and the only translation unit that names a block type.
 * qa_RuntimeConsumer.cpp wires and runs the same blocks by string and includes Runtime.hpp alone.
 * The compile-time gate measures that split.
 */

namespace qa_consumer {

namespace {
std::atomic<double>      gTotal{0.0};
std::atomic<std::size_t> gTotalSamples{0UZ};
} // namespace

void addToTotal(double sum, std::size_t nSamples) {
    gTotal.fetch_add(sum);
    gTotalSamples.fetch_add(nSamples);
}

double      total() { return gTotal.load(); }
std::size_t totalSamples() { return gTotalSamples.load(); }

struct Counter : gr::Block<Counter> {
    gr::PortOut<float> out;

    gr::Annotated<gr::Size_t, "n_samples"> n_samples = 1024U;

    GR_MAKE_REFLECTABLE(Counter, out, n_samples);

    gr::Size_t _emitted = 0U;

    explicit Counter(gr::property_map init = {}) : gr::Block<Counter>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (_emitted >= n_samples) {
            outSpan.publish(0UZ);
            return gr::work::Status::DONE;
        }
        const std::size_t n = std::min(outSpan.size(), static_cast<std::size_t>(n_samples - _emitted));
        for (std::size_t i = 0UZ; i < n; ++i) {
            outSpan[i] = static_cast<float>(_emitted + static_cast<gr::Size_t>(i));
        }
        _emitted += static_cast<gr::Size_t>(n);
        outSpan.publish(n);
        return gr::work::Status::OK;
    }
};

struct Gain : gr::Block<Gain> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<float, "gain"> gain = 1.0f;

    GR_MAKE_REFLECTABLE(Gain, in, out, gain);

    explicit Gain(gr::property_map init = {}) : gr::Block<Gain>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value * gain; }
};

struct Total : gr::Block<Total> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(Total, in);

    explicit Total(gr::property_map init = {}) : gr::Block<Total>(std::move(init)) {}

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan) {
        const std::size_t n   = inSpan.size();
        double            sum = 0.0;
        for (std::size_t i = 0UZ; i < n; ++i) {
            sum += static_cast<double>(inSpan[i]);
        }
        addToTotal(sum, n);
        inSpan.consumeTags(n);
        std::ignore = inSpan.consume(n);
        return gr::work::Status::OK;
    }
};

bool registerDemoBlocks() {
    gr::BlockRegistry& registry = gr::globalBlockRegistry();
    return registry.insert<Counter>("=demo::Counter") && registry.insert<Gain>("=demo::Gain") && registry.insert<Total>("=demo::Total");
}

} // namespace qa_consumer
