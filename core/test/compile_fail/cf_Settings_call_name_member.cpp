#include <gnuradio-4.0/Block.hpp>

// A block that declares a member named like one of the block's settings calls, which hides that call.
struct ReadyFlag : gr::Block<ReadyFlag> {
    gr::PortIn<float>  in;
    gr::PortOut<float> out;
    bool               ready = false;
    GR_MAKE_REFLECTABLE(ReadyFlag, in, out);

    [[nodiscard]] constexpr float processOne(float sample) const noexcept { return sample; }
};

int main() {
    ReadyFlag block;
    return block.ready ? 1 : 0;
}
