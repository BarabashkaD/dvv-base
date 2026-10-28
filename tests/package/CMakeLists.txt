# Consumer of dvv_base; built by run-package-test.cmake. With DVV_BASE_SOURCE_DIR it takes dvv_base
# in-tree through FetchContent (as a dependent library or the app takes it; then DVV_BUILD_CMAKE_DIR
# names dvv-build's cmake folder), otherwise the installed package.
cmake_minimum_required(VERSION 4.2)
project(dvv_base_package_consumer LANGUAGES CXX)

if(DVV_BASE_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(dvv_base SOURCE_DIR "${DVV_BASE_SOURCE_DIR}")
    FetchContent_MakeAvailable(dvv_base)
else()
    find_package(dvv_base 0.1 REQUIRED)
endif()
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")
# Sibling repositories run the shared compile-fail runner from the same directory.
if(NOT EXISTS "${DVV_CMAKE_DIR}/run-compile-fail.cmake")
    message(FATAL_ERROR "run-compile-fail.cmake is missing from ${DVV_CMAKE_DIR}")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dvv::base)
dvv_set_compiler_options(consumer)
