#include <tuple>

#include <gnuradio-4.0/Graph.hpp>

// On a 1:1 block the tag window depends on whether the block supplies forwardTags(). This override deduces its return
// type from a body that reads the window. hasForwardTagsOverride() answers no for it, yet the work path calls it.
struct DeducedForwarderBlock : gr::Block<DeducedForwarderBlock> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(DeducedForwarderBlock, in, out);

    auto forwardTags(auto& inputSpans, auto& outputSpans, std::size_t processedIn) {
        if constexpr (DeducedForwarderBlock::hasWholeChunkTagWindow()) {
            std::ignore = processedIn;
        }
        std::ignore = inputSpans;
        std::ignore = outputSpans;
    }

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<DeducedForwarderBlock>();
    return 0;
}
