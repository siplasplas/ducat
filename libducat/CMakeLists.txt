configure_file(src/config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.hpp" @ONLY)

add_library(libducat
    src/archive.cpp
    src/database.cpp
    src/duplicates.cpp
    src/format.cpp
    src/path.cpp
    src/record.cpp
    src/scanner.cpp
    src/search.cpp
    src/spill.cpp
    src/version.cpp
)
add_library(ducat::libducat ALIAS libducat)

# The commit built, for --version and About; checked at every build.
set(build_info "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp")
add_custom_target(ducat_build_info
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${build_info}
            -P "${PROJECT_SOURCE_DIR}/cmake/build_info.cmake"
    BYPRODUCTS "${build_info}"
    COMMENT "Checking the Git commit"
    VERBATIM)
add_dependencies(libducat ducat_build_info)

set_target_properties(libducat PROPERTIES
    # Also linked into the GUI plugins, which are shared objects.
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME ducat
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(libducat
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(libducat PRIVATE ducat_tkrzw)

target_compile_options(libducat PRIVATE -Wall -Wextra -Wpedantic)

# A static libducat is linked into the programs; a shared one is needed at
# run time. Headers are not installed yet.
if(BUILD_SHARED_LIBS)
    install(TARGETS libducat LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
