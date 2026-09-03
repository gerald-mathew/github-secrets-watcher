cmake_minimum_required(VERSION 3.10)
project(github_secrets_watcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(CURL REQUIRED)
find_package(Git2 REQUIRED)

add_executable(github_secrets_watcher
    src/main.cpp
    src/github.cpp
    src/scanner.cpp
    src/utils.cpp
)

target_include_directories(github_secrets_watcher PRIVATE src)

# Link libraries
target_link_libraries(github_secrets_watcher PRIVATE
    ${CURL_LIBRARIES}
    ${Git2_LIBRARIES}
)

# Include directories (if any from CURL and Git2)
target_include_directories(github_secrets_watcher PRIVATE
    ${CURL_INCLUDE_DIRS}
    ${Git2_INCLUDE_DIRS}
)
