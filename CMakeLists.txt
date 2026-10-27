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

# Optional dependencies, detected now and required by later stages:
# Tkrzw for the database, Qt for the GUI.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(TKRZW QUIET IMPORTED_TARGET tkrzw)
endif()
if(TKRZW_FOUND)
    message(STATUS "Tkrzw: ${TKRZW_VERSION}")
else()
    message(STATUS "Tkrzw: not found (install libtkrzw-dev)")
endif()

find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
    message(STATUS "Qt: ${Qt6_VERSION}")
else()
    message(STATUS "Qt: not found (install qt6-base-dev)")
endif()

add_subdirectory(libducat)
add_subdirectory(cli)
