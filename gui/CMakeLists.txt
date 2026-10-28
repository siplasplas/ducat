add_executable(ducat-gui
    directory_model.cpp
    directory_model.hpp
    duplicates_dialog.cpp
    duplicates_dialog.hpp
    entry_operations.cpp
    entry_operations.hpp
    file_operations.cpp
    file_operations.hpp
    gemini.cpp
    gemini.hpp
    list_views.cpp
    list_views.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    plugins.cpp
    plugins.hpp
    scope_selector.cpp
    scope_selector.hpp
    search_dialog.cpp
    search_dialog.hpp
    usage_delegate.cpp
    usage_delegate.hpp
)

set_target_properties(ducat-gui PROPERTIES AUTOMOC ON)
target_link_libraries(ducat-gui PRIVATE ducat::libducat ducat_plugin_api Qt6::Widgets)

# Opening a folder in gemini-commander over D-Bus. On Wayland an
# xdg-activation token brings its window to the front; Qt offers it only
# through a private interface (qt6-base-private-dev), so without that the
# window may stay behind.
find_package(Qt6 REQUIRED COMPONENTS DBus)
set(QT_NO_PRIVATE_MODULE_WARNING ON) # used knowingly, for one interface
find_package(Qt6 QUIET COMPONENTS GuiPrivate)
target_link_libraries(ducat-gui PRIVATE Qt6::DBus)
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(ducat-gui PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(ducat-gui PRIVATE DUCAT_HAVE_XDG_ACTIVATION)
else()
    message(STATUS "Qt GuiPrivate not found: no xdg-activation token for gemini-commander "
                   "(install qt6-base-private-dev)")
endif()

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
