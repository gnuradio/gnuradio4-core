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

    struct Settings {
        std::string                                                  resource; // the name start() opens
        gr::Annotated<float, gr::Limits<gr::Min<0.f>, gr::Max<1.f>>> gain        = 1.f;
        float                                                        sample_rate = 1.f; // tagged downstream, refused at zero or less

        GR_MAKE_REFLECTABLE(Settings, resource, gain, sample_rate);
    } settings;

    struct Status {
        std::string resource_at_start; // the name start() read

        GR_MAKE_REFLECTABLE(Status, resource_at_start);
    } status;

    GR_MAKE_REFLECTABLE(start_recorder, in, out, settings, status);

    bool rateTagged = false; // this run's first sample carries the rate

    void start() {
        status.resource_at_start = settings.resource;
        if (settings.resource.empty()) {
            throw gr::exception("no resource to open");
        }
        rateTagged = false;
    }

    // the setup forwards no tag, so the rate the block starts with goes downstream on its first sample, and each later
    // change by the hook's forward
    [[nodiscard]] T processOne(T value) {
        if (!rateTagged) {
            rateTagged = true;
            this->publishTag(gr::property_map{{gr::tag::SAMPLE_RATE.key(), settings.sample_rate}}, 0UZ);
        }
        return value;
    }

    void applySettings(const Settings& s, const Settings& old) {
        if (s.sample_rate == old.sample_rate) {
            return;
        }
        if (!(s.sample_rate > 0.f)) {
            throw gr::exception(std::format("sample_rate {} is not positive", s.sample_rate));
        }
        this->forwardTag(gr::tag::SAMPLE_RATE.key(), s.sample_rate);
    }
};

} // namespace good

#endif
