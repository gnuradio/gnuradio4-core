#include <string>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

/**
 * @brief A shared object whose blocks exercise the Markdown and HTML documents of grinfo.
 *
 * The string setting of `probe::Stage` defaults to Markdown emphasis and an HTML entity, which a document shows as
 * written.
 */
namespace probe {

struct Stage : gr::Block<Stage> {
    using Description = gr::Doc<"passes its input through, with a string setting that holds markup">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    gr::Annotated<std::string, "label", gr::Visible, gr::Doc<"text shown as written">> label = "*literal* &lt;";

    GR_MAKE_REFLECTABLE(Stage, in, out, label);

    explicit Stage(gr::property_map init = {}) : gr::Block<Stage>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

} // namespace probe

namespace {

const bool registeredStage [[maybe_unused]] = gr::globalBlockRegistry().insert<probe::Stage>();

} // namespace
