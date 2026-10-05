# SheafDump — печать Sheaf текстом для git diff (textconv). Только формат, без SDL.
add_executable(SheafDump ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(SheafDump PRIVATE Sheaf)
set_target_properties(SheafDump PROPERTIES FOLDER "sheaf")
