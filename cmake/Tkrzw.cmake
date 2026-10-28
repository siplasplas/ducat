# Provides the ducat_tkrzw target for the Tkrzw database library.
#
# By default Tkrzw is downloaded and built from source as a static library
# inside the build directory. Tkrzw 1.0.32 as packaged in Ubuntu cannot remove
# records (Remove reports success but stores its REMOVE marker as the value);
# the upstream fix is in 1.0.34. With DUCAT_SYSTEM_TKRZW=ON the system library
# found by pkg-config is used instead, after a check for that bug.

option(DUCAT_SYSTEM_TKRZW "Use the system Tkrzw library instead of building it" OFF)

find_package(Threads REQUIRED)
add_library(ducat_tkrzw INTERFACE)

if(DUCAT_SYSTEM_TKRZW)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TKRZW REQUIRED IMPORTED_TARGET tkrzw)

    try_run(tkrzw_remove_run tkrzw_remove_compiled
        SOURCES "${CMAKE_CURRENT_LIST_DIR}/check_tkrzw_remove.cpp"
        CXX_STANDARD 20
        LINK_LIBRARIES PkgConfig::TKRZW
        COMPILE_OUTPUT_VARIABLE tkrzw_remove_compile_output)
    if(NOT tkrzw_remove_compiled)
        message(FATAL_ERROR "Cannot build a test program with the system Tkrzw:\n"
                            "${tkrzw_remove_compile_output}")
    endif()
    if(NOT tkrzw_remove_run EQUAL 0)
        message(FATAL_ERROR "The system Tkrzw ${TKRZW_VERSION} cannot remove records "
                            "(a known bug of Ubuntu's 1.0.32 package). "
                            "Configure with -DDUCAT_SYSTEM_TKRZW=OFF to build Tkrzw from source.")
    endif()

    message(STATUS "Tkrzw: system ${TKRZW_VERSION}")
    target_link_libraries(ducat_tkrzw INTERFACE PkgConfig::TKRZW)
    return()
endif()

include(ExternalProject)

set(DUCAT_TKRZW_VERSION 1.0.34)
set(DUCAT_TKRZW_SHA256 afe894c1532feda086b7a94672f412f801758cbe81431fd3c88514fa27ebe369)
set(tkrzw_source "${CMAKE_BINARY_DIR}/tkrzw")
set(tkrzw_library "${tkrzw_source}/libtkrzw.a")
cmake_host_system_information(RESULT tkrzw_jobs QUERY NUMBER_OF_LOGICAL_CORES)

# The database pages are compressed with Zstandard (libzstd-dev), which makes
# the file some 2.4 times smaller at little cost in speed. Without it Tkrzw is
# built without compression, and cannot read databases made with it.
find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY zstd)
if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    set(tkrzw_options --enable-zstd)
    message(STATUS "Tkrzw: compressing the database with Zstandard")
else()
    set(tkrzw_options)
    message(WARNING "libzstd not found: databases are not compressed, and compressed ones "
                    "cannot be read (install libzstd-dev)")
endif()

# Tkrzw uses autotools. Only the static library is built, and it is used
# straight from the source directory, which also holds the headers.
ExternalProject_Add(tkrzw_project
    URL "https://dbmx.net/tkrzw/pkg/tkrzw-${DUCAT_TKRZW_VERSION}.tar.gz"
    URL_HASH SHA256=${DUCAT_TKRZW_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/tkrzw-build"
    SOURCE_DIR "${tkrzw_source}"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --disable-shared ${tkrzw_options} CXX=${CMAKE_CXX_COMPILER}
    # Builds from scratch after configuring: the Makefile does not rebuild
    # the objects when the options change, e.g. once libzstd is installed.
    BUILD_COMMAND make clean
          COMMAND make -j${tkrzw_jobs} libtkrzw.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${tkrzw_library}"
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON)

# The include directory must exist at configure time for the imported target.
file(MAKE_DIRECTORY "${tkrzw_source}")
add_library(tkrzw_static STATIC IMPORTED)
set_target_properties(tkrzw_static PROPERTIES
    IMPORTED_LOCATION "${tkrzw_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${tkrzw_source}")
add_dependencies(tkrzw_static tkrzw_project)

message(STATUS "Tkrzw: ${DUCAT_TKRZW_VERSION}, built from source")
target_link_libraries(ducat_tkrzw INTERFACE tkrzw_static Threads::Threads)
if(tkrzw_options)
    target_link_libraries(ducat_tkrzw INTERFACE "${ZSTD_LIBRARY}")
endif()
