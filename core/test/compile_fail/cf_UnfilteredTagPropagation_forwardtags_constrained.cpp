#include <tuple>

#include <gnuradio-4.0/Graph.hpp>

// The work path calls forwardTags() with the span tuples of the block's own stream ports, so an override constrained to
// those still replaces the default forwarder and still has to be refused.
struct ConstrainedForwarderBlock : gr::Block<ConstrainedForwarderBlock, gr::UnfilteredTagPropagation> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ConstrainedForwarderBlock, in, out);

    template<typename TInputSpans, typename TOutputSpans>
    requires(std::tuple_size_v<TInputSpans> > 0UZ)
    void forwardTags(TInputSpans&, TOutputSpans&, std::size_t) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<ConstrainedForwarderBlock>();
    return 0;
}
