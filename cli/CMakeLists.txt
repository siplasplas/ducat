add_executable(ducat
    main.cpp
)

target_link_libraries(ducat PRIVATE ducat::libducat)
target_compile_options(ducat PRIVATE -Wall -Wextra -Wpedantic)
