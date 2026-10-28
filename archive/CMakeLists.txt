# ducat-archive: reading archives with libarchive (libarchive-dev), for the
# programs that store their contents; libducat itself does not need it.
add_library(ducat-archive STATIC src/archive_reader.cpp)
add_library(ducat::archive ALIAS ducat-archive)
# Also linked into the GUI plugins, which are shared objects.
set_target_properties(ducat-archive PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ducat-archive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ducat-archive PUBLIC ducat::libducat PRIVATE PkgConfig::LIBARCHIVE)
target_compile_options(ducat-archive PRIVATE -Wall -Wextra -Wpedantic)
