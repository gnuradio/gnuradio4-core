#include <tuple>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/tools/RunGraph.hpp>

#include "good_base_plugin.hpp"

// Registers core's test source and sink, then runs the graph file its command line names, as rungraph does. The
// blocks are compiled into the program. The run needs no plugin on any leg, native or Emscripten.
int main(int argc, char** argv) {
    gr::BlockRegistry& registry = gr::globalBlockRegistry();
    std::ignore                 = registry.insert<good::fixed_source<float>>();
    std::ignore                 = registry.insert<good::cout_sink<float>>();
    return gr::tools::runGraph(argc, argv);
}
