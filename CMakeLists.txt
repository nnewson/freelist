cmake_minimum_required(VERSION 3.13)

project(freelist)

# Find GTest
enable_testing()
find_package(GTest REQUIRED)

add_executable(freelistTest include/freelist.h tests/unittests.cpp tests/performancetests.cpp)
include_directories(include)
target_link_libraries(freelistTest GTest::GTest GTest::Main)

gtest_discover_tests(freelistTest)

find_package(Boost 1.65 REQUIRED COMPONENTS system)
target_link_libraries(freelistTest Boost::system)

# Use C++17, without compiler specific extensions
target_compile_features(freelistTest PUBLIC cxx_std_17)
set_target_properties(freelistTest PROPERTIES CXX_EXTENSIONS OFF)
