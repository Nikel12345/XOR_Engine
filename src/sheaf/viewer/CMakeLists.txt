# SheafView — просмотр файлов Sheaf. Линкует только формат, SDL и ImGui: движок ему не нужен.
add_executable(SheafView
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Model.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Views.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Views.h
)
target_link_libraries(SheafView PRIVATE Sheaf ImGui SDL3::SDL3)
set_target_properties(SheafView PROPERTIES FOLDER "sheaf")
add_custom_command(TARGET SheafView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:SheafView>
)
