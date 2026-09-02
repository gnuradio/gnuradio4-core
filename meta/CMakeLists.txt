add_library(gnuradio-meta INTERFACE)
add_library(gnuradio4::gnuradio-meta ALIAS gnuradio-meta)
target_include_directories(gnuradio-meta INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
                                                   $<INSTALL_INTERFACE:include/>)
target_link_libraries(gnuradio-meta INTERFACE gnuradio4::gnuradio-options gnuradio4::vir)

set(meta_public_headers
    include/gnuradio-4.0/meta/CacheLineSize.hpp
    include/gnuradio-4.0/meta/RangesHelper.hpp
    include/gnuradio-4.0/meta/UnitTestHelper.hpp
    include/gnuradio-4.0/meta/formatter.hpp
    include/gnuradio-4.0/meta/immutable.hpp
    include/gnuradio-4.0/meta/indirect.hpp
    include/gnuradio-4.0/meta/reflection.hpp
    include/gnuradio-4.0/meta/typelist.hpp
    include/gnuradio-4.0/meta/utils.hpp
    include/gnuradio-4.0/meta/UncertainValue.hpp)

# Overrides the value gr::kCacheLine resolves to; empty leaves the platform chain in CacheLineSize.hpp to decide. It is
# carried on the interface because that value is the alignment, and so the layout, of public types such as gr::Tag and
# gr::Sequence: a consumer built against a different one disagrees with the library about them, where nothing warns.
set(GR_CACHE_LINE_SIZE
    ""
    CACHE STRING "Cache line size gr::kCacheLine resolves to, in bytes (empty: detect from the platform)")
if(GR_CACHE_LINE_SIZE)
  target_compile_definitions(gnuradio-meta INTERFACE GR_CACHE_LINE_SIZE=${GR_CACHE_LINE_SIZE})
endif()

set_target_properties(gnuradio-meta PROPERTIES PUBLIC_HEADER "${meta_public_headers}")
install(
  TARGETS gnuradio-meta
  EXPORT gnuradio4Targets
  PUBLIC_HEADER DESTINATION include/gnuradio-4.0/meta)

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
