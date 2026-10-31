/// A shared object that is neither a plugin nor a block library: it exports no `gr_plugin_make` and registers nothing
/// when it loads, like the libraries a plugin links against.
extern "C" int ordinary_library_value();

extern "C" int ordinary_library_value() { return 1; }
