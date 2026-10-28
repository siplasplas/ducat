# Writes OUTPUT, a header naming the Git commit of SOURCE_DIR and its time,
# run at every build; the header changes only when they do, so that only
# what includes it is rebuilt then.

set(commit "unknown")
set(commit_time "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" log -1
                            "--format=%h%x09%cd" "--date=format:%Y-%m-%d %H:%M"
                    OUTPUT_VARIABLE log RESULT_VARIABLE log_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(log_result EQUAL 0 AND log MATCHES "^([^\t]+)\t(.*)$")
        set(commit "${CMAKE_MATCH_1}")
        set(commit_time "${CMAKE_MATCH_2}")
        # Changes to tracked files not committed yet are built too.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain
                                --untracked-files=no
                        OUTPUT_VARIABLE changes ERROR_QUIET)
        if(NOT changes STREQUAL "")
            string(APPEND commit "+changes")
        endif()
    endif()
endif()

file(WRITE "${OUTPUT}.tmp"
     "#pragma once\n\n"
     "#define DUCAT_COMMIT \"${commit}\"\n"
     "#define DUCAT_COMMIT_TIME \"${commit_time}\"\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
