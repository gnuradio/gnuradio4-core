#include <gnuradio-4.0/BlockRegistry.hpp>

#include "duplicate_block.hpp"

/// one of two shared objects that register the same block, as duplicate installations of one block library do
namespace gr::testing {

const bool registered [[maybe_unused]] = gr::globalBlockRegistry().insert<LibraryDuplicate>("=test::library_duplicate");

} // namespace gr::testing
