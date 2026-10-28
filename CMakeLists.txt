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

# Installation: cmake --install build [--prefix DIR], by default to /usr/local.
include(GNUInstallDirs)
# Installed programs find an installed shared libducat next to them.
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

# Tkrzw stores the database.
include(cmake/Tkrzw.cmake)

# Qt 6 for the GUI, which is skipped when Qt is missing.
option(DUCAT_BUILD_GUI "Build the Qt GUI when Qt 6 is found" ON)
if(DUCAT_BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        message(STATUS "Qt: ${Qt6_VERSION}, building ducat-gui")
    else()
        message(STATUS "Qt: not found, skipping ducat-gui (install qt6-base-dev)")
    endif()
endif()

# Plugins of the GUI: installed to lib/ducat/plugins, built into
# build/plugins, where ducat-gui finds them when run from the build tree.
option(DUCAT_BUILD_PLUGINS "Build the GUI plugins; fetches their dependencies" ON)
set(DUCAT_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/ducat/plugins")
set(DUCAT_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/plugins")
add_library(ducat_plugin_api INTERFACE)
target_include_directories(ducat_plugin_api INTERFACE "${PROJECT_SOURCE_DIR}/plugin-api")

# Reading archives (libarchive-dev), for "ducat scan --archives".
option(DUCAT_ARCHIVES "Read archives with libarchive when it is found" ON)
if(DUCAT_ARCHIVES)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBARCHIVE QUIET IMPORTED_TARGET libarchive)
    endif()
    if(LIBARCHIVE_FOUND)
        message(STATUS "libarchive: ${LIBARCHIVE_VERSION}, reading archives")
    else()
        message(STATUS "libarchive: not found, archives are not read (install libarchive-dev)")
    endif()
endif()

add_subdirectory(libducat)
if(DUCAT_ARCHIVES AND LIBARCHIVE_FOUND)
    add_subdirectory(archive)
endif()
add_subdirectory(cli)
if(DUCAT_BUILD_GUI AND Qt6_FOUND)
    add_subdirectory(gui)
    if(DUCAT_BUILD_PLUGINS)
        add_subdirectory(plugins)
    endif()
    install(FILES res/ducat.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES res/ducat.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    # Refresh the icon and desktop file caches, so that a running desktop
    # (e.g. GNOME on Wayland, whose shell cannot be restarted without logging
    # out) shows the icon at once. Skipped for packaging (DESTDIR) and when
    # the tools are missing.
    install(CODE "
        if(NOT \"\$ENV{DESTDIR}\")
            set(data \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}\")
            find_program(gtk_update_icon_cache gtk-update-icon-cache)
            find_program(update_desktop_database update-desktop-database)
            if(gtk_update_icon_cache)
                message(STATUS \"Updating the icon cache of \${data}/icons/hicolor\")
                execute_process(COMMAND \"\${gtk_update_icon_cache}\" -f -t
                                \"\${data}/icons/hicolor\" OUTPUT_QUIET ERROR_QUIET)
            endif()
            if(update_desktop_database)
                execute_process(COMMAND \"\${update_desktop_database}\"
                                \"\${data}/applications\" OUTPUT_QUIET ERROR_QUIET)
            endif()
        endif()
    ")
endif()

if(DUCAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
