cmake_minimum_required(VERSION 3.10.0)
project(Pakker VERSION 0.1.0 LANGUAGES CXX C)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h" "src/*.c")

option(BUILD_EXAMPLES "Build examples" ON)
option(PAK_NO_MMAP "Disable memory-mapped I/O" OFF)
option(BUILD_BENCHMARKS "Build benchmark executable" ON)
option(PAK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PAK_ENABLE_LTO "Enable interprocedural optimization (LTO) for Release-style builds" OFF)

find_package(Threads REQUIRED)

add_library(${PROJECT_NAME} STATIC ${SOURCES})
add_library(Pakker::Pakker ALIAS ${PROJECT_NAME})

# Propagate the C++20 requirement to consumers so find_package(Pakker)
# users don't silently compile Pak.h against an older default standard.
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

# Warnings are scoped to CXX only so the vendored LZ4 C source isn't subjected
# to -Wpedantic/-Werror noise from a dependency we don't maintain.
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wextra>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wpedantic>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${PAK_WARNINGS_AS_ERRORS}>>:$<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>>
)

if(PAK_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT PAK_IPO_SUPPORTED OUTPUT PAK_IPO_ERROR)
    if(PAK_IPO_SUPPORTED)
        set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "PAK_ENABLE_LTO requested but IPO/LTO is not supported: ${PAK_IPO_ERROR}")
    endif()
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
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

# Only export/install when Pakker is the top-level project -- consumers
# using add_subdirectory()/FetchContent shouldn't have their own install
# layout polluted by Pakker's install rules.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    include(CMakePackageConfigHelpers)

    install(TARGETS ${PROJECT_NAME}
        EXPORT PakkerTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES src/Pak.h src/PakPlatform.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/PakkerConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PakkerConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PakkerConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pakker
    )
    install(EXPORT PakkerTargets
        FILE PakkerTargets.cmake
        NAMESPACE Pakker::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pakker
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/PakkerConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/PakkerConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pakker
    )
endif()
