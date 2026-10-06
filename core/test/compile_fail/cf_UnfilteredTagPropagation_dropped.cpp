#include <gnuradio-4.0/Graph.hpp>

// UnfilteredTagPropagation requires every key, and DroppedTagKeys names keys the block drops.
struct UnfilteredDroppingBlock : gr::Block<UnfilteredDroppingBlock, gr::UnfilteredTagPropagation, gr::DroppedTagKeys<"phase_offset">> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(UnfilteredDroppingBlock, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<UnfilteredDroppingBlock>();
    return 0;
}
