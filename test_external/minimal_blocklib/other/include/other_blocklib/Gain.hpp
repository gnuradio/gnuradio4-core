#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

// deliberately the same header basename, and the same class name, as minimal_blocklib/Gain.hpp: the
// generated symbols are derived from that basename, so two libraries carrying it are what proves
// they are qualified by the module as well
namespace gr::other_blocklib {

GR_REGISTER_BLOCK(gr::other_blocklib::Gain, [T], [ float, double ])

template<typename T>
struct Gain : public gr::Block<Gain<T>> {
    gr::PortIn<T>  in;
    gr::PortOut<T> out;
    T              gain = T{1};

    GR_MAKE_REFLECTABLE(Gain, in, out, gain);

    constexpr T processOne(T value) const noexcept { return value * gain; }
};

} // namespace gr::other_blocklib
