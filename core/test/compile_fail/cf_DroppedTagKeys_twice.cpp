#include <gnuradio-4.0/Graph.hpp>

// Two DroppedTagKeys lists on one block: the block must name every dropped key in one list.
struct TwoListsBlock : gr::Block<TwoListsBlock, gr::DroppedTagKeys<"freq_est">, gr::DroppedTagKeys<"phase_est">> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(TwoListsBlock, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<TwoListsBlock>();
    return 0;
}
