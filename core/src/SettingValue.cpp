#include <gnuradio-4.0/SettingValue.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace gr::settings {

property_map heldValues(const std::expected<Values, Rejected>& read) {
    property_map values;
    if (read) {
        for (const auto& [key, entry] : *read) {
            if (entry.isValue() && entry.value.has_value()) {
                values.insert_or_assign(std::pmr::string(key), entry.value);
            }
        }
    }
    return values;
}

Rejected detail::failedKeys(const Values& values) {
    Rejected failed;
    for (const auto& [key, entry] : values) {
        if (entry.isFailed()) {
            failed.push_back(Rejection{.key = key, .reason = entry.reason});
        }
    }
    return failed;
}

} // namespace gr::settings
