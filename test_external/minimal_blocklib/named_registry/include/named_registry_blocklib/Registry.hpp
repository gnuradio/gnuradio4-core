#pragma once

#include <gnuradio-4.0/BlockRegistry.hpp>

// The registry this library registers into, named to gr_add_block_library() through REGISTRY_HEADER
// and REGISTRY_INSTANCE. The generated definition units include this header in place of
// gnuradio-4.0/BlockRegistry.hpp and the registration units include it beside
// gnuradio-4.0/BlockRegistration.hpp, so it declares the instance and pulls in the registry itself.
gr::BlockRegistry& namedBlockRegistry();
