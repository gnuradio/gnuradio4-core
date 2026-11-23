#include <gnuradio-4.0/annotated.hpp>

#include <cstdint>

// two numbers where Limits takes predicates.
gr::Annotated<std::uint8_t, gr::Limits<0, 200>> member = std::uint8_t{1};

int main() { return member.value == 0 ? 1 : 0; }
