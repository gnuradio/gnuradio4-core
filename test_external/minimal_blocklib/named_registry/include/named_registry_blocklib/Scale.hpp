#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

namespace gr::named_registry_blocklib {

GR_REGISTER_BLOCK(gr::named_registry_blocklib::Scale, [T], [ float, double ])

template<typename T>
struct Scale : public gr::Block<Scale<T>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    struct Settings {
        T factor = T{1};

        GR_MAKE_REFLECTABLE(Settings, factor);
    } settings;

    GR_MAKE_REFLECTABLE(Scale, in, out, settings);

    constexpr T processOne(T value) const noexcept { return value * settings.factor; }
};

} // namespace gr::named_registry_blocklib
