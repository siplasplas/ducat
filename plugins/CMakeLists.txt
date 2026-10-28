# Plugins of ducat-gui. Each one keeps its dependencies, fetched from GitHub,
# out of the main program, which works without any of them.
include(FetchContent)

# Libraries built here go into plugins (shared objects), and fetched projects
# are added without their install rules: ducat installs only the plugins.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# qt-extra (github.com/siplasplas/qt-extra, LGPL 2.1): only its sources are
# fetched, built here as the static library "qt-extra", the name the fetched
# qceditor looks for, without qt-extra's install rules.
FetchContent_Declare(ducat_qt_extra
    GIT_REPOSITORY https://github.com/siplasplas/qt-extra.git
    GIT_TAG v2.7.0
    GIT_SHALLOW TRUE
    # No CMakeLists.txt there: the sources are fetched but not added.
    SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(ducat_qt_extra)
set(qt_extra_dir "${ducat_qt_extra_SOURCE_DIR}")
file(GLOB qt_extra_sources CONFIGURE_DEPENDS
    "${qt_extra_dir}/common/*.cpp" "${qt_extra_dir}/common/*.h"
    "${qt_extra_dir}/widgets/*.cpp" "${qt_extra_dir}/widgets/*.h")
add_library(qt-extra STATIC ${qt_extra_sources})
set_target_properties(qt-extra PROPERTIES AUTOMOC ON)
target_include_directories(qt-extra PUBLIC
    "$<BUILD_INTERFACE:${qt_extra_dir}/common>"
    "$<BUILD_INTERFACE:${qt_extra_dir}/widgets>")
target_link_libraries(qt-extra PUBLIC Qt6::Widgets)
# qceditor's export needs qt-extra in an export set; this one is excluded
# from installing.
install(TARGETS qt-extra EXPORT ducat-unused ARCHIVE DESTINATION unused EXCLUDE_FROM_ALL)
install(EXPORT ducat-unused DESTINATION unused EXCLUDE_FROM_ALL)

# A plugin module: built into build/plugins, installed to lib/ducat/plugins.
function(ducat_add_plugin target)
    add_library(${target} MODULE ${ARGN})
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${DUCAT_PLUGIN_BUILD_DIR}"
        INSTALL_RPATH "")
    target_link_libraries(${target} PRIVATE ducat_plugin_api Qt6::Widgets)
    install(TARGETS ${target} LIBRARY DESTINATION ${DUCAT_PLUGIN_INSTALL_DIR})
endfunction()

add_subdirectory(filedialog)
add_subdirectory(viewer)
