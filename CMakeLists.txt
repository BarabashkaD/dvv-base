cmake_minimum_required(VERSION 4.2)
project(dvv_base VERSION 0.1.0 DESCRIPTION "dvv base library" LANGUAGES CXX)
# Version string; a pre-release build adds its release-stage label (user decision U-M4ORDER).
set(dvv_base_VERSION_STRING "${PROJECT_VERSION}")

# dvv-build provides the shared CMake modules and the dependency check (dvv-deps.txt); the dvv
# presets set DVV_BUILD_CMAKE_DIR to its cmake folder.
if(NOT DVV_BUILD_CMAKE_DIR OR NOT IS_ABSOLUTE "${DVV_BUILD_CMAKE_DIR}"
   OR NOT EXISTS "${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
    message(FATAL_ERROR "dvv_base: DVV_BUILD_CMAKE_DIR is not set, not absolute or "
                        "has no DvvDeps.cmake (it must be dvv-build's cmake folder); configure "
                        "with a dvv-build preset (e.g. `cmake --preset vs2026`, see README.md) "
                        "or pass -DDVV_BUILD_CMAKE_DIR=<absolute path of dvv-build>/cmake")
endif()
include("${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
dvv_dependencies()

# Higher repositories include the shared modules as "${DVV_CMAKE_DIR}/<module>.cmake": in-tree from
# dvv-build through this cache entry, installed through dvv_baseConfig.cmake (copies next to it).
set(DVV_CMAKE_DIR "${DVV_BUILD_CMAKE_DIR}" CACHE INTERNAL "Shared dvv CMake modules")
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(DVV_BUILD_TESTS "Build the dvv tests" ${PROJECT_IS_TOP_LEVEL})

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.12 REQUIRED CONFIG)

add_library(dvv_base STATIC
    src/environment.cpp
    src/error.cpp
    src/executors.cpp
    src/json.cpp
    src/leak_check.cpp
    src/lifetime.cpp
    src/log.cpp
    src/paths.cpp
    src/uri.cpp)
add_library(dvv::base ALIAS dvv_base)
set_target_properties(dvv_base PROPERTIES EXPORT_NAME base)
target_include_directories(dvv_base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dvv_base PUBLIC Threads::Threads nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(dvv_base PRIVATE shell32 ole32)
endif()
dvv_set_compiler_options(dvv_base)

install(TARGETS dvv_base EXPORT dvv_baseTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/dvv_base)
install(EXPORT dvv_baseTargets NAMESPACE dvv:: DESTINATION ${config_dir})
configure_package_config_file(cmake/dvv_baseConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_baseConfig.cmake INSTALL_DESTINATION ${config_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/dvv_baseConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_baseConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_baseConfigVersion.cmake
    ${DVV_BUILD_CMAKE_DIR}/DvvCompilerOptions.cmake
    ${DVV_BUILD_CMAKE_DIR}/run-compile-fail.cmake
    DESTINATION ${config_dir})

if(DVV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
