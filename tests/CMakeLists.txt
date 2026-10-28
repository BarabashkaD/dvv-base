find_package(doctest 2.5 REQUIRED CONFIG)

add_executable(dvv_base_tests
    main.cpp
    test_environment.cpp
    test_error.cpp
    test_executors.cpp
    test_json.cpp
    test_lifetime.cpp
    test_log.cpp
    test_paths.cpp
    test_task.cpp
    test_uri.cpp
    test_when_all.cpp)
target_link_libraries(dvv_base_tests PRIVATE dvv::base doctest::doctest)
# doctest otherwise only forward-declares std::ostream, which breaks printing std types.
target_compile_definitions(dvv_base_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_base_tests)
add_test(NAME dvv_base.unit COMMAND dvv_base_tests)
# The timeout turns a hang (a lost wake-up, a deadlock) into a failure long before the CI job limit.
set_tests_properties(dvv_base.unit PROPERTIES
    TIMEOUT 300
    # DVV_BASE_TEST_INHERITED checks that a value set by the parent process reads as UTF-8.
    ENVIRONMENT "DVV_LEAK_CHECK=fail;DVV_BASE_TEST_INHERITED=Grüße-日本-🎞"
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")
# Cycle labels: `sanity` marks the fast inner-loop subset (ctest --preset vs2026-sanity); `package`
# marks install and consumer tests, which only the full cycle runs (vs2026-small excludes them).
set_property(TEST dvv_base.unit APPEND PROPERTY LABELS sanity)

# Package test (spec §3.1): install, then build a consumer with find_package(dvv_base) and another
# with dvv_base in-tree; both include DvvCompilerOptions through DVV_CMAKE_DIR.
# Skipped for sanitizer builds, whose consumers would need the same runtime flags.
if(NOT DVV_SANITIZE)
    string(REPLACE ";" "," deps_prefix_path "${CMAKE_PREFIX_PATH}")
    add_test(NAME dvv_base.package
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${PROJECT_BINARY_DIR}
                     -DCONFIG=$<CONFIG>
                     -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-prefix
                     -DSOURCE_DIR=${dvv_base_SOURCE_DIR}
                     -DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/package
                     -DCONSUMER_BUILD=${CMAKE_CURRENT_BINARY_DIR}/package-consumer
                     -DCONSUMER_TREE_BUILD=${CMAKE_CURRENT_BINARY_DIR}/pkg-tree
                     -DGENERATOR=${CMAKE_GENERATOR}
                     -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
                     -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DDEPS_PREFIX_PATH=${deps_prefix_path}
                     -DDVV_BUILD_CMAKE_DIR=${DVV_BUILD_CMAKE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/package/run-package-test.cmake)
    set_tests_properties(dvv_base.package PROPERTIES
        RESOURCE_LOCK dvv_build_tree
        LABELS package)
endif()

# Compile-fail tests (spec §9.4). Case 0 is the control and must build; cases 1-3 must fail to
# compile with the expected diagnostic, so a broken library or a typo cannot pass them.
set(compile_fail_source ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/direct_construction.cpp)
add_executable(dvv_base_compile_ok ${compile_fail_source})
target_compile_definitions(dvv_base_compile_ok PRIVATE DVV_COMPILE_FAIL_CASE=0)
target_link_libraries(dvv_base_compile_ok PRIVATE dvv::base)
dvv_set_compiler_options(dvv_base_compile_ok)
if(MSVC)
    set(compile_fail_patterns "C2248" "C2512" "C2280")
else()
    set(compile_fail_patterns "private constructor" "no matching (function|constructor)" "deleted")
endif()
foreach(case IN ITEMS 1 2 3)
    set(target dvv_base_compile_fail_${case})
    add_executable(${target} EXCLUDE_FROM_ALL ${compile_fail_source})
    target_compile_definitions(${target} PRIVATE DVV_COMPILE_FAIL_CASE=${case})
    target_link_libraries(${target} PRIVATE dvv::base)
    dvv_set_compiler_options(${target})
    math(EXPR pattern_index "${case} - 1")
    list(GET compile_fail_patterns ${pattern_index} pattern)
    # cmake --build needs the top-level tree; in a dependent build (another dvv library or the app)
    # dvv-base is a subproject.
    add_test(NAME dvv_base.compile_fail.${case}
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -DCONFIG=$<CONFIG>
                     -DTARGET=${target}
                     "-DPATTERN=${pattern}"
                     -P ${DVV_CMAKE_DIR}/run-compile-fail.cmake)
    # These tests and the package tests of every dvv repository drive the same build tree in a
    # dependent build with DVV_TEST_DEPENDENCIES, so they share one lock name.
    set_tests_properties(dvv_base.compile_fail.${case} PROPERTIES RESOURCE_LOCK dvv_build_tree)
endforeach()
