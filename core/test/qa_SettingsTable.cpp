#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Sequence.hpp>
#include <gnuradio-4.0/Settings.hpp>

/**
 * @brief What the compiled settings machinery knows about a block is its member table.
 *
 * Every `Annotated` attribute the machinery used to read off the block type directly -- the
 * description, the documentation, the unit, the visibility and an enum member's value names --
 * now travels through `settings::MemberDescriptor`, while the limits stay behind the typed
 * accessor because a validator is a template argument, not data. These tests pin that the table
 * reports what the type declares, that `meta_information` repeats it, and that the limits bite.
 */

namespace qa_settings_table {

using namespace gr;

enum class Waveform { sine, square, triangle };

struct Annotations : Block<Annotations> {
    using Description = Doc<"a block carrying one member of every annotated shape">;

    PortIn<float>  in;
    PortOut<float> out;

    Annotated<float, "gain", Visible, Doc<"linear voltage gain">, Unit<"dB">, Limits<0.f, 10.f>> gain     = 1.0f;
    Annotated<Waveform, "waveform", Doc<"generated shape">>                                      waveform = Waveform::sine;
    Annotated<std::string, "label">                                                              label    = "unnamed";
    gr::Size_t                                                                                   count    = 0U;

    GR_MAKE_REFLECTABLE(Annotations, in, out, gain, waveform, label, count);

    [[nodiscard]] constexpr float processOne(float x) const noexcept { return gain * x; }
};

[[nodiscard]] const gr::settings::MemberDescriptor* memberNamed(std::string_view name) {
    const auto members = gr::settings::blockDescriptor<Annotations>().hooks.members;
    const auto it      = std::ranges::find(members, name, &gr::settings::MemberDescriptor::name);
    return it != members.end() ? &*it : nullptr;
}

[[nodiscard]] Annotations makeBlock() {
    Annotations block;
    block.init(std::make_shared<gr::Sequence>());
    return block;
}

[[nodiscard]] std::string metaString(const Annotations& block, const std::string& key) {
    const auto it = block.meta_information.value.find(gr::convert_string_domain(key));
    if (it == block.meta_information.value.end()) {
        return {};
    }
    return std::string(it->second.value_or(std::string_view{}));
}

} // namespace qa_settings_table

const boost::ut::suite<"settings table"> _settingsTable = [] {
    using namespace boost::ut;
    using namespace qa_settings_table;

    "the table names every reflected member, ports included"_test = [] {
        const auto& descriptor = gr::settings::blockDescriptor<Annotations>();

        expect(that % descriptor.hooks.reflectable);
        expect(eq(descriptor.hooks.members.size(), gr::refl::data_member_count<Annotations>));
        for (std::string_view name : {"in", "out", "gain", "waveform", "label", "count", "name", "unique_name", "input_chunk_size"}) {
            expect(memberNamed(name) != nullptr) << std::format("member '{}' is missing from the table", name);
        }
    };

    "a port takes part in no settings path"_test = [] {
        const gr::settings::MemberDescriptor* port = memberNamed("in");
        expect(fatal(port != nullptr));
        expect(port->setParameter == nullptr);
        expect(port->autoUpdate == nullptr);
        expect(port->applyStaged == nullptr);
        expect(port->readParameter == nullptr);
    };

    "a writable member carries the whole accessor set, a read-only member only the reader"_test = [] {
        const gr::settings::MemberDescriptor* gain = memberNamed("gain");
        expect(fatal(gain != nullptr));
        expect(gain->setParameter != nullptr);
        expect(gain->autoUpdate != nullptr);
        expect(gain->applyStaged != nullptr);
        expect(gain->readParameter != nullptr);

        const gr::settings::MemberDescriptor* uniqueName = memberNamed("unique_name");
        expect(fatal(uniqueName != nullptr));
        expect(uniqueName->setParameter == nullptr) << "an immutable member is never set";
        expect(uniqueName->readParameter != nullptr) << "an immutable member still reads back";
    };

    "the writable set is exactly the members carrying a setter"_test = [] {
        const auto& descriptor = gr::settings::blockDescriptor<Annotations>();

        std::set<std::string> withSetter;
        for (const gr::settings::MemberDescriptor& member : descriptor.hooks.members) {
            if (member.setParameter != nullptr) {
                withSetter.emplace(member.name);
            }
        }
        expect(descriptor.writableMembers == withSetter);
        expect(descriptor.writableMembers == gr::CtxSettings<Annotations>::allWritableMembers());
        expect(that % descriptor.writableMembers.contains("gain"));
        expect(that % !descriptor.writableMembers.contains("in"));
        expect(that % !descriptor.writableMembers.contains("unique_name"));
    };

    "an annotated member reports its description, documentation, unit and visibility"_test = [] {
        const gr::settings::MemberDescriptor* gain = memberNamed("gain");
        expect(fatal(gain != nullptr));
        expect(that % gain->isAnnotated);
        expect(eq(gain->description, std::string_view("gain")));
        expect(eq(gain->documentation, std::string_view("linear voltage gain")));
        expect(eq(gain->unit, std::string_view("dB")));
        expect(that % gain->visible);

        const gr::settings::MemberDescriptor* label = memberNamed("label");
        expect(fatal(label != nullptr));
        expect(that % label->isAnnotated);
        expect(eq(label->documentation, std::string_view("")));
        expect(that % !label->visible);
    };

    "a plain member reports no annotation and is still a setting"_test = [] {
        const gr::settings::MemberDescriptor* count = memberNamed("count");
        expect(fatal(count != nullptr));
        expect(that % !count->isAnnotated);
        expect(that % !count->isEnum);
        expect(count->setParameter != nullptr);
    };

    "an enum member reports its type name and its value names"_test = [] {
        const gr::settings::MemberDescriptor* waveform = memberNamed("waveform");
        expect(fatal(waveform != nullptr));
        expect(that % waveform->isEnum);
        expect(fatal(waveform->enumTypeName != nullptr));
        expect(that % waveform->enumTypeName().contains("Waveform"));

        const std::vector<std::string_view> names(waveform->enumValueNames.begin(), waveform->enumValueNames.end());
        expect(eq(names.size(), 3UZ));
        expect(that % std::ranges::contains(names, std::string_view("sine")));
        expect(that % std::ranges::contains(names, std::string_view("square")));
        expect(that % std::ranges::contains(names, std::string_view("triangle")));
    };

    "meta_information repeats what the table says"_test = [] {
        const Annotations block = makeBlock();

        expect(eq(metaString(block, "gain::description"), std::string("gain")));
        expect(eq(metaString(block, "gain::documentation"), std::string("linear voltage gain")));
        expect(eq(metaString(block, "gain::unit"), std::string("dB")));
        expect(eq(metaString(block, "waveform::enum_type"), memberNamed("waveform")->enumTypeName()));
        expect(eq(metaString(block, "description"), std::string("a block carrying one member of every annotated shape")));

        const auto& metaInformation = block.meta_information.value;
        const auto  visible         = metaInformation.find(gr::convert_string_domain(std::string("gain::visible")));
        expect(fatal(visible != metaInformation.end()));
        expect(that % visible->second.value_or(false));

        expect(that % metaInformation.contains(gr::convert_string_domain(std::string("waveform::enum_values"))));
        expect(that % !metaInformation.contains(gr::convert_string_domain(std::string("count::unit")))) << "an unannotated member contributes nothing";
    };

    "the limits stay behind the typed accessor: an out-of-range value is refused, not applied"_test = [] {
        Annotations block = makeBlock();

        std::ignore       = block.settings().setStaged({{"gain", 3.0f}});
        const auto inside = block.settings().applyStagedParameters();
        expect(that % inside.failedParameters.empty());
        expect(eq(block.gain.value, 3.0f));

        std::ignore        = block.settings().setStaged({{"gain", 100.0f}});
        const auto outside = block.settings().applyStagedParameters();
        expect(that % outside.failedParameters.contains("gain")) << "a value beyond Limits<> must be reported as failed";
        expect(that % !outside.appliedParameters.contains("gain"));
        expect(that % !outside.forwardParameters.contains("gain")) << "a refused value must not reach downstream blocks";
        expect(eq(block.gain.value, 3.0f)) << "a refused value must leave the member alone";
    };

    "an enum setting round-trips through its name"_test = [] {
        Annotations block = makeBlock();

        std::ignore = block.settings().setStaged({{"waveform", std::string("square")}});
        std::ignore = block.settings().applyStagedParameters();
        expect(block.waveform.value == Waveform::square);

        const auto active = block.settings().get(std::array<std::string, 1>{"waveform"});
        expect(fatal(eq(active.size(), 1UZ)));
        expect(eq(std::string(active.begin()->second.value_or(std::string_view{})), std::string("square")));
    };
};

int main() { /* not needed by the UT framework */ }
