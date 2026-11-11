# Plugins of ducat-gui. Each one keeps its dependencies, fetched from GitHub,
# out of the main program, which works without any of them.
include(FetchContent)

# Libraries built here go into plugins (shared objects), and fetched projects
# are added without their install rules: ducat installs only the plugins.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# qt-extra comes from the main CMakeLists.txt, position-independent.

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

add_subdirectory(viewer)
# Archives: spp-archive of siplasplas-plugins (see the main CMakeLists.txt).
