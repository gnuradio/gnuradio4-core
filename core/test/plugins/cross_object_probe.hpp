#ifndef GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP
#define GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP

#include <memory_resource>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>

namespace gr::testing {

// a compute domain whose provider the program registers
inline constexpr gr::ComputeDomain kCrossObjectDomain = gr::ComputeDomain::gpu_shared("cross_object");

/**
 * @brief A block that holds the program-wide values that the code constructing it reads.
 *
 * A shared object and the program that loads it both compile this type. The object registers it, so an instance from
 * the registry holds the values that the object's code reads.
 */
struct CrossObjectProbe : gr::Block<CrossObjectProbe> {
    using Description = gr::Doc<"holds a compute provider's resource that its constructing code reads">;

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CrossObjectProbe, in);

    std::pmr::memory_resource* providerResource = gr::ComputeRegistry::instance().tryResolve(kCrossObjectDomain);

    explicit CrossObjectProbe(gr::property_map init = {}) : gr::Block<CrossObjectProbe>(std::move(init)) {}

    void processOne(float) {}
};

} // namespace gr::testing

#endif // GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP
