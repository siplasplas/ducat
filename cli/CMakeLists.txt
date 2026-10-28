add_executable(ducat
    main.cpp
)

target_link_libraries(ducat PRIVATE ducat::libducat)
target_compile_options(ducat PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS ducat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
