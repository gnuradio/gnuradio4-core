#include <boost/ut.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gnuradio-4.0/SettingValue.hpp>

namespace qa_setting_value {

enum class Mode { automatic, manual };

[[nodiscard]] gr::settings::Value valueEntry(gr::pmt::Value value) { return gr::settings::Value{.state = gr::settings::Value::State::value, .value = std::move(value)}; }

[[nodiscard]] bool refusedAs(const gr::settings::Rejected& rejected) { return rejected.size() == 1UZ && rejected.front().key.empty() && !rejected.front().reason.empty(); }

} // namespace qa_setting_value

const boost::ut::suite<"setting value"> _settingValue = [] {
    using namespace boost::ut;
    using namespace qa_setting_value;
    using gr::settings::Value;

    "a read that narrows a double to a float is refused"_test = [] {
        const auto narrowed = valueEntry(0.1).as<float>();
        expect(fatal(!narrowed.has_value()));
        expect(refusedAs(narrowed.error())) << narrowed.error().front().reason;
        expect(eq(valueEntry(0.5).as<float>().value_or(0.f), 0.5f)) << "a double a float holds exactly reads as a float";
        expect(eq(valueEntry(0.1).as<double>().value_or(0.0), 0.1)) << "a double reads as a double";
    };

    "a whole double reads as an integer and a fraction does not"_test = [] {
        expect(eq(valueEntry(7.0).as<std::int32_t>().value_or(0), 7));
        expect(!valueEntry(7.5).as<std::int32_t>().has_value());
        expect(!valueEntry(std::numeric_limits<double>::infinity()).as<std::int64_t>().has_value());
        expect(!valueEntry(std::ldexp(1.0, 63)).as<std::int64_t>().has_value()) << "2^63 lies past int64";
        expect(!valueEntry(std::int64_t{300}).as<std::uint8_t>().has_value()) << "an integer outside the target's range";
    };

    "an integer reads as a floating-point number only when it holds it exactly"_test = [] {
        expect(eq(valueEntry(std::int32_t{7}).as<double>().value_or(0.0), 7.0));
        const std::int64_t pastDouble = (std::int64_t{1} << 53) + 1;
        expect(!valueEntry(pastDouble).as<double>().has_value());
        expect(!valueEntry(std::numeric_limits<std::int64_t>::max()).as<float>().has_value());
    };

    "a NaN reads as a NaN and a bool reads only as a bool"_test = [] {
        expect(std::isnan(valueEntry(std::numeric_limits<double>::quiet_NaN()).as<float>().value_or(0.f)));
        expect(valueEntry(true).as<bool>().value_or(false));
        expect(!valueEntry(true).as<int>().has_value());
        expect(!valueEntry(std::int32_t{1}).as<bool>().has_value());
    };

    "a complex value reads part by part"_test = [] {
        expect(valueEntry(std::complex<double>{1.5, -2.0}).as<std::complex<float>>().value_or(std::complex<float>{}) == std::complex<float>{1.5f, -2.0f});
        expect(!valueEntry(std::complex<double>{0.1, 0.0}).as<std::complex<float>>().has_value());
        expect(!valueEntry(1.0).as<std::complex<double>>().has_value());
    };

    "a list reads element by element without loss"_test = [] {
        const Value list = valueEntry(gr::Tensor<double>(gr::extents_from, {2UZ}));
        expect(eq(list.as<std::vector<float>>().value_or(std::vector<float>{9.f}).size(), 2UZ));
        expect(valueEntry(std::vector<double>{1.5, 2.0}).as<std::vector<float>>().value_or(std::vector<float>{}) == std::vector<float>{1.5f, 2.0f});
        expect(!valueEntry(std::vector<double>{1.5, 0.1}).as<std::vector<float>>().has_value());
        expect(valueEntry(std::vector<std::string>{"a", "b"}).as<std::vector<std::string>>().value_or(std::vector<std::string>{}) == std::vector<std::string>{"a", "b"});
        expect(!valueEntry(1.0).as<std::vector<double>>().has_value());
    };

    "a text reads as a text and as an enumerator's name"_test = [] {
        expect(eq(valueEntry("manual").as<std::string>().value_or(""), std::string("manual")));
        expect(valueEntry("manual").as<Mode>() == std::expected<Mode, gr::settings::Rejected>(Mode::manual));
        expect(!valueEntry("sideways").as<Mode>().has_value());
        expect(!valueEntry(1.0).as<std::string>().has_value());
    };

    "pending and failed entries read as refused with their reason"_test = [] {
        const auto pending = Value{.state = Value::State::pending, .reason = std::string(gr::settings::kPendingNotRunning)}.as<double>();
        expect(fatal(!pending.has_value()));
        expect(refusedAs(pending.error()));
        expect(eq(pending.error().front().reason, std::string("not running")));

        const auto failed = Value{.state = Value::State::failed, .reason = "no lock"}.as<double>();
        expect(fatal(!failed.has_value()));
        expect(refusedAs(failed.error()));
        expect(eq(failed.error().front().reason, std::string("no lock")));
        expect(!Value{.state = Value::State::value, .reason = "inner block 'gain' runs 2"}.as<double>().has_value()) << "a value entry with no value";
    };

    "the pending causes have their texts"_test = [] {
        expect(eq(gr::settings::kPendingNotRunning, std::string_view("not running")));
        expect(eq(gr::settings::kPendingTimeout, std::string_view("timeout")));
        expect(eq(gr::settings::kPendingOnWorker, std::string_view("applies at the block's next work call")));
    };
};
