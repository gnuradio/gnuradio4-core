#ifndef GNURADIO_TOOLS_INSTALLEDLOCATION_HPP
#define GNURADIO_TOOLS_INSTALLEDLOCATION_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// An installed tool finds its install tree from where it runs, so a moved or installer-placed tree still finds its own
// plugins. The build hands over the program directory relative to the prefix, GR_TOOLS_BIN_TO_PREFIX, and the plugin
// directory relative to the prefix, or absolute when it lies outside it. GR_TOOLS_INSTALLED_PREFIX, the configured
// prefix, stands in where the platform cannot name the running program, as under Emscripten. A program that links
// gnuradio-rungraph resolves these from its own location, which is the same tree when it installs into the GR 4 prefix.
namespace gr::tools {

[[nodiscard]] inline std::filesystem::path runningProgramDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2UZ);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(buffer.find('\0'));
    std::error_code             error;
    const std::filesystem::path program = std::filesystem::canonical(buffer, error);
    return error ? std::filesystem::path{} : program.parent_path();
#elif defined(__linux__)
    std::error_code             error;
    const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : program.parent_path();
#else
    return {};
#endif
}

[[nodiscard]] inline std::string installedPrefix() {
    const std::filesystem::path programDirectory = runningProgramDirectory();
    if (programDirectory.empty()) {
        return GR_TOOLS_INSTALLED_PREFIX;
    }
    const std::filesystem::path prefix = programDirectory / GR_TOOLS_BIN_TO_PREFIX;
    std::error_code             error;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(prefix, error);
    return (error ? prefix.lexically_normal() : resolved).generic_string();
}

[[nodiscard]] inline std::string installedPluginDirectory() { return (std::filesystem::path(installedPrefix()) / GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY).lexically_normal().generic_string(); }

} // namespace gr::tools

#endif // GNURADIO_TOOLS_INSTALLEDLOCATION_HPP
