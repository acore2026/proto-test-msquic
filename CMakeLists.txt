cmake_minimum_required(VERSION 3.20)
project(msquic_loadtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MSQUIC_ROOT "/home/administrator/msquic" CACHE PATH "Path to the local msquic checkout")
set(MSQUIC_INCLUDE_DIR "${MSQUIC_ROOT}/src/inc")
set(MSQUIC_LIB_DIR "${MSQUIC_ROOT}/build/bin/Release")

find_library(MSQUIC_LIBRARY
    NAMES msquic libmsquic.so.2.6.0
    HINTS "${MSQUIC_LIB_DIR}"
    REQUIRED
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(msquic SHARED IMPORTED GLOBAL)
set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION "${MSQUIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
)

add_executable(msquic-loadtest
    src/main.cpp
)

target_link_libraries(msquic-loadtest
    PRIVATE
        msquic
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

target_compile_options(msquic-loadtest PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(msquic-loadtest PROPERTIES
    BUILD_RPATH "${MSQUIC_LIB_DIR}"
    INSTALL_RPATH "${MSQUIC_LIB_DIR}"
)
