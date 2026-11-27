#include <gnuradio-4.0/annotated.hpp>

namespace cf_limits {
inline constexpr auto isEven = [](int value) { return value % 2 == 0; };
} // namespace cf_limits

// a range with a test and its text where Limits takes predicates.
gr::Annotated<int, gr::Limits<0, 100, cf_limits::isEven, "must be even">> member = 2;

int main() { return member.value == 0 ? 1 : 0; }
