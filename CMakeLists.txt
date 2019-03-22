cmake_minimum_required(VERSION 3.13)

# Find GTest
enable_testing()
find_package(GTest REQUIRED)

add_executable(freelist include/freelist.h tests/unittests.cpp tests/performancetests.cpp)
include_directories(include)
target_link_libraries(freelist GTest::GTest GTest::Main)

gtest_discover_tests(freelist)

find_package(Boost 1.65 REQUIRED COMPONENTS system)
target_link_libraries(freelist Boost::system)


# Use C++17, without compiler specific extensions
target_compile_features(freelist PUBLIC cxx_std_17)
set_target_properties(freelist PROPERTIES CXX_EXTENSIONS OFF)