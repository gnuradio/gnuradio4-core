#ifndef GNURADIO_TEST_OPAQUE_STAGE_HPP
#define GNURADIO_TEST_OPAQUE_STAGE_HPP

#include <utility>

#include <gnuradio-4.0/Block.hpp>

// Every object that includes this header holds its own value types and stages. The types of two objects share their
// names and are distinct types.

// a local class of a function with internal linkage, outside any anonymous namespace
[[maybe_unused]] static auto makeLocalSample() {
    struct Sample {
        float value = 0.f;
    };
    return Sample{};
}

namespace gr::testing {

namespace {

struct OpaqueSample {
    float value = 0.f;
};

using LocalSample = decltype(makeLocalSample());

template<typename T>
struct OpaqueStage : gr::Block<OpaqueStage<T>> {
    using Description = gr::Doc<"passes its input through, on ports whose value type has no external linkage">;

    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    GR_MAKE_REFLECTABLE(OpaqueStage, in, out);

    explicit OpaqueStage(gr::property_map init = {}) : gr::Block<OpaqueStage<T>>(std::move(init)) {}

    [[nodiscard]] constexpr T processOne(T sample) const noexcept { return sample; }
};

} // namespace

} // namespace gr::testing

#endif // GNURADIO_TEST_OPAQUE_STAGE_HPP
