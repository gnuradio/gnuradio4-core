#include <gnuradio-4.0/annotated.hpp>

namespace cf_limits {
enum class Taper { rectangular, hann, hamming };
}

// Min on an enumeration, which has no order a host may rely on.
gr::Annotated<cf_limits::Taper, gr::Limits<gr::Min<1>>> member = cf_limits::Taper::hann;

int main() { return member.value == cf_limits::Taper::rectangular ? 1 : 0; }
