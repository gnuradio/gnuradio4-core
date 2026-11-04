#ifndef GNURADIO_TOOLS_TEST_ENVIRONMENT_HPP
#define GNURADIO_TOOLS_TEST_ENVIRONMENT_HPP

#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

// A path the build hands the run through the test's environment. Without it the test has nothing to open, so it stops
// and names the variable.
[[nodiscard]] inline std::string fromEnvironment(const char* variable) {
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::format("the environment variable {} is not set or empty; run the test through ctest", variable));
    }
    return value;
}

// a file of the directory of graph definitions the build names
[[nodiscard]] inline std::string assetPath(std::string_view name) { return std::format("{}/{}", fromEnvironment("GR_TOOLS_TEST_ASSETS"), name); }

#endif // GNURADIO_TOOLS_TEST_ENVIRONMENT_HPP
