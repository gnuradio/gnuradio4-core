#include <cstddef>
#include <cstdlib>
#include <print>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

// Nothing here names a registry. Constructing a gr::Graph reaches the plugin loader in the core archive, and the loader
// reads the block and scheduler registries of the registry library, so linking this program needs only
// gnuradio4::gnuradio-core.

struct CountingSource : gr::Block<CountingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountingSource, out);

    static constexpr std::size_t kSamples   = 1000UZ;
    std::size_t                  _nProduced = 0UZ;

    float processOne() {
        if (++_nProduced >= kSamples) {
            this->requestStop();
        }
        return 1.0F;
    }
};

struct CountingSink : gr::Block<CountingSink> {
    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CountingSink, in);

    std::size_t _nSeen = 0UZ;

    void processOne(float) { ++_nSeen; }
};

int main() {
    gr::Graph graph;
    auto&     source = graph.emplaceBlock<CountingSource>();
    auto&     sink   = graph.emplaceBlock<CountingSink>();
    if (!graph.connect<"out", "in">(source, sink).has_value()) {
        std::println(stderr, "the source and the sink did not connect");
        return 3;
    }

    gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded> scheduler;
    if (!scheduler.exchange(std::move(graph)).has_value()) {
        std::println(stderr, "the scheduler did not take the graph");
        return 4;
    }
    if (!scheduler.runAndWait().has_value()) {
        std::println(stderr, "the scheduler run failed");
        return 5;
    }
    if (sink._nSeen != source._nProduced) {
        std::println(stderr, "the sink saw {} samples, the source produced {}", sink._nSeen, source._nProduced);
        return 6;
    }
    if (sink._nSeen < CountingSource::kSamples) {
        std::println(stderr, "the sink saw {} samples, fewer than the {} that stop the source", sink._nSeen, CountingSource::kSamples);
        return 7;
    }
    std::println("the sink saw all {} samples of the source", sink._nSeen);
    return EXIT_SUCCESS;
}
