#ifndef CORE_TEST_MAPPED_LIBRARY_HPP
#define CORE_TEST_MAPPED_LIBRARY_HPP

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gr::testing {

// whether the process holds the shared object at path mapped. The check maps nothing: it takes a reference only to a
// mapped object and drops it again.
[[nodiscard]] inline bool isMapped(const std::string& path) {
#if defined(_WIN32)
    // reports an already-loaded module without taking a reference, so nothing is released here
    return GetModuleHandleA(path.c_str()) != nullptr;
#else
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
#endif
}

} // namespace gr::testing

#endif // CORE_TEST_MAPPED_LIBRARY_HPP
