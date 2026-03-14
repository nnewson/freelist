cmake_minimum_required(VERSION 3.20)

project(free_list
  VERSION 0.1.0
  DESCRIPTION "Free list in C++"
  LANGUAGES CXX
)

find_package(GTest CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)

# Header-only library target for free_list
add_library(freelist INTERFACE)
target_include_directories(freelist INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(freelist INTERFACE cxx_std_23)
# Suppress the "interference size" warning, as we use a hardware defined cache-line.
# This causes a warning as it can differ at the ABI level, but this queue ISN'T designed
# to be used across different ABIs, so we can safely ignore this warning.
target_compile_options(freelist INTERFACE -Wno-interference-size -mcpu=native)

# Tests
enable_testing()
add_executable(test_freelist tests/test_freelist.cpp)
target_link_libraries(test_freelist PRIVATE freelist GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(test_freelist)

add_executable(perf_test_freelist tests/perf_test_freelist.cpp)
target_link_libraries(perf_test_freelist PRIVATE freelist Boost::headers GTest::gtest_main)
gtest_discover_tests(perf_test_freelist)