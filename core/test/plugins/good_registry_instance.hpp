#ifndef GR_TEST_GOOD_REGISTRY_INSTANCE_HPP
#define GR_TEST_GOOD_REGISTRY_INSTANCE_HPP

#include <gnuradio-4.0/Plugin.hpp>

// the registry the generated registration units insert into, declared by the plugin rather than by the framework
gr::plugin<>& grPluginInstance();

#endif
