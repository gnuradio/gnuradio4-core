#ifndef GNURADIO_PLUGIN_METADATA_HPP
#define GNURADIO_PLUGIN_METADATA_HPP

#include <string>

#include <gnuradio-4.0/Export.hpp>

#ifdef _MSC_VER
#pragma warning(push)
// C4251: std::string has no dll-interface. The plugin ABI deliberately passes std::string across the
// boundary, which is safe only because host and plugins are built with the same toolchain and CRT --
// the same constraint the rest of the plugin ABI already relies on.
#pragma warning(disable : 4251)
#endif

struct GNURADIO_EXPORT gr_plugin_metadata {
    std::string plugin_name;
    std::string plugin_author;
    std::string plugin_license;
    std::string plugin_version;
    std::string block_type = {};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // GNURADIO_PLUGIN_METADATA_HPP
