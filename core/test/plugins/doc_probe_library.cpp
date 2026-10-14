#include <string>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

/**
 * @brief A shared object whose blocks exercise the Markdown and HTML documents of grinfo.
 *
 * The type names `probe::Stage`, `probe_Stage` and `probe_Stage_1` make the anchor slugs `probe-stage`, `probe-stage`
 * and `probe-stage-1`. The second slug repeats the first, and its suffixed form repeats the third. The string setting
 * of `probe::Stage` defaults to Markdown emphasis and an HTML entity, which a document shows as written.
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

struct probe_Stage : gr::Block<probe_Stage> {
    using Description = gr::Doc<"passes its input through, under a type name whose slug repeats that of probe::Stage">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(probe_Stage, in, out);

    explicit probe_Stage(gr::property_map init = {}) : gr::Block<probe_Stage>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

struct probe_Stage_1 : gr::Block<probe_Stage_1> {
    using Description = gr::Doc<"passes its input through, under a type name whose slug is the suffixed slug of probe_Stage">;

    gr::PortIn<float>  in;
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(probe_Stage_1, in, out);

    explicit probe_Stage_1(gr::property_map init = {}) : gr::Block<probe_Stage_1>(std::move(init)) {}

    [[nodiscard]] constexpr float processOne(float value) const noexcept { return value; }
};

namespace {

const bool registeredStage [[maybe_unused]]       = gr::globalBlockRegistry().insert<probe::Stage>();
const bool registeredUnderscored [[maybe_unused]] = gr::globalBlockRegistry().insert<probe_Stage>();
const bool registeredSuffixed [[maybe_unused]]    = gr::globalBlockRegistry().insert<probe_Stage_1>();

} // namespace
