#ifndef GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP
#define GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP

#include <expected>
#include <memory_resource>
#include <utility>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/CircularBuffer.hpp>
#include <gnuradio-4.0/ComputeDomain.hpp>

namespace gr::testing {

// a compute domain whose provider the program registers
inline constexpr gr::ComputeDomain kCrossObjectDomain = gr::ComputeDomain::gpu_shared("cross_object");

/**
 * @brief A block that holds the program-wide values that the code constructing it reads.
 *
 * A shared object and the program that loads it both compile this type. The object registers it, so an instance from
 * the registry holds the values that the object's code reads, and its two function pointers call the object's code.
 */
struct CrossObjectProbe : gr::Block<CrossObjectProbe> {
    using Description = gr::Doc<"holds the default buffer resource and a compute provider's resource that its constructing code reads">;

    gr::PortIn<float> in;

    GR_MAKE_REFLECTABLE(CrossObjectProbe, in);

    using SetResult = std::expected<gr::settings::Values, gr::settings::Rejected>;

    std::pmr::memory_resource* defaultResource  = gr::double_mapped_memory_resource::defaultAllocator();
    std::pmr::memory_resource* providerResource = gr::ComputeRegistry::instance().tryResolve(kCrossObjectDomain);
    // calls into the code that constructed the probe: a `set` of no key on the probe, and a call of `ask` on a thread
    // that this code marks as the probe's own
    SetResult (*setByConstructingCode)(CrossObjectProbe&)                                              = &setNoKey;
    SetResult (*askOnOwnThreadOfConstructingCode)(CrossObjectProbe&, SetResult (*)(CrossObjectProbe&)) = &askOnOwnThread;

    explicit CrossObjectProbe(gr::property_map init = {}) : gr::Block<CrossObjectProbe>(std::move(init)) {}

    void processOne(float) {}

    static SetResult setNoKey(CrossObjectProbe& probe) { return gr::settings::detail::settingsOf(probe).set(gr::property_map{}); }

    static SetResult askOnOwnThread(CrossObjectProbe& probe, SetResult (*ask)(CrossObjectProbe&)) {
        const gr::settings::detail::OwnThread mark(gr::settings::detail::settingsOf(probe));
        return ask(probe);
    }
};

} // namespace gr::testing

#endif // GNURADIO_TEST_CROSS_OBJECT_PROBE_HPP
