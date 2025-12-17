cmake_minimum_required(VERSION 3.22)

project(TestAES67Receiver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable ravennakit extras (we only need the library).
set(RAV_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAV_TESTS OFF CACHE BOOL "" FORCE)
set(RAV_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(RAV_TOOLS OFF CACHE BOOL "" FORCE)

add_subdirectory(external/ravennakit)
add_subdirectory(apps/aes67_rx_cli)


