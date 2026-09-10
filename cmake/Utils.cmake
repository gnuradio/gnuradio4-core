

macro(CheckPythonAvailability)
    # check for CPython and Numpy dependencies
    set(PYTHON_FORCE_INCLUDE OFF)
    if(PYTHON_FORCE_INCLUDE)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter Development NumPy)
    else()
    find_package(Python3 3.12 COMPONENTS Interpreter Development NumPy)
    endif()

    set(PYTHON_AVAILABLE OFF)
    if(Python3_FOUND AND NOT EMSCRIPTEN)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNumPy.py"
        RESULT_VARIABLE NUMPY_NOT_FOUND
        OUTPUT_VARIABLE NUMPY_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    # If NumPy is found, set PYTHON_AVAILABLE to ON
    if(NOT NUMPY_NOT_FOUND)
        set(PYTHON_AVAILABLE ON)
        message(STATUS "Using Python Include Dirs: ${Python3_INCLUDE_DIRS} and ${NUMPY_INCLUDE_DIR}")
    else()
        message(STATUS "Python and Numpy Include headers not found!!")
    endif()
    endif()
endmacro()


macro(SetupMagicEnumTarget)
    # include header-only libraries that have been inlined to simplify builds w/o requiring access to the internet
    add_library(magic_enum INTERFACE)
    add_library(gnuradio4::magic_enum ALIAS magic_enum)
    target_include_directories(
        magic_enum ${CMAKE_EXT_DEP_WARNING_GUARD} INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/magic_enum/>
                                                            $<INSTALL_INTERFACE:include>)

    set(magic_enum_public_headers third_party/magic_enum/magic_enum.hpp third_party/magic_enum/magic_enum_utility.hpp)
    set_target_properties(magic_enum PROPERTIES PUBLIC_HEADER "${magic_enum_public_headers}")
    install(
        TARGETS magic_enum
        EXPORT gnuradio4Targets
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endmacro()


macro(SetupCoverageSupport)
    message("Coverage reporting enabled")
    include(cmake/CodeCoverage.cmake) # https://github.com/bilke/cmake-modules/blob/master/CodeCoverage.cmake #
    # (License: BSL-1.0)
    target_compile_options(
        gnuradio-options
        INTERFACE --coverage
                -Og
                -g1
                -gz
        # -gdwarf-2 -gstrict-dwarf -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
    ) # fortify_source is not possible without optimization
    target_link_libraries(gnuradio-options INTERFACE --coverage)
    append_coverage_compiler_flags()
    set(GCOVR_ADDITIONAL_ARGS "--merge-mode-functions=merge-use-line-min"
                                "--gcov-ignore-parse-errors=negative_hits.warn_once_per_file")
    set(GR4_CORE_COVERAGE_DEPENDENCIES
        qa_buffer
        qa_AtomicBitset
        qa_AtomicRef
        qa_Value
        qa_ValueHelper
        qa_ComputeDomain
        qa_DataSet
        qa_DynamicPort
        qa_LifeCycle
        qa_MemoryAllocators
        qa_PmtCollections
        qa_Port
        qa_Profiler
        qa_Tensor
        qa_TensorMath
        qa_SVD
        qa_TriggerMatcher
        qa_YamlPmt
        qa_PortableTypeName
        qa_thread_affinity
        qa_thread_pool
        qa_SubGraphAssets
        qa_fixed_string
        qa_reflection
        qa_formatter
        qa_traits
        qa_type_name
        qa_UncertainValue
        qa_RangesHelper)
    setup_target_for_coverage_gcovr_xml(
        NAME
        coverage
        EXECUTABLE
        ctest
        EXECUTABLE_ARGS
        "--output-on-failure"
        DEPENDENCIES
        ${GR4_CORE_COVERAGE_DEPENDENCIES}
        EXCLUDE
        "${CMAKE_BINARY_DIR}/*")
    setup_target_for_coverage_gcovr_html(
        NAME
        coverage_html
        EXECUTABLE
        ctest
        EXECUTABLE_ARGS
        "--output-on-failure"
        DEPENDENCIES
        ${GR4_CORE_COVERAGE_DEPENDENCIES}
        EXCLUDE
        "${CMAKE_BINARY_DIR}/*")
endmacro()


function(SetupEmscriptenGnuRadioOptions)
  # Make sure consumers of gnuradio4::gnuradio-options don't accidentally build with conflicting arguments
  target_compile_definitions(gnuradio-options INTERFACE GR_MAX_WASM_THREAD_COUNT=${GR_MAX_WASM_THREAD_COUNT})
  target_compile_options(gnuradio-options INTERFACE -fexceptions -pthread)
  target_link_options(
    gnuradio-options
    INTERFACE
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s ASSERTIONS=1"
    "SHELL:-s INITIAL_MEMORY=256MB"
    "SHELL:-s STACK_SIZE=4194304"
    # "SHELL:-s SAFE_HEAP=1" # additional for debug "SHELL:-s ASSERTIONS=2" # additional for debug "SHELL:-s
    # STACK_OVERFLOW_CHECK=2" # additional for debug "SHELL:-g" # additional for debug "SHELL:-gsource-map" # additional
    # for debug "SHELL:--profiling-funcs" # additional for debug "SHELL:--emit-symbol-map" # additional for debug
    -fexceptions
    -pthread
    "SHELL:-s PTHREAD_POOL_SIZE=${GR_MAX_WASM_THREAD_COUNT}"
    "SHELL:-s PTHREAD_POOL_SIZE_STRICT=2"
    "SHELL:-s FETCH=1"
    "SHELL:-s WASM=1" # output as web-assembly
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall']"
    "SHELL:-s ENVIRONMENT=web,worker,node")
    set(CMAKE_EXECUTABLE_SUFFIX ".js" PARENT_SCOPE)
endfunction()


function(ReportConfigurationState)
    # print effective options
    message(STATUS "================================================")
    message(STATUS "GR-4 build configuration")
    message(STATUS "------------------------------------------------")
    message(STATUS "GR_TOPLEVEL_PROJECT          : ${GR_TOPLEVEL_PROJECT}")
    message(STATUS "GR_ENABLE_BLOCK_REGISTRY     : ${GR_ENABLE_BLOCK_REGISTRY}")
    message(STATUS "INTERNAL_ENABLE_BLOCK_PLUGINS: ${INTERNAL_ENABLE_BLOCK_PLUGINS}")
    message(STATUS "USE_CCACHE                   : ${USE_CCACHE}")
    message(STATUS "WARNINGS_AS_ERRORS           : ${WARNINGS_AS_ERRORS}")
    message(STATUS "TIMETRACE                    : ${TIMETRACE}")
    message(STATUS "ADDRESS_SANITIZER            : ${ADDRESS_SANITIZER}")
    message(STATUS "UB_SANITIZER                 : ${UB_SANITIZER}")
    message(STATUS "THREAD_SANITIZER             : ${THREAD_SANITIZER}")
    message(STATUS "ENABLE_TBB                   : ${ENABLE_TBB}")
    message(STATUS "ENABLE_EXAMPLES              : ${ENABLE_EXAMPLES}")
    message(STATUS "ENABLE_TESTING               : ${ENABLE_TESTING}")
    message(STATUS "GR_PACKAGING                 : ${GR_PACKAGING}")
    message(STATUS "ENABLE_COVERAGE              : ${ENABLE_COVERAGE}")
    message(STATUS "GR_MAX_WASM_THREAD_COUNT     : ${GR_MAX_WASM_THREAD_COUNT}")
    message(STATUS "------------------------------------------------")

    string(TOUPPER "${CMAKE_BUILD_TYPE}" _BT)
    set(_config_cxx_flags "${CMAKE_CXX_FLAGS_${_BT}}")
    set(_config_link_flags "${CMAKE_EXE_LINKER_FLAGS_${_BT}}")
    set(_base_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(_base_link_flags "${CMAKE_EXE_LINKER_FLAGS}")
    get_directory_property(_dir_compile_opts COMPILE_OPTIONS)
    get_directory_property(_dir_link_opts LINK_OPTIONS)
    get_target_property(_t_copts gnuradio-options INTERFACE_COMPILE_OPTIONS)
    get_target_property(_t_lopts gnuradio-options INTERFACE_LINK_OPTIONS)
    if(_t_lopts STREQUAL "_t_lopts-NOTFOUND")
    unset(_t_lopts)
    endif()

    string(
    JOIN
    " "
    _final_compile_opts
    ${_base_cxx_flags}
    ${_config_cxx_flags}
    ${_dir_compile_opts}
    ${_t_copts})
    string(
    JOIN
    " "
    _final_link_opts
    ${_base_link_flags}
    ${_config_link_flags}
    ${_dir_link_opts}
    ${_t_lopts})

    message(STATUS "Effective compiler flags : ${_final_compile_opts}")
    message(STATUS "Effective linker flags   : ${_final_link_opts}")
    message(STATUS "------------------------------------------------")
    message(STATUS "Toolchain details")
    message(STATUS "------------------------------------------------")
    message(STATUS "C++ compiler ID          : ${CMAKE_CXX_COMPILER_ID}")
    message(STATUS "GR_USE_ADAPTIVE_CPP?     : ${GR_USE_ADAPTIVE_CPP}")
    message(STATUS "C++ compiler path        : ${CMAKE_CXX_COMPILER}")
    message(STATUS "C++ compiler version     : ${CMAKE_CXX_COMPILER_VERSION}")
    string(
    REGEX MATCH
            "-fuse-ld=([a-zA-Z0-9._-]+)"
            _linker_backend
            "${_final_link_opts}")
    if(_linker_backend MATCHES "-fuse-ld=([a-zA-Z0-9._-]+)")
    set(_detected_linker "${CMAKE_CXX_COMPILER} with ${CMAKE_MATCH_1}")
    else()
    set(_detected_linker "${CMAKE_LINKER} (default/provided linker)")
    endif()
    message(STATUS "Linker                   : ${_detected_linker}")

    if(CCACHE_PROGRAM)
    execute_process(
        COMMAND ${CCACHE_PROGRAM} --version
        OUTPUT_VARIABLE CCACHE_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(
        REGEX MATCH
            "[^\n]*"
            CCACHE_VERSION_LINE
            "${CCACHE_VERSION}")
    message(STATUS "ccache                   : ${CCACHE_VERSION_LINE}")
    else()
    message(STATUS "ccache                   : not found or disabled")
    endif()

    message(STATUS "================================================")

endfunction()
