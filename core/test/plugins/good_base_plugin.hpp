#ifndef GR_EXAMPLE_GOOD_PLUGIN_HPP
#define GR_EXAMPLE_GOOD_PLUGIN_HPP

#include <print>

#include <gnuradio-4.0/Plugin.hpp>

gr::plugin<>& grPluginInstance();

namespace good {

GR_REGISTER_BLOCK(good::cout_sink, [T], [ float, double ])
template<typename T>
struct cout_sink : public gr::Block<cout_sink<T>> {
    gr::PortIn<T> in;

    struct Settings {
        gr::Size_t total_count = std::numeric_limits<gr::Size_t>::max();

        GR_MAKE_REFLECTABLE(Settings, total_count);
    } settings;

    GR_MAKE_REFLECTABLE(cout_sink, in, settings);

    void processOne(T value) {
        settings.total_count--;
        if (settings.total_count == 0) {
            std::println(stderr, "last value was: {}", value);
        }
    }
};

GR_REGISTER_BLOCK(good::fixed_source, [T], [ float, double ])
template<typename T>
struct fixed_source : public gr::Block<fixed_source<T>> {
    gr::PortOut<T> out;

    struct Settings {
        gr::Size_t event_count = std::numeric_limits<gr::Size_t>::max();

        GR_MAKE_REFLECTABLE(Settings, event_count);
    } settings;
    T value = 0;

    GR_MAKE_REFLECTABLE(fixed_source, out, settings);

    [[nodiscard]] constexpr T processOne() noexcept {
        value++;
        if (settings.event_count != std::numeric_limits<gr::Size_t>::max() && static_cast<gr::Size_t>(value) >= settings.event_count) {
            this->requestStop();
        }
        return value;
    }
};
} // namespace good

#endif
