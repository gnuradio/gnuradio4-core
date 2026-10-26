#include <tuple>

#include <gnuradio-4.0/Graph.hpp>

// The override deduces its return type from a body that reads the tag window, and on a resampling block the window
// depends on whether the block has an override. The probe cannot see this override, and the work path still calls it.
struct DeducedForwarderDecimator : gr::Block<DeducedForwarderDecimator, gr::Resampling<2U, 1U>> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(DeducedForwarderDecimator, in, out);

    auto forwardTags(auto& inputSpans, auto& outputSpans, std::size_t processedIn) {
        if constexpr (DeducedForwarderDecimator::hasWholeChunkTagWindow()) {
            std::ignore = processedIn;
        }
        std::ignore = inputSpans;
        std::ignore = outputSpans;
    }

    gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        std::ignore = inSpan.consume(inSpan.size());
        outSpan.publish(0UZ);
        return gr::work::Status::OK;
    }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<DeducedForwarderDecimator>();
    return 0;
}
