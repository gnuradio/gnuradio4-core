#include <boost/ut.hpp>

#include <gnuradio-4.0/Settings.hpp>
#include <gnuradio-4.0/meta/reflection.hpp>

#include <cstddef>
#include <cstdint>

namespace qa_settings {

struct SizeSettingsTarget {
    struct ResamplingControl {
        static constexpr bool kEnabled = false;
    };

    gr::property_map meta_information{};
    std::size_t      item_count{1UZ};

    GR_MAKE_REFLECTABLE(SizeSettingsTarget, item_count);
};

const boost::ut::suite<"Settings"> settingsTests = [] {
    using namespace boost::ut;

    "portable size value is converted when applied"_test = [] {
        SizeSettingsTarget target;
        gr::CtxSettings    settings(target);
        settings.init();

        gr::property_map update;
        update.emplace("item_count", std::int64_t{42});

        expect(settings.setStaged(update).empty());
        const auto result = settings.applyStagedParameters();

        expect(eq(target.item_count, std::size_t{42}));
        expect(result.appliedParameters.contains("item_count"));
    };
};

} // namespace qa_settings

int main() { /* tests are statically executed */ }
