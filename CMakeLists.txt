cmake_minimum_required(VERSION 3.24)

project(VectorTick
    VERSION 1.0.0
    DESCRIPTION "JIT-Compiled Market Data Analytics Engine"
    LANGUAGES CXX C ASM
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Position independent code for JIT
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Include cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Architecture)

# Platform detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(VT_ARCH_X86_64 ON)
    set(VT_ARCH_STRING "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(VT_ARCH_AARCH64 ON)
    set(VT_ARCH_STRING "aarch64")
else()
    message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    set(VT_ARCH_STRING "unknown")
endif()

# Compiler detection
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(VT_COMPILER_GCC ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(VT_COMPILER_CLANG ON)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(VT_COMPILER_APPLECLANG ON)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Common compiler flags
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fno-exceptions
    -fno-rtti
)

# Debug flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g3 -O0)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2 -g)
endif()

# Collect source files
file(GLOB_RECURSE VT_COMMON_SOURCES src/common/*.cpp)
file(GLOB_RECURSE VT_MEMORY_SOURCES src/memory/*.cpp)
file(GLOB_RECURSE VT_CONCURRENCY_SOURCES src/concurrency/*.cpp)
file(GLOB_RECURSE VT_PROTOCOL_SOURCES src/protocol/*.cpp)
file(GLOB_RECURSE VT_MODEL_SOURCES src/model/*.cpp)
file(GLOB_RECURSE VT_CODEC_SOURCES src/codec/*.cpp)
file(GLOB_RECURSE VT_STORAGE_SOURCES src/storage/*.cpp)
file(GLOB_RECURSE VT_QUERY_SOURCES src/query/*.cpp)
file(GLOB_RECURSE VT_IR_SOURCES src/ir/*.cpp)
file(GLOB_RECURSE VT_EXECUTION_SOURCES src/execution/*.cpp)
file(GLOB VT_JIT_SOURCES src/jit/*.cpp)
file(GLOB_RECURSE VT_REPLAY_SOURCES src/replay/*.cpp)
file(GLOB_RECURSE VT_METRICS_SOURCES src/metrics/*.cpp)

# Target compile options for narrow hardware features
if(VT_ARCH_X86_64)
    set_source_files_properties(src/common/crc32c_sse42.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2")
endif()

# JIT code generator sources (both backends are portable C++ bytecode emitters)
set(VT_JIT_ARCH_SOURCES)
file(GLOB_RECURSE VT_JIT_ARCH_SOURCES src/jit/x64/*.cpp src/jit/a64/*.cpp)

# Assembly sources
set(VT_ASM_SOURCES)
if(VT_ARCH_X86_64)
    file(GLOB VT_ASM_SOURCES asm/x86_64/*.S)
elseif(VT_ARCH_AARCH64)
    file(GLOB VT_ASM_SOURCES asm/aarch64/*.S)
endif()

# Main library
add_library(vectortick STATIC
    ${VT_COMMON_SOURCES}
    ${VT_MEMORY_SOURCES}
    ${VT_CONCURRENCY_SOURCES}
    ${VT_PROTOCOL_SOURCES}
    ${VT_MODEL_SOURCES}
    ${VT_CODEC_SOURCES}
    ${VT_STORAGE_SOURCES}
    ${VT_QUERY_SOURCES}
    ${VT_IR_SOURCES}
    ${VT_EXECUTION_SOURCES}
    ${VT_JIT_SOURCES}
    ${VT_JIT_ARCH_SOURCES}
    ${VT_REPLAY_SOURCES}
    ${VT_METRICS_SOURCES}
    ${VT_ASM_SOURCES}
)

target_include_directories(vectortick
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(vectortick PUBLIC cxx_std_20)
set_warning_flags(vectortick)
set_sanitizer_flags(vectortick)

# Link libraries
find_package(Threads REQUIRED)
target_link_libraries(vectortick PUBLIC Threads::Threads)

# Platform-specific libraries
if(APPLE)
    target_link_libraries(vectortick PRIVATE dl)
elseif(UNIX)
    target_link_libraries(vectortick PRIVATE dl rt)
endif()

# Applications helper
function(add_vt_executable name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE vectortick)
    set_warning_flags(${name})
    set_sanitizer_flags(${name})
endfunction()

add_vt_executable(vectortick_ingest apps/vectortick_ingest.cpp)
add_vt_executable(vectortick_query apps/vectortick_query.cpp)
add_vt_executable(vectortick_replay apps/vectortick_replay.cpp)
add_vt_executable(vectortick_inspect apps/vectortick_inspect.cpp)
add_vt_executable(vectortick_demo apps/vectortick_demo.cpp)

# Tests
enable_testing()

file(GLOB_RECURSE VT_TEST_SOURCES tests/*.cpp)
add_executable(vectortick_tests ${VT_TEST_SOURCES})
target_link_libraries(vectortick_tests PRIVATE vectortick)
target_include_directories(vectortick_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set_warning_flags(vectortick_tests)
set_sanitizer_flags(vectortick_tests)

set(VT_TEST_SUITES
    types_tests
    checked_arithmetic_tests
    protocol_tests
    pcap_tests
    storage_tests
    query_lexer_tests
    query_parser_tests
    query_typecheck_tests
    ir_tests
    reference_executor_tests
    vector_executor_tests
    x86_encoder_tests
    aarch64_encoder_tests
    jit_differential_tests
    recovery_tests
    cli_integration_tests
    fuzz_smoke_tests
)

foreach(SUITE ${VT_TEST_SUITES})
    add_test(NAME ${SUITE} COMMAND vectortick_tests --suite=${SUITE})
endforeach()
add_test(NAME all_tests COMMAND vectortick_tests)

# Benchmarks
file(GLOB_RECURSE VT_BENCH_SOURCES bench/*.cpp)
add_executable(vectortick_bench ${VT_BENCH_SOURCES})
target_link_libraries(vectortick_bench PRIVATE vectortick)
set_warning_flags(vectortick_bench)
set_sanitizer_flags(vectortick_bench)

# Fuzz targets
option(VT_ENABLE_FUZZING "Build fuzz targets with libFuzzer" OFF)
if(VT_ENABLE_FUZZING AND (VT_COMPILER_CLANG OR VT_COMPILER_APPLECLANG))
    file(GLOB VT_FUZZ_SOURCES fuzz/*.cpp)
    foreach(FUZZ_SOURCE ${VT_FUZZ_SOURCES})
        get_filename_component(FUZZ_NAME ${FUZZ_SOURCE} NAME_WE)
        add_executable(${FUZZ_NAME} ${FUZZ_SOURCE})
        target_link_libraries(${FUZZ_NAME} PRIVATE vectortick)
        target_compile_options(${FUZZ_NAME} PRIVATE -fsanitize=fuzzer)
        target_link_options(${FUZZ_NAME} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

# Install
install(TARGETS vectortick
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/vectortick
    DESTINATION include
)
