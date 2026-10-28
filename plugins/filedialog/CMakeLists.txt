# File dialogs from qt-extra (github.com/siplasplas/qt-extra, LGPL 2.1).
# Only its sources are fetched, so that its own install rules stay out of
# ducat's installation; the dialog's sources are built into the plugin.
include(FetchContent)
FetchContent_Declare(ducat_qt_extra
    GIT_REPOSITORY https://github.com/siplasplas/qt-extra.git
    GIT_TAG v2.7.0
    GIT_SHALLOW TRUE
    # No CMakeLists.txt there: the sources are fetched but not added.
    SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(ducat_qt_extra)
set(qt_extra_widgets "${ducat_qt_extra_SOURCE_DIR}/widgets")

add_library(ducat-filedialog MODULE
    file_dialog_plugin.cpp
    "${qt_extra_widgets}/qxbreadcrumb.cpp"
    "${qt_extra_widgets}/qxbreadcrumb.h"
    "${qt_extra_widgets}/qxfilebreadcrumb.cpp"
    "${qt_extra_widgets}/qxfilebreadcrumb.h"
    "${qt_extra_widgets}/qxfiledialog.cpp"
    "${qt_extra_widgets}/qxfiledialog.h"
    "${qt_extra_widgets}/qxfilemetadata.cpp"
    "${qt_extra_widgets}/qxfilemetadata.h"
)
set_target_properties(ducat-filedialog PROPERTIES
    AUTOMOC ON
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${DUCAT_PLUGIN_BUILD_DIR}"
    INSTALL_RPATH ""
)
target_include_directories(ducat-filedialog PRIVATE "${qt_extra_widgets}")
target_link_libraries(ducat-filedialog PRIVATE ducat_plugin_api Qt6::Widgets)
# Warnings for ducat's own code only.
set_source_files_properties(file_dialog_plugin.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

install(TARGETS ducat-filedialog LIBRARY DESTINATION ${DUCAT_PLUGIN_INSTALL_DIR})
