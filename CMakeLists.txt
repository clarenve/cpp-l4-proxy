cmake_minimum_required(VERSION 3.25)

project(
    cpp_l4_proxy
    VERSION 0.1.0
    LANGUAGES CXX
)

add_executable(proxy
    src/main.cpp
)

target_compile_features(proxy PRIVATE cxx_std_23)

set_target_properties(proxy PROPERTIES
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(proxy PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()