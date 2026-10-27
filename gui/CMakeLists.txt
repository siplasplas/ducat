add_executable(ducat-gui
    directory_model.cpp
    directory_model.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
)

set_target_properties(ducat-gui PROPERTIES AUTOMOC ON)
target_link_libraries(ducat-gui PRIVATE ducat::libducat Qt6::Widgets)
target_compile_options(ducat-gui PRIVATE -Wall -Wextra -Wpedantic)
