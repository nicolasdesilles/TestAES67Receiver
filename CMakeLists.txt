cmake_minimum_required(VERSION 3.22)

project(TestAES67Receiver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Low-power SBCs commonly OOM while compiling heavy translation units.
# This option reduces per-file compiler memory usage (at the cost of a bit of runtime perf).
option(LOW_MEM_BUILD "Reduce compiler memory usage (recommended on small SBCs)" ON)
if (LOW_MEM_BUILD)
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif ()

# Disable ravennakit extras (we only need the library).
set(RAV_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAV_TESTS OFF CACHE BOOL "" FORCE)
set(RAV_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(RAV_TOOLS OFF CACHE BOOL "" FORCE)

# Building on small ARM SBCs frequently OOMs with -flto (cc1plus killed).
# Allow enabling it explicitly, but keep it OFF by default here.
set(RAV_ENABLE_LTO OFF CACHE BOOL "" FORCE)

add_subdirectory(external/ravennakit)
add_subdirectory(apps/aes67_rx_cli)


