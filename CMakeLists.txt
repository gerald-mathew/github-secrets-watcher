cmake_minimum_required(VERSION 3.16)
project(github_secrets_watcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages via pkg-config (libgit2 ships no CMake config module)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(GIT2 REQUIRED IMPORTED_TARGET libgit2)

# Core library shared between the CLI and the tests
add_library(gsw_core STATIC
    src/github.cpp
    src/scanner.cpp
    src/utils.cpp
)
target_include_directories(gsw_core PUBLIC src)
target_link_libraries(gsw_core PUBLIC
    PkgConfig::CURL
    PkgConfig::GIT2
)

add_executable(github_secrets_watcher src/main.cpp)
target_link_libraries(github_secrets_watcher PRIVATE gsw_core)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 REQUIRED)
    add_executable(unit_tests
        tests/utils.test.cpp
        tests/scanner.test.cpp
    )
    target_link_libraries(unit_tests PRIVATE gsw_core Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(unit_tests)
endif()