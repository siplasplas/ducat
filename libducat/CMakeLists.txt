configure_file(src/config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.hpp" @ONLY)

add_library(libducat
    src/record.cpp
    src/scanner.cpp
    src/version.cpp
)
add_library(ducat::libducat ALIAS libducat)

set_target_properties(libducat PROPERTIES
    OUTPUT_NAME ducat
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(libducat
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_options(libducat PRIVATE -Wall -Wextra -Wpedantic)
