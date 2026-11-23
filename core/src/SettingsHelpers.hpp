#ifndef GNURADIO_SETTINGS_HELPERS_HPP
#define GNURADIO_SETTINGS_HELPERS_HPP

#include <string>
#include <string_view>

#include <gnuradio-4.0/SettingValue.hpp>
#include <gnuradio-4.0/Value.hpp>

namespace gr::settings {

// values compare as stored, and a NaN equals itself, so a key holding NaN reads as in force as sent
[[nodiscard]] bool sameValue(const pmt::Value& left, const pmt::Value& right);

[[nodiscard]] Rejection invalidEntry(std::string key, std::string_view reason);

} // namespace gr::settings

#endif // GNURADIO_SETTINGS_HELPERS_HPP
