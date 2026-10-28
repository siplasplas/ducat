# File dialogs from qt-extra (QxFileDialog).
ducat_add_plugin(ducat-filedialog file_dialog_plugin.cpp)
target_link_libraries(ducat-filedialog PRIVATE qt-extra)
target_compile_options(ducat-filedialog PRIVATE -Wall -Wextra -Wpedantic)
