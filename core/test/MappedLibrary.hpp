#ifndef CORE_TEST_MAPPED_LIBRARY_HPP
#define CORE_TEST_MAPPED_LIBRARY_HPP

#include <string>

#include <dlfcn.h>

namespace gr::testing {

// whether the process holds the shared object at path mapped. The check maps nothing: it takes a reference only to a
// mapped object and drops it again.
[[nodiscard]] inline bool isMapped(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
}

} // namespace gr::testing

#endif // CORE_TEST_MAPPED_LIBRARY_HPP
