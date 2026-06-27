cmake_minimum_required(VERSION 3.20)
project(msquic_loadtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MSQUIC_ROOT "/home/administrator/msquic" CACHE PATH "Path to the local msquic checkout")
set(MSQUIC_LIB_DIR "${MSQUIC_ROOT}/build/bin/Release" CACHE PATH "Path to the local msquic library directory")
set(LSQUIC_ROOT "" CACHE PATH "Path to LSQUIC install prefix or source build")
set(BORINGSSL_ROOT "" CACHE PATH "Path to BoringSSL build/source root used for LSQUIC")
option(ENABLE_MSQUIC "Build the MSQuic backend when available" ON)
option(ENABLE_LSQUIC "Build the LSQUIC backend when available" ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(msquic-loadtest
    src/main.cpp
)

target_link_libraries(msquic-loadtest
    PRIVATE
        Threads::Threads
        ZLIB::ZLIB
)

set(USING_LSQUIC_TLS OFF)

if(ENABLE_LSQUIC)
    find_package(lsquic CONFIG QUIET
        HINTS
            "${LSQUIC_ROOT}"
            "${LSQUIC_ROOT}/lib/cmake"
            "${LSQUIC_ROOT}/share/lsquic"
    )
    find_path(LSQUIC_INCLUDE_DIR
        NAMES lsquic.h
        HINTS
            "${LSQUIC_ROOT}/include"
            "${LSQUIC_ROOT}/include/lsquic"
            "${LSQUIC_ROOT}"
        PATH_SUFFIXES lsquic
    )
    find_library(LSQUIC_LIBRARY
        NAMES lsquic
        HINTS
            "${LSQUIC_ROOT}/lib"
            "${LSQUIC_ROOT}/build/src/liblsquic"
            "${LSQUIC_ROOT}/src/liblsquic"
    )
    find_path(BORINGSSL_INCLUDE_DIR
        NAMES openssl/ssl.h
        HINTS
            "${BORINGSSL_ROOT}/include"
            "${BORINGSSL_ROOT}"
    )
    find_library(BORINGSSL_SSL_LIBRARY
        NAMES ssl
        HINTS
            "${BORINGSSL_ROOT}/ssl"
            "${BORINGSSL_ROOT}/build"
            "${BORINGSSL_ROOT}/build/ssl"
        NO_DEFAULT_PATH
    )
    find_library(BORINGSSL_CRYPTO_LIBRARY
        NAMES crypto
        HINTS
            "${BORINGSSL_ROOT}/crypto"
            "${BORINGSSL_ROOT}/build"
            "${BORINGSSL_ROOT}/build/crypto"
        NO_DEFAULT_PATH
    )

    if(lsquic_FOUND AND BORINGSSL_INCLUDE_DIR AND BORINGSSL_SSL_LIBRARY AND BORINGSSL_CRYPTO_LIBRARY)
        target_include_directories(msquic-loadtest PRIVATE "${BORINGSSL_INCLUDE_DIR}")
        target_link_libraries(msquic-loadtest PRIVATE
            lsquic::lsquic
            "${BORINGSSL_SSL_LIBRARY}"
            "${BORINGSSL_CRYPTO_LIBRARY}"
            ZLIB::ZLIB
            m
        )
        target_compile_definitions(msquic-loadtest PRIVATE HAVE_LSQUIC=1)
        set(USING_LSQUIC_TLS ON)
        message(STATUS "LSQUIC backend enabled via CMake package")
    elseif(lsquic_FOUND)
        message(WARNING "LSQUIC package found, but BoringSSL was not found. Set -DBORINGSSL_ROOT to the BoringSSL source/build root.")
    elseif(LSQUIC_INCLUDE_DIR AND LSQUIC_LIBRARY AND BORINGSSL_INCLUDE_DIR AND BORINGSSL_SSL_LIBRARY AND BORINGSSL_CRYPTO_LIBRARY)
        target_include_directories(msquic-loadtest PRIVATE "${BORINGSSL_INCLUDE_DIR}" "${LSQUIC_INCLUDE_DIR}")
        target_link_libraries(msquic-loadtest PRIVATE
            "${LSQUIC_LIBRARY}"
            "${BORINGSSL_SSL_LIBRARY}"
            "${BORINGSSL_CRYPTO_LIBRARY}"
            ZLIB::ZLIB
            m
        )
        target_compile_definitions(msquic-loadtest PRIVATE HAVE_LSQUIC=1)
        set(USING_LSQUIC_TLS ON)
        message(STATUS "LSQUIC backend enabled: ${LSQUIC_LIBRARY}")
    else()
        message(WARNING "LSQUIC not found. Set -DLSQUIC_ROOT and -DBORINGSSL_ROOT to enable --protocol=lsquic.")
    endif()
endif()

if(NOT USING_LSQUIC_TLS)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(msquic-loadtest PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(ENABLE_MSQUIC)
    find_path(MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        HINTS "${MSQUIC_ROOT}/src/inc"
    )
    find_library(MSQUIC_LIBRARY
        NAMES msquic libmsquic.so.2.6.0
        HINTS "${MSQUIC_LIB_DIR}"
    )
    if(MSQUIC_INCLUDE_DIR AND MSQUIC_LIBRARY)
        add_library(msquic SHARED IMPORTED GLOBAL)
        set_target_properties(msquic PROPERTIES
            IMPORTED_LOCATION "${MSQUIC_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
        )
        target_link_libraries(msquic-loadtest PRIVATE msquic)
        target_compile_definitions(msquic-loadtest PRIVATE HAVE_MSQUIC=1)
        set_target_properties(msquic-loadtest PROPERTIES
            BUILD_RPATH "${MSQUIC_LIB_DIR}"
            INSTALL_RPATH "${MSQUIC_LIB_DIR}"
        )
        message(STATUS "MSQuic backend enabled: ${MSQUIC_LIBRARY}")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LSQUIC QUIET lsquic)
        endif()
        if(LSQUIC_FOUND)
            message(WARNING "MSQuic not found; LSQUIC is installed but this fast branch has no LSQUIC transport adapter yet. Building SCTP-only.")
        else()
            message(WARNING "MSQuic not found and LSQUIC is not discoverable. Building SCTP-only.")
        endif()
    endif()
endif()

target_compile_options(msquic-loadtest PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
