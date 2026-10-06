#include <gnuradio-4.0/Graph.hpp>

// FilteredTagPropagation keeps the auto-forward keys, and NoTagPropagation forwards no tag.
struct FilteredSilentBlock : gr::Block<FilteredSilentBlock, gr::FilteredTagPropagation, gr::NoTagPropagation> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FilteredSilentBlock, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<FilteredSilentBlock>();
    return 0;
}
