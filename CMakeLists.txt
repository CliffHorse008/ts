cmake_minimum_required(VERSION 3.16)

project(minimal_hls_muxer C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

add_library(hls_muxer STATIC
    src/hls_muxer.c
)

target_include_directories(hls_muxer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hls_muxer PRIVATE -O2 -Wall -Wextra)
endif()

add_executable(demo
    examples/demo_main.c
)

target_link_libraries(demo
    PRIVATE
        hls_muxer
)

add_executable(offline_mux
    examples/offline_mux_main.c
)

target_link_libraries(offline_mux
    PRIVATE
        hls_muxer
)

add_executable(hls_muxer_integration_test
    tests/hls_muxer_integration_test.c
)

target_link_libraries(hls_muxer_integration_test
    PRIVATE
        hls_muxer
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hls_muxer_integration_test PRIVATE -O2 -Wall -Wextra)
endif()

add_test(
    NAME hls_muxer_integration
    COMMAND hls_muxer_integration_test
            ${CMAKE_CURRENT_BINARY_DIR}/hls_muxer_test_output
)
