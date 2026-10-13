#include <gnuradio-4.0/BlockRegistry.hpp>

#include "opaque_stage.hpp"

// A shared object that registers its own stages. A program that includes the same header holds other types of the same
// names.
namespace gr::testing {

const bool registeredOpaqueStage [[maybe_unused]] = gr::globalBlockRegistry().insert<OpaqueStage<OpaqueSample>>("=test::opaque_stage");
const bool registeredLocalStage [[maybe_unused]]  = gr::globalBlockRegistry().insert<OpaqueStage<LocalSample>>("=test::local_stage");

} // namespace gr::testing
