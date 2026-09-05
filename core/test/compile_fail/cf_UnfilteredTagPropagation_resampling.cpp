#include <gnuradio-4.0/Graph.hpp>

// UnfilteredTagPropagation promises that a tag keeps its offset, which a declared rate change gives up.
struct ResamplingBlock : gr::Block<ResamplingBlock, gr::UnfilteredTagPropagation, gr::Resampling<2U, 1U, true>> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(ResamplingBlock, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto&, gr::OutputSpanLike auto&) { return gr::work::Status::OK; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<ResamplingBlock>();
    return 0;
}
