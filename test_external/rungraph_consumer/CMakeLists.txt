cmake_minimum_required(VERSION 3.28)

project(gr4_rungraph_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(gnuradio4 CONFIG REQUIRED)

# a graph-file runner over the installed gnuradio4::gnuradio-rungraph, with core's test source and sink compiled in
add_executable(rungraph_consumer src/consumer.cpp)
target_include_directories(rungraph_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/test/plugins)
target_link_libraries(rungraph_consumer PRIVATE gnuradio4::gnuradio-rungraph)

# under Emscripten the runner reads the graph file from the host's file system
if(EMSCRIPTEN)
  target_link_options(rungraph_consumer PRIVATE "-sNODERAWFS=1")
endif()
