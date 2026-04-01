cmake_minimum_required(VERSION 3.15)
project(GCRecompiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Capstone was originally used, but proved to be too weird. 

add_library(gcrecomp_lib
    src/loader/dol.cpp
    src/analysis/disasm.cpp
    src/analysis/cfg.cpp
    src/analysis/analyzer.cpp
    src/analysis/lifter.cpp
    src/analysis/optimizer.cpp
    src/codegen/emitter.cpp
)

target_include_directories(gcrecomp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(gcrecomp src/main.cpp)
target_link_libraries(gcrecomp PRIVATE gcrecomp_lib)

enable_testing()

add_executable(gcrecomp_tests tests/test_main.cpp)
target_link_libraries(gcrecomp_tests PRIVATE gcrecomp_lib)

add_test(NAME gcrecomp_tests COMMAND gcrecomp_tests)
