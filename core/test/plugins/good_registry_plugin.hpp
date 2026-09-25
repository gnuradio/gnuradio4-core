#ifndef GR_TEST_GOOD_REGISTRY_PLUGIN_HPP
#define GR_TEST_GOOD_REGISTRY_PLUGIN_HPP

#include <format>
#include <string>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Port.hpp>

namespace good {

GR_REGISTER_BLOCK(good::identity, [T], [ float, double ])
template<typename T>
struct identity : gr::Block<identity<T>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    GR_MAKE_REFLECTABLE(identity, in, out);

    [[nodiscard]] constexpr T processOne(T value) const noexcept { return value; }
};

// Passes its input through and opens the resource its setting names when it starts, as a device source opens its
// device. start() records the name it read and refuses an empty one.
GR_REGISTER_BLOCK(good::start_recorder, [T], [ float, double ])
template<typename T>
struct start_recorder : gr::Block<start_recorder<T>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    std::string                                        resource;          // the name start() opens
    std::string                                        resource_at_start; // the name start() read
    gr::Annotated<float, "gain", gr::Limits<0.f, 1.f>> gain        = 1.f;
    float                                              sample_rate = 1.f; // forwarded downstream; zero or less is refused

    GR_MAKE_REFLECTABLE(start_recorder, in, out, resource, resource_at_start, gain, sample_rate);

    void start() {
        resource_at_start = resource;
        if (resource.empty()) {
            throw gr::exception("no resource to open");
        }
    }

    [[nodiscard]] constexpr T processOne(T value) const noexcept { return value; }

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        if (newSettings.contains("sample_rate") && !(sample_rate > 0.f)) {
            throw gr::exception(std::format("sample_rate {} is not positive", sample_rate));
        }
    }
};

} // namespace good

#endif
