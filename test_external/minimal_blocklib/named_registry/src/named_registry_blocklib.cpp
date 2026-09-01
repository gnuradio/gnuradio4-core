#include <named_registry_blocklib/Registry.hpp>
#include <named_registry_blocklib/Scale.hpp>

gr::BlockRegistry& namedBlockRegistry() {
    static gr::BlockRegistry instance;
    return instance;
}
