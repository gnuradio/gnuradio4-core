#include <gnuradio-4.0/BlockRegistry.hpp>

#include "duplicate_block.hpp"

/// the other of the two: whichever of them loads second replaces the registry entries of the one that loaded first
namespace gr::testing {

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryDuplicate>("=test::library_duplicate");

} // namespace gr::testing
