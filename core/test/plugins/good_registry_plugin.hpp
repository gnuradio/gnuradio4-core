#ifndef GR_TEST_GOOD_REGISTRY_PLUGIN_HPP
#define GR_TEST_GOOD_REGISTRY_PLUGIN_HPP

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

} // namespace good

#endif
