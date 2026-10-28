foreach(name IN ITEMS varint scanner record path database search duplicates)
    add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE ducat::libducat)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Writes foreign Tkrzw files to check that they are refused.
target_link_libraries(test_database PRIVATE ducat_tkrzw)

# Reading archives, when built; the test writes its archives with libarchive.
if(TARGET ducat::archive)
    add_executable(test_archive test_archive.cpp)
    target_link_libraries(test_archive PRIVATE ducat::archive PkgConfig::LIBARCHIVE)
    target_compile_options(test_archive PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME archive COMMAND test_archive)
endif()
