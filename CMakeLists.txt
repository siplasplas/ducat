cmake_minimum_required(VERSION 3.20)

project(ducat
    VERSION 0.1.0
    DESCRIPTION "Fast disk usage catalog"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(BUILD_SHARED_LIBS "Build libducat as a shared library" OFF)
option(DUCAT_BUILD_TESTS "Build the unit tests" ON)

# Tkrzw stores the database.
include(cmake/Tkrzw.cmake)

# Qt is optional for now, detected for the planned GUI.
find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
    message(STATUS "Qt: ${Qt6_VERSION}")
else()
    message(STATUS "Qt: not found (install qt6-base-dev)")
endif()

add_subdirectory(libducat)
add_subdirectory(cli)

if(DUCAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
