#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

namespace gr::minimal_blocklib {

GR_REGISTER_BLOCK(gr::minimal_blocklib::Gain, [T], [ float, double ])

template<typename T>
struct Gain : public gr::Block<Gain<T>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;
    T              gain = T{1};

    GR_MAKE_REFLECTABLE(Gain, in, out, gain);

    constexpr T processOne(T value) const noexcept { return value * gain; }
};

} // namespace gr::minimal_blocklib
