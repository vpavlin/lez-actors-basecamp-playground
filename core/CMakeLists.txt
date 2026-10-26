cmake_minimum_required(VERSION 3.14)
project(LezActorsModulePlugin LANGUAGES CXX)
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET "${METADATA_JSON}" name)
find_package(OpenSSL REQUIRED)
# the example guests (examples/*.bin) become byte arrays in the library
set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/examples")
set(OUT "${CMAKE_CURRENT_BINARY_DIR}/lez_examples_data.cpp")
include("${CMAKE_CURRENT_SOURCE_DIR}/embed_examples.cmake")
# EXTERNAL_LIBS links core_ffi: LEZ dev's v0.4 wallet/transaction library (C ABI, core_ffi.h),
# built by the repo-root flake. It builds, signs and pays v0.4 transactions.
logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/lez_actors_module_impl.h
        src/lez_actors_module_impl.cpp
        src/lez_codec.hpp
        src/lez_http.hpp
        src/lez_logtap.hpp
        src/lez_examples.hpp
        src/lez_local_chain.cpp
        src/lez_bedrock.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/lez_examples_data.cpp
    INCLUDE_DIRS src
    FIND_PACKAGES OpenSSL
    LINK_LIBRARIES OpenSSL::Crypto
    EXTERNAL_LIBS
        core_ffi
)
