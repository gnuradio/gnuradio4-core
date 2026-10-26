#include <gnuradio-4.0/Graph.hpp>

// A DroppedTagKeys key with a leading colon: the build refuses the list.
struct ColonDropper : gr::Block<ColonDropper, gr::DroppedTagKeys<":freq_est">> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ColonDropper, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<ColonDropper>();
    return 0;
}
