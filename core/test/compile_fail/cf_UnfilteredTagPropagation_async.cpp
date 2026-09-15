#include <gnuradio-4.0/Graph.hpp>

// The default forwarder skips an asynchronous input span, so no tag from that port would be forwarded at all.
struct AsyncInputBlock : gr::Block<AsyncInputBlock, gr::UnfilteredTagPropagation> {
    gr::PortIn<float, gr::Async> in;
    gr::PortOut<float>           out;

    GR_MAKE_REFLECTABLE(AsyncInputBlock, in, out);

    gr::work::Status processBulk(gr::InputSpanLike auto&, gr::OutputSpanLike auto&) { return gr::work::Status::OK; }
};

int main() {
    gr::Graph flow;
    std::ignore = flow.emplaceBlock<AsyncInputBlock>();
    return 0;
}
