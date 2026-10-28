# The F3 viewer, as in gemini-commander: small files in qceditor's editor
# (github.com/siplasplas/qceditor) on qcodeedit's widgets
# (github.com/siplasplas/qcodeedit, LGPL 3) with Kate syntax colours and
# encoding detection, large files in textviewers' TextViewer
# (github.com/ideorg/textviewers), and a hex view.
#
# qceditor needs all of qcodeedit's parts: Kate syntax data downloaded at run
# time, encodings (cpg, fetched by qcodeedit), spelling (hunspell,
# libhunspell-dev) and diff; qceditor itself needs Botan (libbotan-2-dev).
set(QCE_BUILD_DEMO OFF)
set(QCE_BUILD_TESTS OFF)
set(QCE_BUILD_KATE ON)
set(QCE_BUILD_KATE_DOWNLOADER ON)
set(QCE_BUILD_ENCODING ON)
set(QCE_BUILD_GRAMMAR ON)
set(QCE_BUILD_SPELL ON)
set(QCE_BUILD_SPELL_DOWNLOADER ON)
set(QCE_BUILD_DIFF ON)
# An installed static cpg may lack position-independent code, which the
# plugin needs: qcodeedit then fetches cpg and builds it here.
set(CMAKE_DISABLE_FIND_PACKAGE_cpg TRUE)
FetchContent_Declare(qcodeedit
    GIT_REPOSITORY https://github.com/siplasplas/qcodeedit.git
    GIT_TAG v1.66.4
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
set(QCEDITOR_BUILD_APP OFF)
FetchContent_Declare(qceditor
    GIT_REPOSITORY https://github.com/siplasplas/qceditor.git
    GIT_TAG v1.25.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
# textviewers' own CMakeLists.txt builds demos: only its sources are fetched,
# without the cpg submodule, which these sources do not use.
FetchContent_Declare(ducat_textviewers
    GIT_REPOSITORY https://github.com/ideorg/textviewers.git
    GIT_TAG v1.2
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(qcodeedit qceditor ducat_textviewers)
set(textviewers_dir "${ducat_textviewers_SOURCE_DIR}")

find_package(Qt6 REQUIRED COMPONENTS Concurrent)
find_package(ICU REQUIRED COMPONENTS uc i18n)

ducat_add_plugin(ducat-viewer
    hex_view_widget.cpp
    hex_view_widget.hpp
    viewer_plugin.cpp
    viewer_window.cpp
    viewer_window.hpp
    "${textviewers_dir}/wid/TextViewer.cpp"
    "${textviewers_dir}/wid/TextViewer.h"
    "${textviewers_dir}/wid/PaintArea.cpp"
    "${textviewers_dir}/wid/PaintArea.h"
    "${textviewers_dir}/wid/FindBar.cpp"
    "${textviewers_dir}/wid/FindBar.h"
    "${textviewers_dir}/logic/AbstractView.cpp"
    "${textviewers_dir}/logic/Wrap.cpp"
    "${textviewers_dir}/logic/LineView.cpp"
    "${textviewers_dir}/logic/ByteView.cpp"
    "${textviewers_dir}/logic/LineDeque.cpp"
    "${textviewers_dir}/logic/ByteDeque.cpp"
    "${textviewers_dir}/logic/ByteDocument.cpp"
    "${textviewers_dir}/logic/ChangeableDocument.cpp"
    "${textviewers_dir}/logic/LineIndexedDocument.cpp"
    "${textviewers_dir}/logic/Searcher.cpp"
    "${textviewers_dir}/misc/selection.cpp"
)
# SYSTEM: its headers' warnings do not show in ducat's own files.
target_include_directories(ducat-viewer SYSTEM PRIVATE "${textviewers_dir}")
target_link_libraries(ducat-viewer PRIVATE
    qceditor::editor
    qcodeedit::qcodeedit
    qcodeedit::kate
    qcodeedit::katedata
    Qt6::Concurrent
    ICU::uc ICU::i18n
)
# Warnings for ducat's own code only.
set_source_files_properties(viewer_plugin.cpp viewer_window.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

# Encoding detection reads cpg's languages.txt and n-gram models at run time.
# qcodeedit compiles in their place in the build tree; the installed plugin
# gets a copy in share/ducat/cpg, found relative to the installed ducat-gui.
set(cpg_data_install_dir "${CMAKE_INSTALL_DATADIR}/ducat/cpg")
install(FILES "${CPG_LANGUAGES_FILE}" DESTINATION "${cpg_data_install_dir}")
install(DIRECTORY "${CPG_MODELS_DIR}/" DESTINATION "${cpg_data_install_dir}/models")
file(RELATIVE_PATH cpg_data_from_bin "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_PREFIX}/${cpg_data_install_dir}")
target_compile_definitions(ducat-viewer PRIVATE
    DUCAT_CPG_DATA_FROM_BIN="${cpg_data_from_bin}")
target_link_libraries(ducat-viewer PRIVATE qcodeedit::encoding)
