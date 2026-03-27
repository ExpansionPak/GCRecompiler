cmake_minimum_required(VERSION 3.15)
project(GCRecompiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Capstone
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "Disable all architectures by default" FORCE)
set(CAPSTONE_PPC_SUPPORT ON CACHE BOOL "Enable PPC support" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "Disable Capstone tests" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "Disable Capstone cstool" FORCE)
set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "Build static library" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "Don't build shared library" FORCE)

FetchContent_Declare(
    capstone
    GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
    GIT_TAG 5.0.1
)
FetchContent_MakeAvailable(capstone)

include_directories(include)

add_executable(gcrecomp 
    src/main.cpp 
    src/loader/dol.cpp
    src/analysis/disasm.cpp
    src/analysis/cfg.cpp
    src/analysis/analyzer.cpp
)

target_link_libraries(gcrecomp PRIVATE capstone)
