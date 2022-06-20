cmake_minimum_required(VERSION 3.4 FATAL_ERROR)

project(TelegramRecorderTests VERSION 1.0 LANGUAGES CXX)

include_directories(BEFORE ../)

find_package(spdlog REQUIRED)
find_library(fmt fmt)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/e2239ee6043f73722e7aa812a459f54a28552929.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(tgrec_test lru_test.cpp hash_test.cpp ../hash.cpp)
set_property(TARGET tgrec_test PROPERTY CXX_STANDARD 17)
include(GoogleTest)
gtest_discover_tests(tgrec_test)
target_link_libraries(tgrec_test PRIVATE crypto gtest gmock gtest_main fmt spdlog)
