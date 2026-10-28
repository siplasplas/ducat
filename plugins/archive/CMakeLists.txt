# Reading archives in the GUI: Enter on an archive on disk, and scans with
# archives. Built on ducat-archive, so only when libarchive was found.
ducat_add_plugin(ducat-archive-plugin archive_plugin.cpp)
target_link_libraries(ducat-archive-plugin PRIVATE ducat::archive)
target_compile_options(ducat-archive-plugin PRIVATE -Wall -Wextra -Wpedantic)
