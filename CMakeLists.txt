cmake_minimum_required(VERSION 3.25)

project(
    cpp_l4_proxy
    VERSION 0.1.0
    LANGUAGES CXX
)

add_executable(proxy
    src/main.cpp
    src/socket.cpp
    src/shutdown.cpp
    src/backend.cpp
    src/proxy.cpp
    src/server.cpp
)

add_executable(proxy_kqueue
    src/kqueue_main.cpp
    src/socket.cpp
    src/shutdown.cpp
    src/kqueue_server.cpp
)

add_executable(backend_server
    src/backend_main.cpp
)

add_executable(load_generator
    src/load_generator.cpp
)

target_include_directories(
    proxy
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(
    proxy_kqueue
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(
    proxy
    PRIVATE
        cxx_std_23
)

target_compile_features(
    backend_server
    PRIVATE
        cxx_std_23
)

target_compile_features(
    load_generator
    PRIVATE
        cxx_std_23
)

set_target_properties(
    proxy
    backend_server
    load_generator
    PROPERTIES
        CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(proxy PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(backend_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(load_generator PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()