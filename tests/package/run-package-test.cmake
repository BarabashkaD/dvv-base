# Installs the build tree into a scratch prefix and configures, builds and runs the consumer twice:
# against the installed package and with dvv_base in-tree (FetchContent of the source directory).
# A third configure asks the installed package for version 0.5 and must fail: the version file
# uses SameMinorVersion, so a 0.6 package does not satisfy a 0.5 request.
# Called by the dvv_base.package test with -DBUILD_DIR, -DCONFIG, -DPREFIX, -DSOURCE_DIR,
# -DCONSUMER_SOURCE, -DCONSUMER_BUILD, -DCONSUMER_TREE_BUILD (the in-tree consumer; a short name
# keeps its deep paths short on Windows), -DGENERATOR, -DPLATFORM, -DCXX_COMPILER,
# -DDEPS_PREFIX_PATH and -DDVV_BUILD_CMAKE_DIR (dvv-build's cmake folder, which the in-tree
# dvv_base needs).
cmake_minimum_required(VERSION 4.2)

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "failed (${status}): ${ARGV}")
    endif()
endfunction()

# run_fails(<pattern> <command>...): the command must fail and its output must match the pattern.
function(run_fails pattern)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE output
                    ERROR_VARIABLE output)
    if(status EQUAL 0)
        message(FATAL_ERROR "succeeded, expected a failure: ${ARGN}")
    endif()
    if(NOT output MATCHES "${pattern}")
        message(FATAL_ERROR "the output does not match '${pattern}':\n${output}")
    endif()
endfunction()

# consumer_configure(<out-var> <build dir> <prefix path> [<extra configure arguments>...])
# Writes the initial-cache file and sets <out-var> to the configure command.
function(consumer_configure out_var build_dir prefix_path)
    # A list value cannot pass through a command line intact; an initial-cache file keeps it whole.
    set(initial_cache "${build_dir}-cache.cmake")
    file(WRITE "${initial_cache}" "set(CMAKE_PREFIX_PATH \"${prefix_path}\" CACHE STRING \"\")\n")
    set(configure "${CMAKE_COMMAND}" -C "${initial_cache}" -S "${CONSUMER_SOURCE}" -B "${build_dir}"
        -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
        ${ARGN})
    if(PLATFORM)
        list(APPEND configure -A "${PLATFORM}")
    endif()
    set(${out_var} "${configure}" PARENT_SCOPE)
endfunction()

# consumer(<build dir> <prefix path> [<extra configure arguments>...])
function(consumer build_dir prefix_path)
    consumer_configure(configure "${build_dir}" "${prefix_path}" ${ARGN})
    run(${configure})
    run("${CMAKE_COMMAND}" --build "${build_dir}" --config "${CONFIG}")

    file(GLOB_RECURSE executable "${build_dir}/consumer" "${build_dir}/consumer.exe")
    list(FILTER executable EXCLUDE REGEX "CMakeFiles")
    if(NOT executable)
        message(FATAL_ERROR "consumer executable not found in ${build_dir}")
    endif()
    list(GET executable 0 executable)
    run("${executable}")
endfunction()

if(NOT DVV_BUILD_CMAKE_DIR)
    message(FATAL_ERROR "pass -DDVV_BUILD_CMAKE_DIR=<dvv-build>/cmake")
endif()
string(REPLACE "," ";" deps_prefix_path "${DEPS_PREFIX_PATH}")
file(REMOVE_RECURSE "${PREFIX}" "${CONSUMER_BUILD}" "${CONSUMER_BUILD}-0.5"
            "${CONSUMER_TREE_BUILD}")
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${PREFIX}")
consumer("${CONSUMER_BUILD}" "${PREFIX};${deps_prefix_path}")
consumer("${CONSUMER_TREE_BUILD}" "${deps_prefix_path}" "-DDVV_BASE_SOURCE_DIR=${SOURCE_DIR}"
         "-DDVV_BUILD_CMAKE_DIR=${DVV_BUILD_CMAKE_DIR}")
consumer_configure(configure "${CONSUMER_BUILD}-0.5" "${PREFIX};${deps_prefix_path}"
                   -DDVV_BASE_REQUEST=0.5)
# CMake words a missing package differently from a rejected version: only the latter names the
# requested version as incompatible.
run_fails("compatible with requested version \"0\\.5\"" ${configure})
