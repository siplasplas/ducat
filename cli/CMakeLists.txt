add_executable(ducat
    main.cpp
)

target_link_libraries(ducat PRIVATE ducat::libducat)
if(TARGET ducat::archive)
    target_link_libraries(ducat PRIVATE ducat::archive)
    target_compile_definitions(ducat PRIVATE DUCAT_HAVE_ARCHIVES)
endif()
target_compile_options(ducat PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS ducat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
