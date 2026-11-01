foreach(name IN ITEMS varint scanner record path database search duplicates)
    add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE ducat::libducat)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Writes foreign Tkrzw files to check that they are refused.
target_link_libraries(test_database PRIVATE ducat_tkrzw)

# Merging folders, from the GUI's file operations, when the GUI is built.
if(TARGET ducat-gui)
    add_executable(test_merge test_merge.cpp "${PROJECT_SOURCE_DIR}/gui/file_operations.cpp")
    target_include_directories(test_merge PRIVATE "${PROJECT_SOURCE_DIR}/gui")
    target_link_libraries(test_merge PRIVATE Qt6::Widgets)
    target_compile_options(test_merge PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME merge COMMAND test_merge)
    set_tests_properties(merge PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Reading archives, when built; the test writes its archives with libarchive.
if(TARGET ducat::archive)
    add_executable(test_archive test_archive.cpp)
    target_link_libraries(test_archive PRIVATE ducat::archive PkgConfig::LIBARCHIVE)
    target_compile_options(test_archive PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME archive COMMAND test_archive)
endif()
