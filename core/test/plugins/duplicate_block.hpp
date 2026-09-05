#ifndef GR_TEST_DUPLICATE_BLOCK_HPP
#define GR_TEST_DUPLICATE_BLOCK_HPP

#include <gnuradio-4.0/Block.hpp>

/// the block that two block libraries both carry, so that the second to load registers over the first one's entries
namespace gr::testing {

struct LibraryDuplicate : gr::Block<LibraryDuplicate> {
    using Description = gr::Doc<"negates its input; two shared objects register it under one type name and alias">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(LibraryDuplicate, in, out);

    explicit LibraryDuplicate(gr::property_map init = {}) : gr::Block<LibraryDuplicate>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return -value; }
};

} // namespace gr::testing

#endif // GR_TEST_DUPLICATE_BLOCK_HPP
