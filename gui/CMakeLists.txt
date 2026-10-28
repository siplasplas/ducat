add_executable(ducat-gui
    directory_model.cpp
    directory_model.hpp
    duplicates_dialog.cpp
    duplicates_dialog.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    plugins.cpp
    plugins.hpp
    search_dialog.cpp
    search_dialog.hpp
    usage_delegate.cpp
    usage_delegate.hpp
)

set_target_properties(ducat-gui PROPERTIES AUTOMOC ON)
target_link_libraries(ducat-gui PRIVATE ducat::libducat ducat_plugin_api Qt6::Widgets)

# Where the installed plugins are, relative to the installed ducat-gui.
file(RELATIVE_PATH plugin_dir_from_bin "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_PREFIX}/${DUCAT_PLUGIN_INSTALL_DIR}")
target_compile_definitions(ducat-gui PRIVATE
    DUCAT_PLUGIN_DIR_FROM_BIN="${plugin_dir_from_bin}")
target_compile_options(ducat-gui PRIVATE -Wall -Wextra -Wpedantic)

# The window icon, also when the icon is not installed (:/ducat.svg).
qt_add_resources(ducat-gui icons
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/res"
    FILES "${PROJECT_SOURCE_DIR}/res/ducat.svg"
)

install(TARGETS ducat-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
