cmake_minimum_required(VERSION 3.10.0)
project(Pakker VERSION 0.1.0)

enable_language(C)
include(CTest)

set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h" "src/*.c")

option(BUILD_EXAMPLES "Build examples" ON)
option(PAK_NO_MMAP "Disable memory-mapped I/O" OFF)
option(BUILD_BENCHMARKS "Build benchmark executable" ON)

find_package(Threads REQUIRED)

add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g>
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)

if(PAK_NO_MMAP)
    target_compile_definitions(${PROJECT_NAME} PUBLIC PAK_NO_MMAP)
endif()

if(BUILD_EXAMPLES)
# Example executable
file(GLOB EXAMPLE_SOURCES "example/*.cpp" "example/*.h")
add_executable(PakkerExample ${EXAMPLE_SOURCES})
target_link_libraries(PakkerExample PRIVATE ${PROJECT_NAME})
endif()

if(BUILD_TESTING)
add_executable(PakkerTests tests/PakRuntimeTests.cpp)
target_link_libraries(PakkerTests PRIVATE ${PROJECT_NAME})
add_test(NAME PakkerRuntime COMMAND PakkerTests)
endif()

if(BUILD_BENCHMARKS)
add_executable(PakkerBenchmark benchmarks/PakRuntimeBenchmark.cpp)
target_link_libraries(PakkerBenchmark PRIVATE ${PROJECT_NAME})
endif()
