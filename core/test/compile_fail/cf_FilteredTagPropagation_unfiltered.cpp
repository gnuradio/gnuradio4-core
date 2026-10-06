#include <gnuradio-4.0/Graph.hpp>

// FilteredTagPropagation keeps the auto-forward keys alone, and UnfilteredTagPropagation requires every key.
struct BothKeyPoliciesBlock : gr::Block<BothKeyPoliciesBlock, gr::FilteredTagPropagation, gr::UnfilteredTagPropagation> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(BothKeyPoliciesBlock, in, out);

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<BothKeyPoliciesBlock>();
    return 0;
}
